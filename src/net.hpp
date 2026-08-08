// net.hpp — the boundary between the game thread and everything that blocks.
//
// Nothing in here may be called from the network side, and nothing on the
// network side touches Lua or Unreal. The only shared state is two queues and a
// status word, all mutex-guarded. That is the whole concurrency design, and
// keeping it that small is deliberate: a network stall must never be able to
// hitch a frame, and a Lua error must never be able to corrupt a socket.
//
// Lua calls submit*() and then drains poll() once per tick. Everything else
// happens on a worker thread and on ixwebsocket's own callback thread.

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rp
{
    enum class Status
    {
        Offline,       // not started, or stopped
        Authenticating, // REST login or register in flight
        Connecting,    // socket opening, handshake not finished
        Ready,         // `ready` received; chat works
        Reconnecting,  // dropped, backing off
        Failed,        // terminal: bad credentials, banned, or a bad config
    };

    const char* statusName(Status s);

    // What the game thread receives. `json` is JSON text, already decrypted where
    // decryption applies, so the Lua side never sees an envelope.
    struct Inbound
    {
        std::string event; // "ready", "chat_receive", "chat_error", "membership",
                           // "party", "who_result", "force_disconnect",
                           // "server_shutdown", "status", "rest_result", "log"
        std::string json;
        long long requestId = 0; // echoes submitRest / submitWho, 0 when unsolicited
    };

    struct Config
    {
        std::string baseUrl;   // https://212-28-190-199.sslip.io  (no trailing slash)
        std::string caFile;    // "" = system store; a path = extra private root
                               // (the installer's local-ca.crt, for home hosts)
        bool verifyPeer = true; // never expose a way to turn this off in the UI

        // The server drops position updates above 5/sec silently, so there is
        // nothing to gain by going faster.
        int positionHz = 3;

        // The admin panel can change limits while the server runs, so they have
        // to be re-read rather than fetched once at login.
        int infoRefreshSeconds = 60;
    };

    class Net
    {
    public:
        Net();
        ~Net();

        Net(const Net&) = delete;
        Net& operator=(const Net&) = delete;

        bool start(const Config& cfg, const unsigned char* key, size_t keyLen, std::string& err);
        void stop();

        Status status() const
        {
            return _status.load();
        }

        // ---- submitted from the game thread, executed on the worker -----------

        // registerNew=false logs in, true registers. Either way the token is kept
        // and the socket opens on success.
        void submitAuth(const std::string& name, const std::string& password, bool registerNew);

        // command is "say" | "emote" | "whisper" | "party" | "guild" | "global" |
        // "channel". target is the whisper recipient, channel the channel name;
        // both ignored when irrelevant. The message is encrypted here, never in Lua.
        void submitChat(const std::string& command,
                        const std::string& target,
                        const std::string& channel,
                        const std::string& message);

        // Coalesced: only the most recent position is ever sent, so a fast Lua
        // timer cannot flood the socket. The server drops excess anyway
        // (rl.positionUpdates), but there is no reason to pay for it.
        void submitPosition(double x, double y, double z);

        void submitWho(const std::string& query, long long requestId);

        // action is "new" | "join" | "leave" | "info".
        void submitParty(const std::string& action, const std::string& code, long long requestId);

        // Generic authenticated REST for profiles, mutes, channels, guilds.
        // Result arrives as an Inbound with event "rest_result" and the same
        // requestId.
        //
        // encryptBody wraps the body in an AES-256-GCM envelope before sending,
        // sized against maxProfileBytes. Profiles MUST use it while the server
        // has REQUIRE_ENCRYPTION on, which is the deployed default: a plaintext
        // profile body is rejected. Envelope-shaped response bodies are decrypted
        // on the way back automatically, so callers always see plain JSON.
        void submitRest(const std::string& method,
                        const std::string& path,
                        const std::string& bodyJson,
                        long long requestId,
                        bool encryptBody = false);

        void submitLogout();

        // ---- drained on the game thread -------------------------------------

        bool poll(Inbound& out);
        size_t pending() const;

        // Server-declared limits, re-read from GET /info on a timer and from the
        // `ready` frame. Never cache these anywhere else: the admin panel can
        // change them mid-session, and a stale copy means silent 413s.
        int maxMessageBytes() const
        {
            return _maxMessageBytes.load();
        }
        int maxPlaintextBytes() const
        {
            return _maxMessageBytes.load() - 16; // minus the GCM tag
        }
        int maxProfileBytes() const
        {
            return _maxProfileBytes.load();
        }
        int localChatRadius() const
        {
            return _localChatRadius.load();
        }
        int minPasswordLength() const
        {
            return _minPasswordLength.load();
        }
        bool registrationOpen() const
        {
            return _registrationOpen.load();
        }

        // The largest maxMessageBytes we can actually transmit. Base64 inflates
        // the ciphertext by 4/3, but the server's maxHttpBufferSize is only
        // maxMessageBytes + 65536 — and an oversized frame is dropped by the
        // transport as a DISCONNECT, with no error message. Above roughly 196 KB
        // the expansion outruns the buffer.
        int transmittableMessageBytes() const;

        std::string characterName() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;

        std::atomic<Status> _status{Status::Offline};

        // Startup fallbacks, replaced by the first /info response.
        std::atomic<int> _maxMessageBytes{100000};
        std::atomic<int> _maxProfileBytes{262144};
        std::atomic<int> _localChatRadius{3000};
        std::atomic<int> _minPasswordLength{8};
        std::atomic<bool> _registrationOpen{true};
        std::atomic<bool> _requireEncryption{true};
    };
} // namespace rp
