# Palworld RP Mod — Networking Architecture Decision

Decided before code. Backend (v2.3.1) is treated as frozen.

## Decision

**Option 3, refined: a native UE4SS C++ mod owns the wire. The backend does not change.**

| Layer | Owns | Language |
|---|---|---|
| Native DLL (UE4SS C++ mod) | TCP, TLS 1.3, Engine.IO/Socket.IO framing, AES-256-GCM, REST calls, ImGui UI | C++ |
| UE4SS Lua | slash parsing, chat model, config, position sampling, `/who` + profile logic | Lua 5.4 |
| PalSchema | assets only — no networking role | data/blueprints |

Chosen build: **C++ + IXWebSocket + mbedTLS + Windows BCrypt.** UI: **ImGui overlay from the same DLL.**

---

## Why not Lua for the protocol

Socket.IO is not the blocker. Everything underneath it is.

UE4SS Lua has no TCP, no TLS, no CSPRNG, and no AES. There is no way to pin
`TLS_AES_256_GCM_SHA384` from Lua at all — cipher suite selection lives in the TLS
handshake, which Lua never sees. And pure-Lua AES-GCM on a 100 KB message is
roughly 0.1–1 s of work, on the game thread, per message. That is a visible hitch
in the frame graph for every long-form emote.

So a native DLL is mandatory regardless of how the protocol is framed. Once you
are shipping one, putting the protocol in it too is nearly free. Approach 1 is
rejected; approach 3 wins by elimination, not preference.

## Why the backend does not need a plain-WS endpoint

The "Socket.IO has its own handshake" problem mostly evaporates if you skip the
long-poll-then-upgrade dance and connect straight over WebSocket:

```
wss://212-28-190-199.sslip.io/socket.io/?EIO=4&transport=websocket
```

Socket.IO v4 servers accept a WebSocket-only handshake. The subset you then need
is small and stable:

**Engine.IO v4 (first char of each frame)**

- `0{...}` — open. Read `sid`, `pingInterval`, `pingTimeout`.
- `2` — server ping. Reply `3`. (In EIO4 the *server* pings; the client pongs.
  `chat.js` sets `pingInterval: 25000`, `pingTimeout: 20000`.)
- `4<payload>` — a Socket.IO packet follows.

**Socket.IO v5 (second char, after the `4`)**

- Send `40{"token":"<session token>"}` — this is the wire form of
  `{ auth: { token } }`. The handshake middleware in `chat.js` reads
  `socket.handshake.auth.token` and refuses a bad one before any event runs.
- `40{"sid":"..."}` back = connected. `44{...}` = rejected (`unauthorized`,
  `too many connections`).
- `42["event",payload]` — an event in either direction.
- `42<id>["event",payload]` / `43<id>[result]` — the ack pattern. Needed:
  `party_create`, `party_join`, `party_leave`, `party_info`, `who` all take an
  optional ack callback and it is the cleanest way to use them.
- `41` — server-side disconnect.

That is ~150 lines of client code, and it leaves `src/chat.js` and its ATTACK
regression suite untouched. Adding a second transport to the server would mean
refactoring a security-tested router that has explicit tests for identity
spoofing, guild spoofing and channel crosstalk. 150 client lines is the cheaper
risk.

### Escape hatch (build only if needed)

If the framing misbehaves in the field, add a `ws` server at `/ws` on the same
HTTP server that, per connection, opens an internal Socket.IO client to
`127.0.0.1:3000` and pipes JSON frames both directions. ~80 lines, zero edits to
`chat.js`, routing and tests untouched. Keep it in your pocket. Do not build it
first.

## Crypto

**Windows BCrypt** — `BCRYPT_AES_ALGORITHM` with `BCRYPT_CHAIN_MODE_GCM`. Ships
with Windows, no dependency to vendor, microseconds for a 100 KB message.
`BCryptGenRandom` for the 12-byte IV.

The key lives as a byte array in the DLL and encryption/decryption happens
entirely inside it. Lua never receives the key. That does not change your stated
threat model — the key is still extractable from the DLL by anyone who wants it —
but it stops any other Lua mod in the process from reading it out of your Lua
state, which is free to prevent.

**Never reuse an IV.** Fresh random 12 bytes per message. GCM nonce reuse under a
fixed key is a total break, not a degradation.

## TLS

mbedTLS, because the cipher-suite pin is the requirement that constrains the
choice: `mbedtls_ssl_conf_ciphersuites` takes an explicit per-connection list, so
you can offer `TLS1-3-AES-256-GCM-SHA384` and nothing else. Schannel gives you
TLS 1.3 but suite restriction is machine-scoped and awkward.

Also handle the private-CA case: the installer issues a Caddy-signed cert when the
host is behind a router, and exports `local-ca.crt`. The mod needs a setting to
load an extra root, or every home-hosted server fails verification. Do **not** add
a "skip verification" toggle — it will end up on by default in someone's install.

---

## Corrections to the handoff brief

Six things in the brief do not match what `API.md` and `src/chat.js` actually do.

1. **The field is `command`, not `type`.** `{"command":"say","message":...}`.
   `type` only appears on inbound `chat_receive`.

2. **`update_position` is missing from the brief and is mandatory.** `{x,y,z}`, a
   few times a second. Without it every player sits at the origin, and `/s` and
   `/em` reach everyone within 3000 units of `(0,0,0)` — i.e. each other,
   regardless of where they are. Lua must sample the pawn on a timer and re-send
   after every reconnect (`chat.js` resets position to origin on connect).

3. **Slash names differ.** The brief says `/global` and `/c <channel>`; `API.md`
   uses `/gl` and `/1`–`/10`. Purely client-side, so your call — but channels go
   on the wire **by name**, and the slot→name mapping is mod config.

4. **Encrypted emotes are composed client-side.** With `REQUIRE_ENCRYPTION` on,
   the frame carries `composeWithSender: true` and the client renders
   `sender .. " " .. decrypt(message)`. The server cannot prepend a name to text
   it cannot read.

5. **`maxSocketsPerAccount` is enforced at handshake.** Do not open a second
   socket for REST or a second UI panel — the connection is refused with
   `too many connections`.

6. **The real plaintext cap is 99,984 bytes, not 100,000.** `envelope.js` measures
   the limit against the *ciphertext*, which is plaintext + the 16-byte GCM tag.
   Size the input box to 99,984. Two more constraints from the same file:
   - base64 must be **canonical and unwrapped** — the regex rejects whitespace,
     so the common Lua/C base64 snippets that wrap at 76 characters will fail.
   - `iv` must decode to exactly 12 bytes; `ct` to at least 17.

   Headroom check: 100,000 ciphertext bytes is 133,336 base64 chars; the frame
   fits inside `maxHttpBufferSize` (165,536) with ~32 KB spare. Fine, but not
   generous — do not add large sibling fields to the packet.

---

## Verify before writing code

1. UE4SS build in use exposes `on_lua_start` and `UE4SS_ENABLE_IMGUI()` — confirm
   against the version you actually ship for the current Palworld patch.
2. Caddy proxies WebSocket upgrades on `/socket.io/`. Test with a Node client from
   another machine first, so a failure here is not confused with a framing bug.
3. `REQUIRE_ENCRYPTION` state on the live server, and the key from
   `palworld-rp keygen`.
4. `GET /info` limits at runtime, rather than hardcoding them — the mod should size
   its input box from the response.

## Then build, in this order

1. REST only, no socket: `/health` → `/info` → `/auth/register` → `/auth/login`.
   Proves TLS, cert chain and suite pin before any protocol work.
2. AES-GCM round trip in isolation. Encrypt in the DLL, decrypt in Node with the
   same key, byte-compare. Cheapest possible place to find an envelope bug.
3. Socket connect → `40{"token"}` → read `ready`. No chat yet.
4. `update_position` on a timer, then `/s` out and `chat_receive` in. This is the
   loop you asked for.
5. Remaining commands, then `/who` + profile UI.
