// chathook.cpp — detour on PalPlayerState::EnterChat.
//
// Discovery (CLAUDE.md "Step 1 findings") established:
//
//   EnterChat(Msg: FText, Category: EPalChatCategory) -> bool
//   flags 0x04020401: Native | Public | Final | BlueprintCallable, NOT net.
//
// It runs on the sending client, called from WBP_Ingame_Chat's blueprint when
// the player commits the native chat box. Suppressing it here keeps a message
// off the wire entirely — the whole point: recognised RP commands must never
// reach Palworld's own (plaintext, server-visible) chat.
//
// How the detour works. UE4SS's RegisterHook cannot suppress (its dispatcher
// always calls the original after the pre-callbacks), so we swap the
// UFunction's native func pointer ourselves and become the thunk. A native
// thunk is entered two ways:
//
//   * From Blueprint bytecode: Stack.Code() != null, and the thunk itself must
//     evaluate each parameter expression (StepCompiledIn) and then skip the
//     EX_EndFunctionParms byte. Because evaluating a parameter can have side
//     effects, it must happen exactly once — so on pass-through we do NOT
//     restore the code pointer and re-run the original thunk. Instead we
//     re-enter via ProcessEvent with the already-evaluated buffer:
//
//   * From ProcessEvent: Stack.Code() == null and the parameters are already
//     in Stack.Locals(). We forward straight to the original thunk. This arm
//     serves both our own pass-through re-entry and any other caller.
//
// Decision rule (owner's): text without a leading '/' is NEVER touched — it
// goes to ordinary Palworld chat, so the mod cannot swallow a message meant
// for someone without it installed. Only a leading-slash first token that Lua
// has registered via rp_hook_set_commands is claimed. Unknown slash text
// passes through too.

#include "chathook.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <vector>

#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>
#include <Unreal/FFrame.hpp>
#include <Unreal/FText.hpp>
#include <Unreal/FString.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h> // WideCharToMultiByte

namespace
{
    using namespace RC;
    using namespace RC::Unreal;

    constexpr const CharType* kFunctionPath = STR("/Script/Pal.PalPlayerState:EnterChat");

    UFunction* g_function = nullptr;
    UnrealScriptFunction g_original = nullptr;

    FProperty* g_msgProp = nullptr;      // TextProperty, param 1
    FProperty* g_categoryProp = nullptr; // EnumProperty (uint8 underneath), param 2
    FProperty* g_returnProp = nullptr;   // BoolProperty
    int32_t g_parmsSize = 0;

    rp::chathook::Callbacks g_cb;

    std::mutex g_cmdMutex;
    std::set<std::string> g_commands; // lowercase, no slash

    std::atomic<bool> g_installed{false};

    std::string narrow(const StringType& wide)
    {
        if (wide.empty()) return {};
        const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

    void log(const std::string& line)
    {
        if (g_cb.log) g_cb.log(line);
    }

    // First token after a leading slash, lowercased; empty if no slash.
    std::string slashToken(const std::string& text)
    {
        if (text.size() < 2 || text[0] != '/') return {};
        std::string token;
        for (size_t i = 1; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c == ' ' || c == '\t') break;
            token.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c);
        }
        return token;
    }

    bool isOurs(const std::string& text)
    {
        const std::string token = slashToken(text);
        if (token.empty()) return false;
        std::lock_guard<std::mutex> lock(g_cmdMutex);
        return g_commands.count(token) != 0;
    }

    void detour(UObject* Context, FFrame& Stack, void* RESULT_DECL)
    {
        // ProcessEvent-style call: params already in Locals. This covers our
        // own pass-through re-entry below and any non-Blueprint caller.
        if (!Stack.Code())
        {
            g_original(Context, Stack, RESULT_DECL);
            return;
        }

        // Blueprint call: evaluate the parameters exactly once, the way the
        // real thunk would (same pattern as UE4SS's own hook dispatcher).
        std::vector<uint8_t> parms(static_cast<size_t>(g_parmsSize), 0);
        for (FProperty* prop : TFieldRange<FProperty>(g_function, EFieldIterationFlags::IncludeDeprecated))
        {
            if (!prop->HasAnyPropertyFlags(CPF_Parm) || prop->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
            Stack.MostRecentPropertyAddress() = nullptr;
            Stack.StepCompiledIn(parms.data() + prop->GetOffset_Internal(), prop);
        }
        Stack.Code() += !!Stack.Code(); // P_FINISH: skip EX_EndFunctionParms

        const auto* msg = reinterpret_cast<const FText*>(parms.data() + g_msgProp->GetOffset_Internal());
        const uint8_t category = parms[static_cast<size_t>(g_categoryProp->GetOffset_Internal())];
        const std::string text = narrow(msg->ToString());

        // This line is also the discovery data for the category<->mode map.
        log("[chathook] EnterChat: category=" + std::to_string(category) +
            " len=" + std::to_string(text.size()) +
            (text.size() && text[0] == '/' ? " slash" : " plain"));

        if (isOurs(text))
        {
            // Ours: route to Lua's dispatcher and suppress the original send.
            // Nothing reaches Palworld's chat path. Report success upward —
            // the widget clears its input box either way.
            if (g_cb.route) g_cb.route(text);
            if (RESULT_DECL && g_returnProp) *static_cast<bool*>(RESULT_DECL) = true;
            g_msgProp->DestroyValue(parms.data() + g_msgProp->GetOffset_Internal());
            return;
        }

        // Not ours: hand the evaluated buffer back to the engine. ProcessEvent
        // re-enters this detour with a null Code, which forwards to the real
        // EnterChat. The FText copy-in/out is handled by ProcessEvent itself.
        Context->ProcessEvent(g_function, parms.data());
        if (RESULT_DECL && g_returnProp)
        {
            *static_cast<bool*>(RESULT_DECL) =
                parms[static_cast<size_t>(g_returnProp->GetOffset_Internal())] != 0;
        }
        g_msgProp->DestroyValue(parms.data() + g_msgProp->GetOffset_Internal());
    }
} // namespace

namespace rp::chathook
{
    bool install(Callbacks callbacks)
    {
        if (g_installed.load()) return true;
        g_cb = std::move(callbacks);

        g_function = UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr, kFunctionPath);
        if (!g_function)
        {
            log("[chathook] EnterChat not found - native chat hook disabled, ImGui window unaffected. (Palworld update?)");
            return false;
        }

        // Validate the shape we discovered before touching anything. A game
        // update that changes the signature must land here, not in the detour.
        g_msgProp = nullptr;
        g_categoryProp = nullptr;
        g_returnProp = nullptr;
        int paramIndex = 0;
        bool shapeOk = true;
        for (FProperty* prop : TFieldRange<FProperty>(g_function, EFieldIterationFlags::IncludeDeprecated))
        {
            if (!prop->HasAnyPropertyFlags(CPF_Parm)) continue;
            if (prop->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                g_returnProp = prop;
                continue;
            }
            if (prop->HasAnyPropertyFlags(CPF_OutParm) && !prop->HasAnyPropertyFlags(CPF_ReferenceParm))
            {
                shapeOk = false; // out params would need FOutParmRec plumbing we deliberately do not have
                break;
            }
            const std::string cls = narrow(prop->GetClass().GetName());
            if (paramIndex == 0 && cls == "TextProperty") g_msgProp = prop;
            else if (paramIndex == 1 && (cls == "EnumProperty" || cls == "ByteProperty")) g_categoryProp = prop;
            else shapeOk = false;
            ++paramIndex;
        }
        if (!shapeOk || !g_msgProp || !g_categoryProp || paramIndex != 2)
        {
            log("[chathook] EnterChat exists but its signature changed - hook disabled, ImGui window unaffected.");
            return false;
        }

        g_parmsSize = g_function->GetParmsSize();
        g_original = g_function->GetFuncPtr();
        if (!g_original || g_parmsSize <= 0)
        {
            log("[chathook] EnterChat has no native func pointer - hook disabled.");
            return false;
        }

        g_function->SetFuncPtr(&detour);
        g_installed.store(true);
        log("[chathook] installed on PalPlayerState::EnterChat");
        return true;
    }

    void uninstall()
    {
        if (!g_installed.exchange(false)) return;
        if (g_function && g_original) g_function->SetFuncPtr(g_original);
        g_function = nullptr;
        g_original = nullptr;
    }

    bool installed()
    {
        return g_installed.load();
    }

    void setCommands(const std::string& csvLowercase)
    {
        std::set<std::string> next;
        std::string token;
        for (const char c : csvLowercase + ",")
        {
            if (c == ',')
            {
                if (!token.empty()) next.insert(token);
                token.clear();
            }
            else if (c != ' ')
            {
                token.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c);
            }
        }
        std::lock_guard<std::mutex> lock(g_cmdMutex);
        g_commands.swap(next);
    }
}
