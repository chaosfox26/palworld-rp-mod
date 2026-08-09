// chathook.hpp — hook into Palworld's native chat submit path.
//
// THE most patch-fragile code in the project (see CLAUDE.md, "Next feature").
// Everything lives behind install(): if the function is missing or its shape
// has changed after a game update, install() logs why and returns false, and
// the mod carries on with only the ImGui window. Nothing else may depend on
// this hook existing.
//
// Why this is C++ and not Lua: marshalling EPalChatCategory (an EnumProperty)
// into a Lua hook faults inside UE4SS's push_enumproperty — three separate
// in-game crashes traced to it. Here the category is read as a plain byte and
// no Lua marshalling ever happens.

#pragma once

#include <functional>
#include <string>

namespace rp::chathook
{
    // Both callbacks run on the game thread, inside the hooked call.
    //
    //   route(text)  — a recognised slash command typed in the native chat box.
    //                  The original send is suppressed; the text must reach
    //                  Lua's dispatcher (the single command parser).
    //   log(line)    — diagnostics, including the per-send category byte.
    struct Callbacks
    {
        std::function<void(std::string text)> route;
        std::function<void(std::string line)> log;
    };

    // Detour PalPlayerState::EnterChat. Safe to call once, after Unreal init.
    // Returns false (and logs the reason) if the function cannot be found or
    // no longer looks like `(Msg: Text, Category: enum/byte) -> bool`.
    bool install(Callbacks callbacks);

    // Restore the original function pointer. Safe to call if never installed.
    void uninstall();

    bool installed();

    // The set of first tokens (lowercase, no slash) the hook may claim, e.g.
    // "s,say,em,w,who,1,2,...". Comes from Lua — main.lua stays the single
    // source of truth for what is a command. Until this is non-empty the hook
    // claims NOTHING: fail-open, so the native chat always works.
    void setCommands(const std::string& csvLowercase);

    // Step 3: append one line to the game's own chat list by calling
    // WBP_Ingame_Chat's AddChatMessage on the live widget instance.
    //
    // Purely client-side: the PalChatMessage struct goes straight into the
    // local widget and never touches Palworld's network path. GAME THREAD
    // ONLY — call it from the Lua tick, which already runs there.
    //
    // Returns false (logging the reason once, not per message) when the
    // widget is not on screen or the game update changed the shapes; callers
    // fall back to the RP window, which shows every line regardless.
    bool inject(const std::string& senderUtf8, const std::string& messageUtf8, int category);
}
