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

**Never compiled or run.** Nothing has been built on Windows or run against the
live backend. `src/crypto.hpp`, `src/net.cpp` and `src/dllmain.cpp` have never
seen MSVC. The Lua scripts have never executed — no Lua interpreter was available
where they were written.

**Highest-risk spots, in order:**

1. `src/dllmain.cpp` — `on_lua_start`'s signature and `LuaMadeSimple`'s
   `get_*`/`set_*` argument helpers have both changed across UE4SS releases. If
   the mod fails to load, start here.
2. `CMakeLists.txt` — built against UE4SS's own `UE4SS` CMake target per
   `docs/guides/creating-a-c++-mod.md`, not yet confirmed green in CI.
3. `Scripts/json.lua` — unexecuted. Give it a pass before trusting it.
4. `Scripts/main.lua` — the pawn position path (`UEHelpers.GetPlayerController()`,
   `pawn:K2_GetActorLocation()`) needs checking against Palworld specifically.

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

## CI

`.github/workflows/build.yml`, three jobs split by how likely they are to work:

| Job | Needs | Expected |
|---|---|---|
| `host-tests` | g++, node | green |
| `windows-crypto` | MSVC, node — **no UE4SS** | green after the first fixes |
| `windows-dll` | MSVC, CMake, vcpkg, UE4SS source, `EPIC_SUBMODULE_TOKEN` | fails first; `continue-on-error: true` |

The split is deliberate: a UE4SS path detail must not block verification of the
crypto and protocol work. `windows-dll` builds against a full `RE-UE4SS` source
checkout via CMake — read the failing step's log before changing `CMakeLists.txt`
or the workflow, and flip `continue-on-error` to `false` once it goes green.

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
