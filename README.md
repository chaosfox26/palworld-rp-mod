# Palworld RP Chat — client mod

UE4SS C++ mod plus Lua scripts. Talks to
[palworld-rp-backend](https://github.com/chaosfox26/palworld-rp-backend) **v2.6.1**
over its existing Socket.IO endpoint. The backend is unmodified.

**Nothing about the server's limits is hardcoded.** They are mutable at runtime
through the backend's admin overrides file, so the mod re-reads `GET /info` every
60 seconds and sizes everything — the input box, the chat cap, the profile cap —
from the live values.

```
Palworld process
├── PalworldRpChat.dll   (C++)   socket · TLS · Socket.IO framing · AES-256-GCM · ImGui window
└── Scripts/main.lua     (Lua)   slash commands · chat model · position · /who
```

Two queues and a status word are the only shared state. Nothing that blocks runs
on the game thread.

## Layout

| Path | What it is |
|---|---|
| `src/b64.hpp` | canonical base64 — no wrapping, which the server rejects |
| `src/sio.hpp` | Engine.IO v4 / Socket.IO v5 subset; no platform or JSON deps |
| `src/crypto.hpp` | AES-256-GCM over Windows BCrypt |
| `src/key.h` | **edit this** — the baked-in application key |
| `src/net.hpp/.cpp` | worker thread, REST, socket, reconnect, queues |
| `src/dllmain.cpp` | UE4SS mod entry, Lua bindings, ImGui window |
| `Scripts/main.lua` | commands, chat model, position sampling |
| `Scripts/json.lua` | small JSON codec for the Lua side |
| `tests/` | host-side tests that run on any compiler |

## Build

```
xmake f -m Game__Shipping__Win64
xmake
```

Read the comments at the top of `xmake.lua` first. The UE4SS mod rule name and
the mods directory have both changed across releases, so that file is scaffolding
you should expect to adjust; the dependency and syslink lines are the part worth
copying.

Dependencies come from xmake's package manager: `ixwebsocket` (mbedTLS backend)
and `nlohmann_json`. `bcrypt`, `crypt32` and `ws2_32` are Windows system
libraries.

### Before the first build

1. Generate the key on the backend and paste it into `src/key.h`:

   ```
   npx -y github:chaosfox26/palworld-rp-backend keygen
   ```

   It ships as 32 zero bytes on purpose — obviously not a real key, so a build
   that was never edited is easy to spot.

2. Set `backendUrl` in `rp-config.json` (created next to `main.lua` on first run).
   `palworld-rp url` prints it.

3. If the backend runs on a home machine behind a router, Let's Encrypt cannot
   validate it and Caddy issues its own certificate. Set `caFile` to the exported
   `local-ca.crt`. On a public-IP host leave it empty.

## Tests

The two files where a bug is silent rather than loud are covered, and they build
anywhere because they have no platform dependencies:

```
cd tests
g++ -std=c++17 -I../src test_protocol.cpp -o test_protocol && ./test_protocol
g++ -std=c++17 -I../src test_envelope.cpp -o test_envelope
node envelope_interop.js emit | ./test_envelope | node envelope_interop.js check
```

Current state: **28,190 protocol checks pass**, and the envelope interop chain
confirms this mod's base64 is byte-identical to Node's across all three padding
remainders, including a plaintext exactly at the 99,984-byte cap.

`envelope_interop.js` also re-implements `src/envelope.js`'s validation rules
rather than importing them. A diff between the two copies is itself the signal
that the contract moved.

## Two things worth knowing

### The TLS cipher pin is not achievable through ixwebsocket, and does not matter much

`ix::SocketTLSOptions` has a `ciphers` field, but `IXSocketMbedTLS.cpp` never
reads it — it calls `mbedtls_ssl_config_defaults` and never
`mbedtls_ssl_conf_ciphersuites`. So setting `ciphers` there does nothing, and
`TLS_AES_256_GCM_SHA384` cannot be pinned without patching the library.

Two corrections to the original plan, while we are here:

- In TLS 1.3 the **server** selects the cipher suite from the client's offered
  list. The client controls what it offers and in what order, not the final
  choice.
- Caddy cannot restrict TLS 1.3 suites either, because Go's `crypto/tls` does
  not expose that. So the pin is unavailable on both ends.

The practical answer is that it does not matter. Chat content is already wrapped
in AES-256-GCM at the application layer, so the TLS suite only governs the
metadata and the password exchange — and AES-128-GCM is not weak. If the
"AES-256 twice over" claim matters for the mod description, vendor ixwebsocket as
a source subdirectory and add one `mbedtls_ssl_conf_ciphersuites` call; it is
BSD-3 licensed and the patch is two lines. Otherwise leave it.

`verifyPeer` exists in `Config` for bisecting a handshake problem and is
deliberately not reachable from the UI. Do not add a toggle — it will end up on
by default in someone's install.

### Position is client-reported

A modified client can claim to stand anywhere and read local chat from across the
map. Accepted by design: fixing it would need the backend to talk to the game
server, which this architecture exists to avoid. `/s` and `/em` are semi-public.
Say so in the mod description.

Related: the server resets every new socket to the origin, so `main.lua` marks
position dirty on `ready` and re-sends immediately. Without that, `/s` resolves
against `(0,0,0)` until the next timer tick — meaning you talk to whoever is near
the origin instead of whoever is near you.

## Commands

```
/s /say         local, within localChatRadius
/em /e /me      emote, same range
/w <name>       whisper
/p /g /gl       party, guild, global
/c <channel>    a channel you have joined
/1 .. /10       channel slots from rp-config.json
/who <name>     search profiles
/party          new | join <code> | leave
/guild          create | join | leave
/channel        join | leave | list
/mute /unmute   server-side, so it saves your bandwidth too
/profile <k> <v>
```

Bare text with no slash goes to local chat. F9 toggles the window.

## Status

Working: the full connect → register/login → `/s` → `chat_receive` loop, all chat
scopes, parties, guilds, channels, mutes, profiles, `/who`, reconnect with
backoff, and silent re-login on token expiry.

Not yet done:

- Nothing in this project has been compiled on Windows or run against the live
  backend. The host-side tests pass; the Windows-only paths (BCrypt, the UE4SS
  bindings, ImGui) have not executed.
- `Scripts/*.lua` has not been run — no Lua interpreter was available where it was
  written. `json.lua` in particular deserves a pass before you trust it.
- `on_lua_start`'s signature and `LuaMadeSimple`'s argument helpers are the two
  most version-sensitive things here. If the mod fails to load, start there.
- The profile UI is text output in the chat window, not the TRP3-style layout.
- No UMG front end. The Lua chat model is UI-agnostic so one can be added without
  touching command handling.
