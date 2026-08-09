# CLAUDE.md

Context for working on this repo. Read this before changing anything.

## Who you are working with

**The owner is a first-time modder and has never compiled anything.** They wrote
and deployed the backend, so they are not helpless — but the C++ toolchain, git,
and CI are all new to them. `GETTING-STARTED.md` is the guide they are following;
keep it accurate if you change the workflow or the install layout.

What this means in practice:

- Do the toolchain work yourself. Install git and `gh` rather than asking them to.
- Read CI logs with `gh run view --log-failed` rather than asking them to paste
  from the website.
- Explain in terms of what to do next, not what the compiler said. Compiler output
  is your input, not theirs.
- Say when something is expected to fail, before it fails. They have been told the
  first builds will be red; reinforce that rather than letting it look like a
  problem.
- The repo is **private** because it holds the AES key and backend URL. Do not
  make it public, and do not add either to a public place.
- They cannot judge whether an approach is sound, so do not offer them
  architectural choices. Decide, explain the reasoning briefly, and proceed.
  Escalate only when genuinely stuck — and then summarise plainly.

## What this is

The **client** mod for a Palworld roleplay chat system: a UE4SS C++ mod plus Lua
scripts. It talks to an already-deployed backend over that backend's existing
Socket.IO endpoint.

```
Palworld process
├── PalworldRpChat.dll   (C++)   socket · TLS · Socket.IO framing · AES-256-GCM · ImGui window
└── Scripts/*.lua        (Lua)   slash commands · chat model · position · /who
```

## Do not

- **Do not modify or rebuild the backend.** It is finished, deployed, and its
  routing is covered by a security regression suite (path traversal, identity
  spoofing, guild spoofing, channel crosstalk, rate-limit reset by reconnect).
  Repo: https://github.com/chaosfox26/palworld-rp-backend — read only.
- **Do not add a "skip certificate verification" toggle to the UI.** `verifyPeer`
  exists in `Config` for bisecting a handshake problem. If it becomes reachable
  from the UI it will end up on by default in someone's install.
- **`src/key.h` now holds a REAL key, and the repo must stay private.** It was
  verified (base64 and hex agree, 32 bytes) and committed deliberately: the key is
  extractable from the shipped DLL by every player anyway, so a private repo is
  proportionate protection. Making the repo public means regenerating the key,
  rebuilding, and every player reinstalling — git remembers, so deleting it in a
  later commit does not help. Never paste it into an issue, a log, or a Discord
  message.
- **Do not move the key into Lua or a config file.** It stays in `src/key.h` so
  no other Lua mod sharing the process can read it out of a shared Lua state.
- **Do not use a base64 helper that wraps lines.** The server's regex rejects
  whitespace. `src/b64.hpp` never wraps; there is a test for it.
- **Do not describe this as end-to-end encryption**, in code comments, the README,
  or the mod description. Every player has the key. It protects players from the
  host, not from each other.

## Architecture decisions, already settled

`docs/ARCHITECTURE-DECISION.md` has the full reasoning. Summary:

- **Networking is native, not Lua.** UE4SS Lua has no TCP, no TLS, no CSPRNG and
  no AES. Pure-Lua AES-GCM would cost 0.1–1 s per 100 KB message on the game
  thread.
- **The backend keeps Socket.IO; we speak its protocol.** Connect straight to
  `wss://host/socket.io/?EIO=4&transport=websocket` and the long-poll upgrade
  dance never happens. The needed subset is ~150 lines and lives in `src/sio.hpp`.
  An escape hatch — a `ws` bridge on the backend — is documented but deliberately
  not built.
- **AES-256-GCM via Windows BCrypt.** No dependency to vendor.
- **ImGui overlay for the input box**, because the game's chat box caps input and
  UE4SS already exposes ImGui to C++ mods.
- **PalSchema has no networking role.** Reserve it for assets.

### The TLS cipher pin is not achievable — investigated twice, do not retry

The handoff brief asserts "on TLS 1.3 the client picks the cipher suite". That is
not correct, and the brief has repeated it across revisions, so expect to see it
again.

`ix::SocketTLSOptions` has a `ciphers` field but `IXSocketMbedTLS.cpp` never reads
it; it calls `mbedtls_ssl_config_defaults` and never
`mbedtls_ssl_conf_ciphersuites`. Caddy cannot restrict TLS 1.3 suites either
because Go's `crypto/tls` does not expose it. Also: in TLS 1.3 the *server*
selects the suite from the client's offered list, not the client.

It does not matter — content is already AES-256-GCM at the application layer, so
the TLS suite only covers metadata and the password exchange, and AES-128-GCM is
not weak. If the "AES-256 twice over" claim is wanted, vendor ixwebsocket and add
one `mbedtls_ssl_conf_ciphersuites` call (BSD-3, two-line patch).

## The UE4SS build: use CMake, not xmake

`windows-dll` was blocked for 6 CI runs. The diagnosis was correct but the fix is
upstream of it.

**What was happening.** `add_rules("ue4ss.mod")` makes the mod depend on UE4SS's
core target, which drags in all of UE4SS's own packages. One of them, `raw_pdb`,
is requested by git tag `v1.0.0`. Looking at xmake-repo's package definition, a
gitref takes the branch that calls `cmake --install` — but raw_pdb v1.0.0 predates
having an `install()` rule, so `ninja install` dies with "unknown target
'install'". The registered versions (`2024.08.27`, `2022.10.17`) would have taken
a different branch that copies files manually. So it is a genuine upstream
packaging incompatibility, not a runner problem, and no amount of retrying or
pinning the runner image fixes it.

**Why not just patch raw_pdb.** You could ship a local xmake repo overriding that
package. It would probably work. Do not bother, because:

> "Added CMake build system alongside Xmake — CMake >= 3.22 is now supported as
> the **default build system**. Documentation updated to reference CMake build
> instructions. **xmake may be deprecated in the future. Meanwhile, we cannot
> guarantee ABI compatibility.**"
> — RE-UE4SS changelog, PR #1067

Two things follow. The xmake path is being abandoned upstream, so patching it buys
a fix with a shelf life. And more seriously, **a mod built with xmake may be ABI
incompatible with a UE4SS built with CMake** — which is what shipped releases now
are. That is not a build error; it is a mod that loads and then misbehaves or
crashes, which is far worse than a red CI job.

**Direction, in order of preference:**

1. **CMake against the UE4SS source tree.** Now the documented default. Sidesteps
   xmake-repo entirely. `xmake.lua` in this repo becomes dead weight — delete it
   rather than leaving two build files to drift apart.
2. **If CMake still wants the full dependency tree**, skip building UE4SS at all:
   the releases include a **`zDEV`** package intended for mod developers, with the
   headers and import library. Compile our DLL against those with plain CMake or
   `cl`. We have already proven MSVC compiles this codebase — `windows-crypto` is
   green — so this is only a matter of include paths and one import lib.

Do not clone submodules recursively either way. `Re-UE4SS/UEPseudo` currently 404s
upstream, and the submodules exist to build UE4SS itself, which is what we are
trying to avoid doing.

## Epic Games source access is required, and that is settled

UE4SS's build pulls a **private** Unreal-headers submodule hosted in the
`@EpicGames` GitHub organisation. Cloning it needs a GitHub account linked to an
Epic Games account plus an accepted org invite. This is true for CMake and xmake
alike — it is upstream of the build system, so switching build systems does not
avoid it.

**The decision: the owner links the accounts and supplies a token to CI.** Free,
about ten minutes of clicking, done once. `GETTING-STARTED.md` Part 4a walks
through it.

- The token lives in the repo secret **`EPIC_SUBMODULE_TOKEN`** (classic PAT,
  `repo` scope only). `windows-dll` must use it to authenticate the submodule
  clone — rewrite the submodule URL to an authenticated HTTPS form rather than
  trying to use SSH, which needs a key the runner does not have.
- **Never echo the token**, and never let it reach the build log. Mask it, use
  `git config url.<...>.insteadOf`, or pass it via `extraheader`.
- **Do not upload Epic's headers in an artifact and do not commit them.** Using the
  source is fine under the owner's licence; redistributing it is not. The artifact
  ships only our compiled DLL and our Lua scripts.
- The PAT expires. When it does, `windows-dll` starts failing at the clone step —
  that is a token renewal, not a code regression. Say so plainly rather than
  debugging the build.

### Options that were considered and rejected

- **Leave `windows-dll` red and build locally.** This is what a previous session
  recommended, and it is wrong for this owner: they explicitly asked not to have to
  compile anything themselves. Cloud building is the entire reason the CI exists.
  Do not re-propose it.
- **A community/unofficial mirror of the Unreal headers.** Rejected on principle.
  Those mirrors exist to bypass Epic's access control, and the official route is a
  signup form. Do not suggest it again.
- **Dropping the UE4SS C++ API** (standalone Lua C module, or a companion process
  with its own window). Genuinely viable and needs no Epic account — but it trades
  a ten-minute form for two fresh unknowns: whether UE4SS exports the Lua API
  symbols a C module would need, and a replacement for the in-game ImGui window.
  Keep it as the fallback only if the Epic route somehow fails.

## Cache the build — 30 minutes a cycle is the real bottleneck now

The CMake pipeline works, but a from-scratch run is 30+ minutes because nothing is
cached and runners are ephemeral. At three or four more iterations that is hours of
waiting, and it is the single biggest thing slowing Part 4 down. Fix it before
grinding through more compile errors.

Three caches, in order of payoff:

1. **vcpkg binary cache** — biggest win by far. `ixwebsocket` and `nlohmann-json`
   are rebuilt from source every run. Set `VCPKG_BINARY_SOURCES=clear;files,<dir>,readwrite`
   and `actions/cache` that directory, keyed on the hash of `vcpkg.json` plus the
   `builtin-baseline`. Should turn minutes of dependency building into a download.
2. **The RE-UE4SS checkout.** `--depth 1 --recursive --shallow-submodules` is still
   a large clone every run. Cache it keyed on the resolved commit SHA. Careful:
   **the cache must not contain Epic's headers if it could ever be read by anything
   outside this private repo.** Actions caches are scoped to the repo, so this is
   acceptable here — but do not move to a shared or public cache.
3. **`ccache` or `sccache`** for UE4SS's core objects, which dominate compile time
   and never change between our iterations.

If (1) and (2) alone get a cycle under ten minutes, stop there. This is a means to
an end, not a project.

## Two C++ API details that came out of the same changelog

Both are already fixed in `src/dllmain.cpp`; do not undo them.

1. **ImGui setup goes in `on_ui_init()`, never the constructor.** UE4SS's docs say
   `UE4SS_ENABLE_IMGUI()` is intended for `on_ui_init`, and "failing to do so will
   cause a crash when you try to render something with imgui". Using it in a
   constructor separately caused a race (UE4SS #481). `register_tab` moved there
   too.
2. **`ExecuteAsync` and `LoopAsync` are deprecated** in favour of
   `LoopInGameThreadWithDelay`, which runs on the game thread and needs no
   `ExecuteInGameThread` wrapper. `main.lua` prefers the new one and falls back.

Also worth knowing for install instructions: UE4SS #1313 added "support for using
mod name as the main dll name for C++ mods", so `PalworldRpChat.dll` may not need
renaming to `main.dll` on current builds. Verify against the shipped version
rather than assuming either way.

## Next feature: hook Palworld's own chat box

The owner wants to type in the game's native chat bar (the one with `[Global]`,
"Type here...", Tab to change mode) and see RP messages appear in the game's chat
log — not only in the ImGui tab.

**This is a hybrid, not a replacement.** The native box has a small character cap;
that cap is the entire reason the ImGui window exists. Do not remove the ImGui
window and do not route long-form composition through the native box.

| Surface | Job |
|---|---|
| Native chat bar | short lines, quick `/s` `/em` `/w`, and **receiving everything** |
| ImGui window | long-form composition up to the byte cap, profiles, `/who`, config |

### The work, in order

1. **Discovery, in the running game.** Nothing here can be looked up — it has to be
   found. Use UE4SS Live View and object dumps to locate:
   - the widget class behind that chat bar,
   - the UFunction that fires on submit (the one receiving the typed FString),
   - the UFunction that appends a line to the chat log,
   - how the `[Global]` / Tab mode selector is represented.
   Record the exact names found in this file — they are the whole asset here.
2. **Hook submit.** `RegisterHook` on the submit function. Read the text. If it is
   ours (a slash command we own, or an RP mode), route it to `rp_send` and
   **suppress the original call** so it does not also go to Palworld's own chat.
   If it is not ours, let it through untouched.
3. **Inject inbound.** On `chat_receive`, call the game's append-to-log function so
   RP lines appear in the native list. Keep writing them to the ImGui log too.
4. **Mode mapping.** Decide how Palworld's chat modes map to ours, or whether the
   RP scope is selected purely by slash command. Slash-only is simpler and worth
   trying first.

### Step 1 findings (2026-08-08, in-game scan, Steam build)

Found by `RpChatDiscovery` (a discovery-only Lua mod in the game's
`ue4ss\Mods\`, not in this repo).

**Bulk reflection dumping is unavailable in this game — do not retry it.**
Two separate crashes: `DumpAllObjects` (UE4SS's full dump), and a targeted
`ForEachProperty`/`ForEachName` walk over ~10 named objects. The targeted one
got most of the way through before dying (its partial output is where the
signatures below come from), but "most of the way" in a discovery tool means a
crashed game. The safe tools are: the plain `ForEachUObject` name scan
(`GetFullName` only — ran clean three times over ~250k objects), and
**call-time parameter logging** via pass-through `RegisterHook`, which is how
the remaining gaps are being closed. UE4SS's C++ Header Dumper (a different
code path from `DumpAllObjects`) is the untried fallback if those fail.

**The real culprit, from run 4's crash dump — and it is not what the previous
revision of this section said.** Top frames: `UEnum::ForEachName` (Class.hpp:602)
under `LuaType::push_enumproperty` (LuaUObject.cpp:1515), reading address `0x1`.
**`EPalChatCategory`'s UEnum name table is not walkable by this UE4SS build.**
An `EnumProperty` faults *while being marshalled into Lua* — having it in a
hooked function's parameter list is enough; no callback code needs to touch it,
and pcall cannot catch the AV. This one fault explains all three reflection/hook
crashes: the targeted dump died exactly at `EPalChatCategory` (its partial
output stops right before it), and both `EnterChat` hook crashes were the
`Category: EnumProperty` param being pushed. FText was never proven guilty —
earlier suspicion of it is retracted; it is merely untested.

The rules, corrected:

- **Never hook `EnterChat` from Lua** — its `Category` is an `EnumProperty`.
- **Never let any `EPalChatCategory` value cross the Lua boundary** in either
  direction (param, `:get()`, struct field, dump).
- `EnterChat_Receive` carries `Category` as a plain **ByteProperty**, which
  skips `push_enumproperty` entirely — that is the one Lua-reachable source of
  the category numbers, currently being tested. Note it is the client→server
  RPC: it fires on the hosting machine (fine in self-hosted co-op, never on a
  client of a dedicated server).
- If even the byte hook faults, Lua discovery is finished: the categories come
  from a C++ hook, where `EPalChatCategory` is just an integer and none of
  this marshalling exists. **Step 2's production hook belongs in C++
  regardless** — it needs the message text and must not marshal the enum.

**The chat widget** — one live instance in a session:

- `WBP_Ingame_Chat_C` at
  `/Game/Pal/Blueprint/UI/UserInterface/InGame/Chat/WBP_Ingame_Chat`, nested
  under `WBP_PalHUD_InGame_InputListener_C` → `WBP_Ingame_Chat_Wrapper` in the
  HUD tree. Companion widgets: `WBP_Ingame_Chat_Wrapper_C`,
  `WBP_Ingame_ChatListContent_C` (the log list), and
  `WBP_PalChatUIControlOverlay_C` (gamepad chat overlay, has its own
  `SendChat` + `ChangeChatCategory`).

**The submit path**, with recovered signatures (function flags in brackets):

1. Input box is a `PalMultiLineEditableTextBox` inside `WBP_Ingame_Chat`; its
   commit event is bound to `WBP_Ingame_Chat_C:OnTextCommittedInternal`
   (see the `BndEvt__..._OnMultiLineEditableTextBoxCommittedEvent` binding).
2. `PalPlayerState:EnterChat(Msg: FText, Category: EPalChatCategory) -> bool`
   [0x04020401 — BlueprintCallable, Native, Public, Final; **not** a net
   function]. Runs locally on the sending client. **This is the step-2 hook
   point**: suppressing it keeps the message off the wire entirely, and its
   bool return may already mean "accepted" (verify at call time).
3. `PalPlayerController:EnterChat_Receive(Message: FString, Category: byte)`
   [0x00220CC0 — NetServer, reliable]. Despite the name, the flags say this is
   the client→server RPC ("receive" is written from the server's point of
   view). On a dedicated server it never runs on a client.
4. `PalGameStateInGame:BroadcastChatMessage(ChatMessage: PalChatMessage)`
   [0x00024CC0 — NetMulticast, reliable] — server → every client.

**The receive/append path**: `BroadcastChatMessage` multicast →
`PalGameStateInGame.OnRecievedChatMessageDelegate` (sic — Pocketpair's typo) →
`PalUIChat:OnReceivedChat(Message: PalChatMessage)` / `OnFilteredChat(Waiter,
Message)` (word filtering via `PalChatFilteringWaiter`) →
`WBP_Ingame_Chat_C:AddChatMessage`. **Injection point for step 3 is
`AddChatMessage`** on the live widget instance — purely client-side, so an
injected RP line never touches Palworld's own network path.

**The message struct**, `/Script/Pal.PalChatMessage`:

```
Category            EnumProperty    (EPalChatCategory)
Sender              StrProperty
SenderPlayerUId     StructProperty  (FGuid-shaped; verify at call time)
Message             StrProperty
ReceiverPlayerUIds  ArrayProperty
MessageId           NameProperty
MessageArgKeys      ArrayProperty
MessageArgValues    ArrayProperty
```

**Mode selector**: enum `/Script/Pal.EPalChatCategory`;
`WBP_Ingame_Chat_C:ChangeCategory` switches it.

**The Lua byte-hook test (2026-08-08, session 5): crash-safe but no data.**
The `EnterChat_Receive` byte logger installed cleanly and the session ran Say,
Global and Guild sends with zero crashes — but the hook never fired and no
values were logged. Explanation: it is the client→server RPC, so it executes
on the hosting machine, and the session was evidently not self-hosted. Treat
"hook a NetServer RPC client-side" as dead on non-hosted sessions.

**Still pending:** `EPalChatCategory`'s numeric values — now logged by the C++
`chathook` (below) on every send, `AddChatMessage`'s exact parameters (almost
certainly the `PalChatMessage` struct), and `EnterChat`'s return-value
semantics. Also useful: `SendSystemToPlayerChat(WorldContextObject, Message:
FString, ReceiverPlayerUIds: array)` was recovered in full — a fallback
injection route that needs no widget instance.

### Step 2 status: VERIFIED IN GAME (2026-08-08, session 6)

The owner's session confirmed, in one run: backend round trip (registered,
connected as `chaosfox26`, `/s test` came back and rendered), native-box routing
(`/party new` and `/p` reached the RP backend), plain-text pass-through
(three native messages went to ordinary Palworld chat), and no slash text
leaked into Palworld chat on the sender's screen. The remaining formality is
the two-viewer check — a second account confirming a suppressed line never
renders for *others* — worth doing once before calling suppression bulletproof.

**Category observations from that session: the three native chat modes
produced `category = 1, 2, 3`, one per mode, in the order sent.** Which mode
maps to which number is NOT yet recorded — the owner tabbed through modes and
the log line does not name them. Get the pairing confirmed (one question, or
one more send naming the mode) before step 4 bakes it in. `0` is presumably a
system/none value; unobserved.

### Step 2 implementation notes

`src/chathook.cpp` detours `PalPlayerState::EnterChat` by swapping the
UFunction's native func pointer (`GetFuncPtr`/`SetFuncPtr`) — UE4SS's
`RegisterHook` cannot suppress, its dispatcher always calls the original.
The detour handles both entry modes: Blueprint bytecode (steps each param
exactly once via `StepCompiledIn`, because param expressions may have side
effects) and ProcessEvent (params pre-marshalled; also how our own
pass-through re-enters). Compiles clean; **suppression is unverified in
game** — the explicit test is: type `/s test` in the native box while another
account watches; the line must appear in neither player's Palworld chat.

**The plain-text decision (owner's, final):** text without a leading slash in
the native box is never touched — it goes to ordinary Palworld chat, so the
mod can never swallow a message meant for someone without it installed. Only
slash tokens registered by `main.lua` via `rp_hook_set_commands` are claimed;
unknown slash text passes through too. Until Lua registers the list the hook
claims nothing (fail-open). `main.lua` remains the only command parser —
routed text joins the same queue as the ImGui input box.

Every send through the hook logs `category=N` to the RP Chat window's
scrollback — reading those off during the backend round-trip session closes
discovery's last gap. Keybinds: **F9** toggles the RP window (chat mod),
**F7**/**F6** belong to the discovery mod (scan / byte logger).

### Step 3 status: built, not yet run in game

`rp::chathook::inject(sender, message, category)` calls the live
`WBP_Ingame_Chat` instance's `AddChatMessage` via `ProcessEvent` with a
zero-initialised `PalChatMessage` (zeros are valid for every field we don't
set: empty arrays, `NAME_None`, zero guid). `Sender`/`Message` are placement-
new'd FStrings freed afterwards by `DestroyValue`; `Category` is written as a
raw byte. The live instance is found per call (widgets die across level
transitions) by class name, banning CDO/archetype flags and requiring the
`/Engine/Transient` path. Shape checks run once and log a single warning on
mismatch — a Palworld update downgrades injection, never crashes it.

Purely client-side: nothing injected touches Palworld's network path, so
whispers are safe to mirror. Game thread only — it is called from Lua's
`renderChat` on the tick. `config.nativeChat = false` turns it off. Injected
lines use sender label `[RP]` and `NATIVE_CATEGORY = 1` until step 4 does the
real mode mapping.

In-game verification list for the next session: an inbound `/s` renders in
the native chat list; a whisper renders; the line also still appears in the
RP window; with the chat widget closed/absent nothing crashes (inject returns
false); after a level transition it still works (fresh instance lookup).

Also seen and worth knowing: `/Script/Pal.PalCheatManager:SendChatToBroadcast`
and `/Script/Pal.PalUtility:SendSystemToPlayerChat[_Localize]` (a system-line
injection route that may be an alternative to `AddChatMessage` if widget-level
injection misbehaves), and `Engine.GameMode:Say` (stock UE, probably unused by
Palworld).

### Warnings

- **Suppression is the risky part.** If the original call is not properly blocked,
  every RP line is also broadcast to the real game chat — visible to players
  without the mod, and outside the encrypted path. Verify this explicitly.
- **This is the most patch-fragile code in the project.** Function names and
  signatures move when Palworld updates. Isolate it in its own file with a clear
  failure mode: if the hook cannot be installed, log it once and fall back to the
  ImGui window rather than crashing or silently swallowing input.
- **The cap still applies.** A message typed in the native box is limited by that
  box. Do not try to defeat that; that is what the other window is for.

## Backend contract — targeting v2.6.1

`API.md`, `src/chat.js`, `src/rest.js` and `src/envelope.js` in the backend repo
are the ground truth. Everything below was checked against them.

### The one that shapes the design

**No limit may be hardcoded anywhere.** `MAX_MESSAGE_BYTES`, `MAX_PROFILE_BYTES`,
`LOCAL_CHAT_RADIUS`, `MAX_PARTY_SIZE`, `MAX_CHANNELS_PER_ACCOUNT`,
`MIN_PASSWORD_LENGTH`, `REGISTRATION_OPEN`, `REQUIRE_ENCRYPTION` and the chat
budget can all change **while the server is running**, through an admin overrides
file. A client that baked in 100,000 starts getting 413s the day the cap is
lowered.

How this repo handles it: `net.cpp` polls `GET /info` every 60 s, stores the
values in atomics, and emits a `limits` event to Lua. `crypto.hpp` takes the cap
as a **per-call argument** — there is deliberately no usable constant. The ImGui
input buffer resizes when the cap moves. If you add a new size check, plumb the
live value through; do not reintroduce a constant.

### The rest

1. Chat packet field is **`command`**, not `type`. `type` only appears on inbound
   `chat_receive`.
2. **`update_position` `{x,y,z}` is mandatory.** Every socket starts at the origin.
   Without a re-send on `ready`, `/s` and `/em` resolve against `(0,0,0)`. Capped
   at 5/sec, excess silently dropped; non-finite becomes 0, coordinates clamp to
   ±1e9 (we clamp client-side too).
3. **Profiles must be envelopes** while `REQUIRE_ENCRYPTION` is on, which is the
   deployed default. `POST /profile/:name` with a `{v,iv,ct}` body is stored and
   served verbatim. Sized against **maxProfileBytes (262144)**, not the chat cap —
   different numbers. The account *name* stays cleartext; `/who` and routing need
   it. Because the server cannot merge ciphertext, a write replaces the whole
   payload, so `main.lua` caches our own fields and re-sends them all.
4. **Limits are measured against ciphertext.** Usable plaintext is always the cap
   minus the 16-byte tag.
5. **Base64 must be canonical and unwrapped.** `/^[A-Za-z0-9+/]+={0,2}$/`, length a
   multiple of 4, padding required, standard alphabet, no whitespace.
6. **Encrypted emotes are composed client-side** via `composeWithSender: true`,
   using the server-assigned `sender`, never a name from inside the ciphertext.
7. **There is no `type: "whisper"`.** Whispers arrive as `whisper_in`, and are
   echoed to all of the sender's own sockets as `whisper_out`. Render differently.
8. **Two separate caps: 5 concurrent tokens and 8 sockets per account.** Issuing a
   6th token evicts the oldest, so an unbudgeted relogin loop logs the player out
   of their other client and then out of this one. `net.cpp` allows 3 consecutive
   relogins and then stops.
9. **`/health` has no limits in it** — it is `{ok, uptimeSeconds}` only. Limits come
   from `/info`.
10. **A ban is 403 with a `reason` field.** Surface it; a generic "login failed" is
    misleading.
11. **`chat_error` may carry `retryAfterSeconds`.** Honour it rather than spinning.
12. Socket queries (`who`, all `party_*`) share one limiter: 30 burst, 5/sec. The
    chat message counter is charged **before** validation, so malformed packets
    cost the same as real ones.
13. Names `^[a-z0-9][a-z0-9_-]{1,23}$`, channels `^[a-z0-9][a-z0-9_-]{1,31}$`, and
    Windows reserved device names (`con`, `nul`, `com0`–`com9`, `lpt0`–`lpt9`) are
    rejected on every platform. Validated client-side in `main.lua` too.

Socket auth goes in the CONNECT packet — `40{"token":"..."}` — never the query
string, because Caddy logs request URLs.

### A ceiling worth knowing about

`maxHttpBufferSize` is `maxMessageBytes + 65536`, and an oversized frame is dropped
by the transport as a **disconnect with no error message**. But ciphertext travels
base64-encoded, which inflates it by 4/3. Solving `4C/3 + overhead ≤ C + 65536`
puts the real ceiling around **196 KB**.

So if `MAX_MESSAGE_BYTES` is ever raised above ~196,000, a full-size message
silently disconnects the sender. `Net::transmittableMessageBytes()` computes the
lower of the two limits and everything sizes against that. Do not bypass it.

## State

**Verified.** 28,190 protocol checks pass. Base64 is byte-identical to Node's
across all three padding remainders, including a plaintext exactly at the 99,984
cap. Both host test binaries build clean under `-Wall -Wextra -Werror`.

**Compiles and links.** `PalworldRpChat.dll` builds clean against RE-UE4SS
3.0.1.0.0 (`1c1a149`) with MSVC 19.44 — PE32+ x64, exporting `start_mod` and
`uninstall_mod`, with no non-system dependency beyond `UE4SS.dll` and the VC
redist. `src/crypto.hpp` is additionally proven against Node in both directions
by `windows-crypto`.

**Never run.** Nothing has been loaded into Palworld or pointed at the live
backend. The Lua scripts still have never executed — no Lua interpreter was
available where they were written.

**Highest-risk spots, in order:**

1. `Scripts/main.lua` — the pawn position path (`UEHelpers.GetPlayerController()`,
   `pawn:K2_GetActorLocation()`) needs checking against Palworld specifically.
2. `Scripts/json.lua` — unexecuted. Give it a pass before trusting it.
3. `src/dllmain.cpp` — it compiles, but compiling is not loading. `on_lua_start`'s
   signature is still only checked against 3.0.1. If the mod fails to load rather
   than fails to build, start here.

Resolved, do not re-litigate: `LuaMadeSimple::get_string()` returns
`std::string_view` on 3.0.1 and the returned view is **unowned** — the
implementation calls `lua_remove()` before returning it. Every binding copies
into a `std::string` at the boundary. Do not "simplify" those copies away.

## Testing

```bash
bash tests/run.sh          # protocol + base64 interop, needs g++ and node
```

On Windows, and the more valuable one — it proves BCrypt interoperates with Node
in both directions:

```cmd
cd tests
cl /std:c++17 /EHsc /W4 /I..\src test_crypto_win.cpp /Fe:test_crypto_win.exe /link bcrypt.lib
node envelope_interop.js emit | test_crypto_win.exe | node envelope_interop.js verify
```

`tests/envelope_interop.js` re-implements the backend's `envelope.js` validation
rather than importing it. A diff between the two copies is the signal that the
contract moved — keep it that way.

## Building the DLL locally

CI is the reproducible build; this is for iteration, because a red CI cycle
costs 30 minutes and a local rebuild of just our two translation units costs
seconds. The tree lives **outside** this repo so it is never at risk of being
committed:

```
D:\Claude\Test\ue4ss-build\
├── ue4ss\          RE-UE4SS checkout (Epic-derived headers — never commit or upload)
├── mod\            directory junction -> this repo, so edits here are picked up
├── CMakeLists.txt  the same 4-line top-level project CI generates
├── vcpkg.json      the same manifest, same builtin-baseline
└── build.ps1       .\build.ps1 [-Configure] [-Clean]
```

`build.ps1` imports `vcvars64.bat` itself rather than trusting the shell. This
matters: the **Developer PowerShell for VS 2022 shortcut defaults to x86**, and a
32-bit DLL will not load into Palworld. The script hard-fails if the target
architecture is not x64.

Prerequisites beyond VS 2022 Build Tools, both installed once:

- **Rust** (`rustup`, stable, `x86_64-pc-windows-msvc`). UE4SS's `patternsleuth`
  dependency is Rust, driven through `corrosion`. GitHub's runners ship it
  preinstalled, which is why CI never needed a step for it.
- **A full vcpkg clone** at `D:\Claude\Test\vcpkg`. The vcpkg bundled with VS is
  the *tool only* — no port registry, no `versions/` — so it cannot resolve the
  manifest's `builtin-baseline` and is useless here.

Submodule auth is not a problem locally: the owner's `gh` login already has Epic
source access. The submodule URLs are SSH (`git@github.com:`), so rewrite them to
HTTPS for the credential helper. Do this **without touching global git config**:

```powershell
$env:GIT_CONFIG_COUNT=1
$env:GIT_CONFIG_KEY_0='url.https://github.com/.insteadOf'
$env:GIT_CONFIG_VALUE_0='git@github.com:'
git -C ...\ue4ss submodule update --init --recursive --depth 1
```

**Switching the vcpkg triplet needs the stale cache cleared.** `find_package`
results are cached by absolute path, so after changing `VCPKG_TARGET_TRIPLET` a
plain reconfigure still points at the deleted tree and ninja fails with
`'vcpkg_installed/x64-windows/lib/z.lib' ... missing and no known rule to make
it`. Clear the offending entries rather than wiping `build/`, which would cost a
full 30-minute UE4SS rebuild:

```powershell
cmake -B build -S . -U ZLIB_INCLUDE_DIR -U ZLIB_LIBRARY_DEBUG -U ZLIB_LIBRARY_RELEASE -U FIND_PACKAGE_MESSAGE_DETAILS_ZLIB
```

Note the `-S`: without it `cmake -B build` infers the wrong source directory and
fails with a "does not match the source used to generate cache" error instead of
clearing anything.

## Installing a build into the game

UE4SS 3.0.1 resolves a C++ mod's DLL as `dlls/main.dll` **first**, then falls
back to `dlls/<ModName>.dll` (`UE4SS/src/Mod/CppMod.cpp`). So the rename to
`main.dll` is genuinely unnecessary as long as the folder name matches — this is
UE4SS #1313, now confirmed against the shipped version rather than assumed.

```
Pal\Binaries\Win64\ue4ss\Mods\PalworldRpChat\
├── dlls\PalworldRpChat.dll
├── Scripts\main.lua, json.lua
└── enabled.txt          (empty file; or add the name to Mods\mods.txt)
```

## CI

`.github/workflows/build.yml`, three jobs split by how likely they are to work:

| Job | Needs | Expected |
|---|---|---|
| `host-tests` | g++, node | green |
| `windows-crypto` | MSVC, node — **no UE4SS** | green |
| `windows-dll` | MSVC, CMake, vcpkg, UE4SS source, `EPIC_SUBMODULE_TOKEN` | green since run 31290676388; now blocking |

The split is deliberate: a UE4SS path detail must not block verification of the
crypto and protocol work. `windows-dll` builds against a full `RE-UE4SS` source
checkout via CMake. All three jobs are green and `continue-on-error` is off —
a red `windows-dll` is a real regression now (or an expired
`EPIC_SUBMODULE_TOKEN`, which fails at the clone step; that is a token renewal,
not a code problem). A from-scratch run takes ~30 minutes; the caching section
above is the fix if iteration moves back to CI.

## Before the mod can run

Both of these are already done:

1. ~~Generate and install the key~~ — done, verified, in `src/key.h`. A
   `static_assert` now fails the build if it is ever reverted to the placeholder.
2. ~~Set `backendUrl`~~ — `https://212-28-190-199.sslip.io`, already the default in
   `Scripts/main.lua`. `rp-config.example.json` documents every setting.

Still situational: `caFile` is only needed if the backend moves to a home machine
behind a router, where Caddy self-signs instead of using Let's Encrypt.

## Housekeeping on first open

There is a partial `.git/` directory here from an `init` that could not complete
(it was created over a mount that disallows unlink). Clear it and start clean:

```cmd
rmdir /s /q .git
git init && git add -A && git commit -m "initial commit"
```

Then create the GitHub remote and push, which is what makes CI start running.

## Suggested order of work

1. Get `windows-crypto` green. It has no UE4SS dependency, so it is pure signal
   about `crypto.hpp`, and it validates the one thing that is unreadable-in-game
   when wrong.
2. Get `windows-dll` producing a DLL via the CMake pipeline.
3. Fix `on_lua_start` / `LuaMadeSimple` against the shipped UE4SS version.
4. Run `Scripts/json.lua` under a real Lua 5.4 and fix what breaks.
5. Only then test in game: `/health` reachable → login → `/s` → `chat_receive`.
