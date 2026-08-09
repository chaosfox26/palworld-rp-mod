// dllmain.cpp — the UE4SS C++ mod: Lua bindings plus the ImGui chat window.
//
// This is the most version-sensitive file in the project. Two signatures are
// known to have changed across UE4SS releases and MUST be checked against the
// build you actually ship:
//
//   1. CppUserModBase::on_lua_start(...)  — the parameter list gained
//      main_lua / async_lua / hook_luas over time.
//   2. LuaMadeSimple::Lua::register_function and the get_*/set_* argument
//      helpers.
//
// If the mod fails to load, look here first. Everything else in this project is
// plain C++ and will compile anywhere.
//
// Ownership rules that keep this safe:
//   * Lua owns the chat model. It is the only consumer of Net::poll().
//   * The ImGui window owns no chat state it did not receive from Lua, so there
//     is never a second reader racing the queue.
//   * Text typed in the window goes into a queue Lua drains, rather than being
//     sent directly, so command parsing lives in exactly one place.

#include "chathook.hpp"
#include "key.h"
#include "net.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <Mod/CppUserModBase.hpp>

// CppUserModBase.hpp only forward-declares LuaMadeSimple::Lua (enough for the
// reference parameters in its own signatures); calling member functions like
// register_function/get_string/set_bool needs the full definition.
#include <LuaMadeSimple/LuaMadeSimple.hpp>

// UE4SS_ENABLE_IMGUI() lives here, not in CppUserModBase.hpp.
#include <UE4SSProgram.hpp>

#include <imgui.h>

using namespace RC;

namespace
{
    // ------------------------------------------------------------------ globals

    std::unique_ptr<rp::Net> g_net;

    struct UiState
    {
        std::mutex mutex;

        // Rendered lines, newest last. Bounded so a long session cannot grow
        // without limit.
        std::deque<std::string> log;
        size_t maxLog = 2000;
        bool scrollToBottom = true;

        // Text submitted from the window, waiting for Lua to parse it.
        std::deque<std::string> submitted;

        // The input buffer has to be big enough for the server's real limit.
        // 99,984 plaintext bytes plus a terminator; ImGui writes in place.
        std::vector<char> input;

        // Login form
        char name[32] = {};
        char password[128] = {};

        std::string status = "Offline";
        std::string lastError;
        bool visible = true;
    };

    UiState g_ui;

    void uiLog(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(g_ui.mutex);
        g_ui.log.push_back(line);
        while (g_ui.log.size() > g_ui.maxLog) g_ui.log.pop_front();
        g_ui.scrollToBottom = true;
    }

    // -------------------------------------------------------------- ImGui panel

    void renderChatWindow()
    {
        if (!g_ui.visible) return;

        ImGui::TextUnformatted("Status:");
        ImGui::SameLine();
        {
            std::lock_guard<std::mutex> lock(g_ui.mutex);
            ImGui::TextUnformatted(g_ui.status.c_str());
            if (!g_ui.lastError.empty())
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "(%s)", g_ui.lastError.c_str());
            }
        }

        const rp::Status st = g_net ? g_net->status() : rp::Status::Offline;

        // ---- login form, only while not connected --------------------------
        if (st == rp::Status::Offline || st == rp::Status::Failed)
        {
            ImGui::Separator();
            ImGui::InputText("Character", g_ui.name, sizeof(g_ui.name));
            ImGui::InputText("Password", g_ui.password, sizeof(g_ui.password), ImGuiInputTextFlags_Password);

            // Both of these come from /info and can change while the server runs,
            // so they are read live rather than written into the label.
            const int minPassword = g_net ? g_net->minPasswordLength() : 8;
            ImGui::TextDisabled("Name: lowercase letters, digits, _ and -, 2 to 24 characters.");
            ImGui::TextDisabled("Password: at least %d characters.", minPassword);

            if (ImGui::Button("Log in") && g_net)
            {
                g_net->submitAuth(g_ui.name, g_ui.password, false);
            }
            ImGui::SameLine();

            const bool canRegister = !g_net || g_net->registrationOpen();
            if (!canRegister) ImGui::BeginDisabled();
            if (ImGui::Button("Register") && g_net)
            {
                g_net->submitAuth(g_ui.name, g_ui.password, true);
            }
            if (!canRegister)
            {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(registration is closed on this server)");
            }
            ImGui::SameLine();
            if (ImGui::Button("Forget password"))
            {
                // Do not leave it sitting in process memory once it is not needed.
                std::fill(std::begin(g_ui.password), std::end(g_ui.password), '\0');
            }
        }

        // ---- scrollback ----------------------------------------------------
        ImGui::Separator();
        const float inputHeight = ImGui::GetTextLineHeight() * 5.0f + 40.0f;
        if (ImGui::BeginChild("rp_scrollback",
                              ImVec2(0, -inputHeight),
                              true,
                              ImGuiWindowFlags_HorizontalScrollbar))
        {
            std::lock_guard<std::mutex> lock(g_ui.mutex);
            for (const auto& line : g_ui.log)
            {
                // Wrapped, unformatted: chat text must never be treated as a
                // printf format string.
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopTextWrapPos();
            }
            if (g_ui.scrollToBottom)
            {
                ImGui::SetScrollHereY(1.0f);
                g_ui.scrollToBottom = false;
            }
        }
        ImGui::EndChild();

        // ---- input ---------------------------------------------------------
        //
        // The cap is not a constant. The server's admin overrides file can change
        // maxMessageBytes while we are connected, so the buffer has to follow it
        // rather than being sized once on first render. transmittableMessageBytes
        // accounts for base64 inflation against the transport buffer, which is a
        // lower ceiling than the server's stated limit at large sizes.
        const int cap = g_net ? g_net->transmittableMessageBytes() - 16 : 99984;

        {
            std::lock_guard<std::mutex> lock(g_ui.mutex);
            const size_t want = size_t(cap > 0 ? cap : 1) + 1;
            if (g_ui.input.size() != want)
            {
                const size_t used = g_ui.input.empty()
                                        ? 0
                                        : strnlen(g_ui.input.data(), g_ui.input.size());
                std::string carried(g_ui.input.data(), std::min(used, want - 1));
                g_ui.input.assign(want, '\0');
                std::copy(carried.begin(), carried.end(), g_ui.input.begin());
            }
        }

        // Enter sends, Ctrl+Enter makes a newline.
        //
        // ImGui's default for a multiline box is the opposite, which is wrong for
        // a chat window: the common case is one line and send. Long-form writers
        // still get newlines, just with a modifier.
        const ImGuiInputTextFlags inputFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine;

        const bool enterPressed = ImGui::InputTextMultiline(
            "##rp_input",
            g_ui.input.data(),
            g_ui.input.size(),
            ImVec2(-1, ImGui::GetTextLineHeight() * 4.0f),
            inputFlags);

        const size_t used = strnlen(g_ui.input.data(), g_ui.input.size());
        ImGui::Text("%zu / %d bytes", used, cap);
        ImGui::SameLine();
        ImGui::TextDisabled("|  Enter sends, Ctrl+Enter for a new line");

        const bool ready = (st == rp::Status::Ready);
        const bool canSend = ready && used > 0;

        if (!canSend) ImGui::BeginDisabled();
        const bool buttonPressed = ImGui::Button("Send");
        if (!canSend) ImGui::EndDisabled();

        // A disabled button with no explanation just looks broken. Say why.
        if (!ready)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                               "Log in first — status is %s, and sending needs Ready.",
                               rp::statusName(st));
        }
        else if (used == 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Type something to send.");
        }
        else
        {
            ImGui::SameLine();
            ImGui::TextDisabled("/s /em /w /p /g /gl /1-/10 /who /party");
        }

        if (canSend && (buttonPressed || enterPressed))
        {
            std::lock_guard<std::mutex> lock(g_ui.mutex);
            g_ui.submitted.emplace_back(g_ui.input.data(), used);
            g_ui.input[0] = '\0';
            // Keep focus in the box so a conversation does not need a click
            // between every line.
            ImGui::SetKeyboardFocusHere(-1);
        }
    }

    // ------------------------------------------------------------- Lua bindings
    //
    // Thin on purpose. Every one of these is a queue push or a queue pop; none
    // of them can block the game thread.

    void registerLuaApi(LuaMadeSimple::Lua& lua)
    {
        // rp_start(baseUrl, caFile, positionHz) -> ok, err
        lua.register_function("rp_start", [](const LuaMadeSimple::Lua& lua) -> int {
            rp::Config cfg;
            cfg.baseUrl = lua.get_string();
            cfg.caFile = lua.get_string();
            cfg.positionHz = static_cast<int>(lua.get_integer());

            if (!g_net) g_net = std::make_unique<rp::Net>();

            std::string err;
            const bool ok = g_net->start(cfg, rp::kAppKey, sizeof(rp::kAppKey), err);
            lua.set_bool(ok);
            lua.set_string(err);
            return 2;
        });

        // rp_login(name, password, registerNew)
        lua.register_function("rp_login", [](const LuaMadeSimple::Lua& lua) -> int {
            const std::string name{lua.get_string()};
            const std::string password{lua.get_string()};
            const bool registerNew = lua.get_bool();
            if (g_net) g_net->submitAuth(name, password, registerNew);
            return 0;
        });

        lua.register_function("rp_logout", [](const LuaMadeSimple::Lua&) -> int {
            if (g_net) g_net->submitLogout();
            return 0;
        });

        // rp_send(command, target, channel, message)
        //
        // The message crosses this boundary as plaintext and is encrypted on the
        // far side. The key is never handed to Lua, so no other Lua mod sharing
        // this process can read it out of our state.
        lua.register_function("rp_send", [](const LuaMadeSimple::Lua& lua) -> int {
            const std::string command{lua.get_string()};
            const std::string target{lua.get_string()};
            const std::string channel{lua.get_string()};
            const std::string message{lua.get_string()};
            if (g_net) g_net->submitChat(command, target, channel, message);
            return 0;
        });

        // rp_position(x, y, z) — coalesced; call as often as you like.
        lua.register_function("rp_position", [](const LuaMadeSimple::Lua& lua) -> int {
            const double x = lua.get_number();
            const double y = lua.get_number();
            const double z = lua.get_number();
            if (g_net) g_net->submitPosition(x, y, z);
            return 0;
        });

        // rp_who(query, requestId)
        lua.register_function("rp_who", [](const LuaMadeSimple::Lua& lua) -> int {
            const std::string query{lua.get_string()};
            const auto id = lua.get_integer();
            if (g_net) g_net->submitWho(query, id);
            return 0;
        });

        // rp_party(action, code, requestId)
        lua.register_function("rp_party", [](const LuaMadeSimple::Lua& lua) -> int {
            const std::string action{lua.get_string()};
            const std::string code{lua.get_string()};
            const auto id = lua.get_integer();
            if (g_net) g_net->submitParty(action, code, id);
            return 0;
        });

        // rp_rest(method, path, bodyJson, requestId, encryptBody)
        //
        // encryptBody must be true for profile writes: while the server has
        // REQUIRE_ENCRYPTION on, a plaintext profile body is rejected. Response
        // bodies that arrive as envelopes are decrypted before Lua sees them.
        lua.register_function("rp_rest", [](const LuaMadeSimple::Lua& lua) -> int {
            const std::string method{lua.get_string()};
            const std::string path{lua.get_string()};
            const std::string body{lua.get_string()};
            const auto id = lua.get_integer();
            const bool encryptBody = lua.get_bool();
            if (g_net) g_net->submitRest(method, path, body, id, encryptBody);
            return 0;
        });

        // rp_poll() -> event, json, requestId   (nil when the queue is empty)
        lua.register_function("rp_poll", [](const LuaMadeSimple::Lua& lua) -> int {
            rp::Inbound in;
            if (!g_net || !g_net->poll(in))
            {
                lua.set_nil();
                return 1;
            }
            lua.set_string(in.event);
            lua.set_string(in.json);
            lua.set_integer(in.requestId);
            return 3;
        });

        // rp_status() -> status, sendableBytes, localChatRadius, characterName,
        //                maxProfileBytes, minPasswordLength
        //
        // sendableBytes already accounts for base64 inflation against the
        // transport buffer, so Lua can compare a message length against it
        // directly.
        lua.register_function("rp_status", [](const LuaMadeSimple::Lua& lua) -> int {
            const rp::Status st = g_net ? g_net->status() : rp::Status::Offline;
            lua.set_string(rp::statusName(st));
            lua.set_integer(g_net ? g_net->transmittableMessageBytes() - 16 : 99984);
            lua.set_integer(g_net ? g_net->localChatRadius() : 3000);
            lua.set_string(g_net ? g_net->characterName() : std::string());
            lua.set_integer(g_net ? g_net->maxProfileBytes() : 262144);
            lua.set_integer(g_net ? g_net->minPasswordLength() : 8);
            return 6;
        });

        // ---- UI plumbing --------------------------------------------------

        // rp_ui_log(line) — Lua formats, the window displays.
        lua.register_function("rp_ui_log", [](const LuaMadeSimple::Lua& lua) -> int {
            uiLog(std::string{lua.get_string()});
            return 0;
        });

        // rp_ui_take_input() -> text or nil. Text typed in the window arrives
        // here so Lua's slash parser stays the only command parser.
        lua.register_function("rp_ui_take_input", [](const LuaMadeSimple::Lua& lua) -> int {
            std::lock_guard<std::mutex> lock(g_ui.mutex);
            if (g_ui.submitted.empty())
            {
                lua.set_nil();
                return 1;
            }
            lua.set_string(g_ui.submitted.front());
            g_ui.submitted.pop_front();
            return 1;
        });

        lua.register_function("rp_ui_set_status", [](const LuaMadeSimple::Lua& lua) -> int {
            const std::string status{lua.get_string()};
            const std::string err{lua.get_string()};
            std::lock_guard<std::mutex> lock(g_ui.mutex);
            g_ui.status = status;
            g_ui.lastError = err;
            return 0;
        });

        lua.register_function("rp_ui_toggle", [](const LuaMadeSimple::Lua&) -> int {
            g_ui.visible = !g_ui.visible;
            return 0;
        });

        // ---- native chat hook -------------------------------------------

        // rp_hook_set_commands(csv) — main.lua tells the hook which slash
        // tokens it owns. Until called, the hook claims nothing (fail-open),
        // so Palworld's chat can never lose a message to a half-loaded mod.
        lua.register_function("rp_hook_set_commands", [](const LuaMadeSimple::Lua& lua) -> int {
            rp::chathook::setCommands(std::string{lua.get_string()});
            return 0;
        });

        // rp_hook_status() -> bool — lets /help say whether the native box works.
        lua.register_function("rp_hook_status", [](const LuaMadeSimple::Lua& lua) -> int {
            lua.set_bool(rp::chathook::installed());
            return 1;
        });
    }
} // namespace

class PalworldRpMod : public CppUserModBase
{
public:
    PalworldRpMod()
    {
        ModName = STR("PalworldRpChat");
        ModVersion = STR("0.1.0");
        ModDescription = STR("Roleplay chat: local, emote, whisper, party, guild, global, channels.");
        ModAuthors = STR("chaosfox26");

        // NOTHING ImGui-RELATED GOES IN THE CONSTRUCTOR.
        //
        // UE4SS fixed "a crash caused by a race condition enabled by C++ mods
        // using UE4SS_ENABLE_IMGUI in their constructor" (#481), and the UI is not
        // initialised this early anyway. Both the macro and register_tab belong in
        // on_ui_init(). See below.
    }

    ~PalworldRpMod() override
    {
        // Put EnterChat's real function pointer back before the DLL unloads,
        // or the game calls into freed code on the next chat message.
        rp::chathook::uninstall();

        // Join the worker before the DLL unloads, or its thread outlives the code
        // it is running.
        if (g_net) g_net->stop();
        g_net.reset();
    }

    void on_unreal_init() override
    {
        uiLog("RP chat loaded. Open the RP Chat tab, then log in.");

        // Native chat-box hook. Failure is a logged degradation, not an error:
        // everything works from the ImGui window without it.
        rp::chathook::install({
            // route: recognised slash text from the native box joins the same
            // queue the ImGui input uses, so Lua's dispatcher stays the only
            // command parser.
            [](std::string text) {
                std::lock_guard<std::mutex> lock(g_ui.mutex);
                g_ui.submitted.push_back(std::move(text));
            },
            // log: to the ImGui scrollback, where the category byte values
            // will be read off during the next in-game session.
            [](std::string line) { uiLog(line); },
        });
    }

    // The only correct place for ImGui setup.
    //
    // UE4SS docs are explicit: UE4SS_ENABLE_IMGUI() is intended to be used here,
    // and "failing to do so will cause a crash when you try to render something
    // with imgui". Calling it in the constructor is separately known to race
    // (fixed in UE4SS #481, but the fix was to stop doing it).
    void on_ui_init() override
    {
        UE4SS_ENABLE_IMGUI();

        // UE4SS ships ImGui and exposes it to C++ mods, so the DLL we needed for
        // the socket also gives us the input box the game's own chat cannot
        // provide.
        register_tab(STR("RP Chat"), [](CppUserModBase*) { renderChatWindow(); });
    }

    // NOTE: verify this signature against your UE4SS version — it has changed.
    void on_lua_start(StringViewType,
                      LuaMadeSimple::Lua& lua,
                      LuaMadeSimple::Lua& main_lua,
                      LuaMadeSimple::Lua& async_lua,
                      std::vector<LuaMadeSimple::Lua*>& hook_luas) override
    {
        // Register into every Lua state the scripts might run on, or a call from
        // an async callback finds the function missing.
        registerLuaApi(lua);
        registerLuaApi(main_lua);
        registerLuaApi(async_lua);
        for (auto* hook : hook_luas)
        {
            if (hook) registerLuaApi(*hook);
        }
    }
};

#define PALWORLD_RP_API __declspec(dllexport)

extern "C"
{
    PALWORLD_RP_API CppUserModBase* start_mod()
    {
        return new PalworldRpMod();
    }

    PALWORLD_RP_API void uninstall_mod(CppUserModBase* mod)
    {
        delete mod;
    }
}
