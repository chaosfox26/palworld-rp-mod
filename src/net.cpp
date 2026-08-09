#include "net.hpp"

#include "b64.hpp"
#include "crypto.hpp"
#include "sio.hpp"

#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <map>

using json = nlohmann::json;

namespace rp
{
    const char* statusName(Status s)
    {
        switch (s)
        {
            case Status::Offline: return "Offline";
            case Status::Authenticating: return "Authenticating";
            case Status::Connecting: return "Connecting";
            case Status::Ready: return "Ready";
            case Status::Reconnecting: return "Reconnecting";
            case Status::Failed: return "Failed";
        }
        return "?";
    }

    namespace
    {
        enum class JobKind
        {
            Auth,
            Chat,
            Position,
            Who,
            Party,
            Rest,
            Logout,
            Shutdown,
        };

        struct Job
        {
            JobKind kind{};
            std::string a, b, c, d;
            double x = 0, y = 0, z = 0;
            bool flag = false;
            bool flag2 = false;
            long long requestId = 0;
        };

        // What we are waiting on an ack for, so the reply can be routed back to the
        // right Lua caller.
        struct PendingAck
        {
            std::string event;     // the Inbound event name to emit
            long long requestId{}; // echoed back to Lua
        };

        std::string trimTrailingSlash(std::string s)
        {
            while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
            return s;
        }

        // https://host -> wss://host/socket.io/?EIO=4&transport=websocket
        //
        // Connecting with transport=websocket directly is what makes this whole
        // approach tractable: Socket.IO v4 accepts a WebSocket-only handshake, so
        // the HTTP long-poll session and its upgrade dance never happen.
        std::string socketUrlFor(const std::string& baseUrl)
        {
            std::string u = trimTrailingSlash(baseUrl);
            if (u.rfind("https://", 0) == 0)
                u = "wss://" + u.substr(8);
            else if (u.rfind("http://", 0) == 0)
                u = "ws://" + u.substr(7);
            return u + "/socket.io/?EIO=4&transport=websocket";
        }
    } // namespace

    struct Net::Impl
    {
        Config cfg;
        crypto::Aes256Gcm aes;

        // Worker
        std::thread worker;
        std::atomic<bool> running{false};
        std::mutex jobMutex;
        std::condition_variable jobCv;
        std::deque<Job> jobs;

        // Coalesced position: only the newest matters.
        std::mutex posMutex;
        bool posDirty = false;
        double px = 0, py = 0, pz = 0;
        std::chrono::steady_clock::time_point lastPositionSend{};

        // Inbound to the game thread
        mutable std::mutex inMutex;
        std::deque<Inbound> inbox;

        // Session
        mutable std::mutex sessionMutex;
        std::string token;
        std::string name;
        std::string password; // kept only to re-login on token expiry
        bool haveCredentials = false;

        // Only 5 concurrent tokens exist per account, and issuing a sixth evicts
        // the OLDEST — so a client that re-logs in a loop logs itself out, and
        // takes the player's other client with it. Counted so we stop instead of
        // churning.
        int consecutiveRelogins = 0;
        static constexpr int kMaxConsecutiveRelogins = 3;

        // Socket
        std::unique_ptr<ix::WebSocket> ws;
        std::atomic<bool> socketOpen{false};
        std::atomic<bool> sioConnected{false};
        std::mutex ackMutex;
        std::map<long long, PendingAck> acks;
        long long nextAckId = 1;
        int reconnectAttempt = 0;

        ix::SocketTLSOptions tls() const
        {
            ix::SocketTLSOptions o;
            if (!cfg.verifyPeer)
            {
                // Deliberately unreachable from the UI. Present only so a
                // developer can bisect a handshake problem, never for players.
                o.caFile = "NONE";
            }
            else if (!cfg.caFile.empty())
            {
                // The installer issues a Caddy-signed cert when the host is behind
                // a router and exports local-ca.crt. Without this every
                // home-hosted server fails verification.
                o.caFile = cfg.caFile;
            }
            else
            {
                o.caFile = "SYSTEM";
            }
            return o;
        }

        void push(const std::string& event, const std::string& jsonText, long long requestId = 0)
        {
            std::lock_guard<std::mutex> lock(inMutex);
            inbox.push_back(Inbound{event, jsonText, requestId});
        }

        void log(const std::string& text)
        {
            push("log", json{{"text", text}}.dump());
        }

        void submit(Job j)
        {
            {
                std::lock_guard<std::mutex> lock(jobMutex);
                jobs.push_back(std::move(j));
            }
            jobCv.notify_one();
        }
    };

    // ------------------------------------------------------------------ lifetime

    Net::Net() : _impl(std::make_unique<Impl>()) {}

    Net::~Net()
    {
        stop();
    }

    bool Net::start(const Config& cfg, const unsigned char* key, size_t keyLen, std::string& err)
    {
        if (_impl->running.load())
        {
            err = "Already started.";
            return false;
        }
        if (cfg.baseUrl.empty())
        {
            err = "Backend URL is not set.";
            return false;
        }
        if (!_impl->aes.init(key, keyLen, err)) return false;

        ix::initNetSystem();

        _impl->cfg = cfg;
        _impl->cfg.baseUrl = trimTrailingSlash(cfg.baseUrl);
        _impl->running.store(true);
        _status.store(Status::Offline);

        Impl* impl = _impl.get();
        Net* self = this;

        _impl->worker = std::thread([impl, self]() {
            ix::HttpClient http(/*async=*/false);
            http.setTLSOptions(impl->tls());

            auto setStatus = [&](Status s) {
                self->_status.store(s);
                impl->push("status", json{{"status", statusName(s)}}.dump());
            };

            auto authHeader = [&]() {
                std::lock_guard<std::mutex> lock(impl->sessionMutex);
                return std::string("Bearer ") + impl->token;
            };

            // ---- REST -------------------------------------------------------

            struct RestResult
            {
                int code = 0;
                std::string body;
                std::string error;
            };

            auto rest = [&](const std::string& method,
                            const std::string& path,
                            const std::string& body,
                            bool authed) -> RestResult {
                const std::string url = impl->cfg.baseUrl + path;
                auto args = http.createRequest(url);
                args->extraHeaders["Content-Type"] = "application/json";
                args->connectTimeout = 10;
                args->transferTimeout = 30;
                if (authed) args->extraHeaders["Authorization"] = authHeader();

                // GET, POST and DELETE are the only verbs the backend exposes.
                std::string verb = ix::HttpClient::kGet;
                if (method == "POST") verb = ix::HttpClient::kPost;
                else if (method == "DELETE") verb = ix::HttpClient::kDelete;

                auto resp = http.request(url, verb, body, args);
                RestResult r;
                r.code = resp->statusCode;
                r.body = resp->body;
                if (!resp->errorMsg.empty()) r.error = resp->errorMsg;
                return r;
            };

            // ---- socket -----------------------------------------------------

            auto sendRaw = [&](const std::string& frame) {
                if (impl->ws && impl->socketOpen.load()) impl->ws->send(frame);
            };

            auto emit = [&](const std::string& event, const json& arg) {
                sendRaw(sio::event(event, arg.dump()));
            };

            auto emitWithAck = [&](const std::string& event,
                                   const json& arg,
                                   const std::string& replyEvent,
                                   long long requestId) {
                long long id;
                {
                    std::lock_guard<std::mutex> lock(impl->ackMutex);
                    id = impl->nextAckId++;
                    impl->acks[id] = PendingAck{replyEvent, requestId};
                }
                sendRaw(sio::event(event, arg.dump(), id));
            };

            // Turn one received chat_receive frame into something Lua can render:
            // decrypt the envelope, and compose the emote line ourselves because
            // the server cannot prepend a name to text it cannot read.
            auto decryptInbound = [&](json& j) {
                if (!j.contains("message")) return;

                auto& m = j["message"];
                if (!m.is_object()) return; // plaintext mode, or already a string

                if (!m.contains("v") || !m.contains("iv") || !m.contains("ct")) return;

                std::string plain, err;
                const std::string iv = m.value("iv", std::string());
                const std::string ct = m.value("ct", std::string());
                if (impl->aes.decryptFromEnvelope(iv, ct, plain, err))
                {
                    j["message"] = plain;
                }
                else
                {
                    j["message"] = std::string("[unreadable: ") + err + "]";
                    j["decryptFailed"] = true;
                }
            };

            auto composeEmote = [&](json& j) {
                // With REQUIRE_ENCRYPTION on, emote frames carry
                // composeWithSender: true and the client renders the name. The
                // name comes from the session token server-side, so it still
                // cannot be spoofed by doing this here.
                if (j.value("type", std::string()) != "emote") return;
                if (!j.value("composeWithSender", false)) return;
                const std::string sender = j.value("sender", std::string());
                const std::string msg = j.value("message", std::string());
                if (!sender.empty()) j["message"] = sender + " " + msg;
            };

            auto onFrame = [&](const std::string& raw) {
                const auto f = sio::parse(raw);
                if (!f.ok)
                {
                    impl->log("Ignored an unparseable frame from the server.");
                    return;
                }

                switch (f.eio)
                {
                    case sio::Eio::Open:
                    {
                        sio::Handshake hs;
                        if (sio::parseHandshake(f.payload, hs))
                        {
                            impl->log("Engine.IO open, sid " + hs.sid);
                        }
                        // Authenticate. This is the wire form of { auth: { token } };
                        // it must not go in the query string, which Caddy logs.
                        json authArg;
                        {
                            std::lock_guard<std::mutex> lock(impl->sessionMutex);
                            authArg["token"] = impl->token;
                        }
                        sendRaw(sio::connect(authArg.dump()));
                        return;
                    }
                    case sio::Eio::Ping:
                        // The server pings in Engine.IO v4; we answer. Getting this
                        // backwards is a silent disconnect every pingTimeout.
                        sendRaw(sio::pong());
                        return;
                    case sio::Eio::Close:
                        impl->log("Server closed the Engine.IO session.");
                        return;
                    case sio::Eio::Message:
                        break;
                    default:
                        return;
                }

                switch (f.sio)
                {
                    case sio::Sio::Connect:
                        impl->sioConnected.store(true);
                        impl->reconnectAttempt = 0;
                        return;

                    case sio::Sio::ConnectError:
                    {
                        std::string message = "unknown";
                        try
                        {
                            const auto j = json::parse(f.payload);
                            message = j.value("message", message);
                        }
                        catch (...)
                        {
                        }
                        impl->log("Socket refused: " + message);
                        if (message.find("unauthorized") != std::string::npos)
                        {
                            // Token expired. Re-login with the stored password and
                            // reconnect, exactly as API.md prescribes.
                            std::lock_guard<std::mutex> lock(impl->sessionMutex);
                            impl->token.clear();
                        }
                        return;
                    }

                    case sio::Sio::Ack:
                    {
                        PendingAck pending;
                        bool found = false;
                        {
                            std::lock_guard<std::mutex> lock(impl->ackMutex);
                            auto it = impl->acks.find(f.ack);
                            if (it != impl->acks.end())
                            {
                                pending = it->second;
                                found = true;
                                impl->acks.erase(it);
                            }
                        }
                        if (!found) return;

                        // Ack payloads are an array of callback arguments; we only
                        // ever pass one.
                        try
                        {
                            auto arr = json::parse(f.payload);
                            json first = arr.is_array() && !arr.empty() ? arr[0] : json::object();
                            impl->push(pending.event, first.dump(), pending.requestId);
                        }
                        catch (...)
                        {
                            impl->push(pending.event,
                                       json{{"ok", false}, {"error", "Malformed reply."}}.dump(),
                                       pending.requestId);
                        }
                        return;
                    }

                    case sio::Sio::Event:
                    {
                        std::string event, arg;
                        if (!sio::splitEvent(f.payload, event, arg)) return;

                        json j = json::object();
                        if (!arg.empty())
                        {
                            try
                            {
                                j = json::parse(arg);
                            }
                            catch (...)
                            {
                                return;
                            }
                        }

                        if (event == "ready")
                        {
                            if (j.contains("limits") && j["limits"].is_object())
                            {
                                const auto& L = j["limits"];
                                if (L.contains("maxMessageBytes"))
                                    self->_maxMessageBytes.store(L.value("maxMessageBytes", 100000));
                                if (L.contains("localChatRadius"))
                                    self->_localChatRadius.store(L.value("localChatRadius", 3000));
                            }
                            {
                                std::lock_guard<std::mutex> lock(impl->sessionMutex);
                                impl->name = j.value("name", impl->name);
                            }
                            setStatus(Status::Ready);

                            // The server starts every new socket at the origin. If
                            // we do not re-send position immediately, /s and /em
                            // resolve against (0,0,0) until the next timer tick —
                            // which means talking to whoever else is near the
                            // origin instead of whoever is near you.
                            {
                                std::lock_guard<std::mutex> lock(impl->posMutex);
                                impl->posDirty = true;
                            }
                        }
                        else if (event == "chat_receive")
                        {
                            decryptInbound(j);
                            composeEmote(j);
                        }
                        else if (event == "force_disconnect")
                        {
                            self->_status.store(Status::Failed);
                        }

                        impl->push(event, j.dump());
                        return;
                    }

                    default:
                        return;
                }
            };

            auto openSocket = [&]() {
                impl->ws = std::make_unique<ix::WebSocket>();
                impl->ws->setUrl(socketUrlFor(impl->cfg.baseUrl));
                impl->ws->setTLSOptions(impl->tls());
                // Engine.IO has its own heartbeat; a second WebSocket-level ping
                // would just add noise.
                impl->ws->setPingInterval(0);
                // We run our own backoff so that a dead token triggers a re-login
                // instead of an endless reconnect against a credential that will
                // never work.
                impl->ws->disableAutomaticReconnection();

                impl->ws->setOnMessageCallback([impl, &onFrame](const ix::WebSocketMessagePtr& msg) {
                    switch (msg->type)
                    {
                        case ix::WebSocketMessageType::Open:
                            impl->socketOpen.store(true);
                            break;
                        case ix::WebSocketMessageType::Message:
                            onFrame(msg->str);
                            break;
                        case ix::WebSocketMessageType::Close:
                            impl->socketOpen.store(false);
                            impl->sioConnected.store(false);
                            break;
                        case ix::WebSocketMessageType::Error:
                            impl->socketOpen.store(false);
                            impl->sioConnected.store(false);
                            impl->log("Socket error: " + msg->errorInfo.reason);
                            break;
                        default:
                            break;
                    }
                });

                setStatus(Status::Connecting);
                impl->ws->start();
            };

            auto closeSocket = [&]() {
                if (impl->ws)
                {
                    impl->ws->stop();
                    impl->ws.reset();
                }
                impl->socketOpen.store(false);
                impl->sioConnected.store(false);
            };

            // ---- server limits ----------------------------------------------
            //
            // Every one of these is mutable at runtime through the admin
            // overrides file. Nothing downstream may cache them.

            auto applyInfo = [&](const std::string& body) {
                try
                {
                    const auto j = json::parse(body);
                    if (j.contains("limits") && j["limits"].is_object())
                    {
                        const auto& L = j["limits"];
                        self->_maxMessageBytes.store(L.value("maxMessageBytes", 100000));
                        self->_maxProfileBytes.store(L.value("maxProfileBytes", 262144));
                        self->_localChatRadius.store(L.value("localChatRadius", 3000));
                        self->_minPasswordLength.store(L.value("minPasswordLength", 8));
                    }
                    if (j.contains("encryption") && j["encryption"].is_object())
                    {
                        self->_requireEncryption.store(j["encryption"].value("required", true));
                    }
                    self->_registrationOpen.store(j.value("registrationOpen", true));

                    // Tell Lua, so the UI and the input box can follow.
                    impl->push("limits",
                               json{{"maxMessageBytes", self->_maxMessageBytes.load()},
                                    {"maxPlaintextBytes", self->maxPlaintextBytes()},
                                    {"transmittableMessageBytes", self->transmittableMessageBytes()},
                                    {"maxProfileBytes", self->_maxProfileBytes.load()},
                                    {"localChatRadius", self->_localChatRadius.load()},
                                    {"minPasswordLength", self->_minPasswordLength.load()},
                                    {"registrationOpen", self->_registrationOpen.load()},
                                    {"requireEncryption", self->_requireEncryption.load()}}
                                   .dump());
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            };

            auto fetchInfo = [&]() {
                const auto info = rest("GET", "/info", "", false);
                if (info.code == 200) return applyInfo(info.body);
                return false;
            };

            auto lastInfoFetch = std::chrono::steady_clock::time_point{};

            // ---- auth -------------------------------------------------------

            auto doAuth = [&](const std::string& user, const std::string& pass, bool registerNew) {
                setStatus(Status::Authenticating);

                // /info first: it carries the real limits and the encryption
                // policy, so the input box is sized from the wire rather than a
                // constant that can drift. Note this is /info, NOT /health —
                // /health is a bare liveness probe and has no limits in it.
                const auto info = rest("GET", "/info", "", false);
                if (info.code == 200)
                {
                    applyInfo(info.body);
                    lastInfoFetch = std::chrono::steady_clock::now();

                    if (registerNew && !self->_registrationOpen.load())
                    {
                        impl->push("chat_error",
                                   json{{"message", "Registration is closed on this server."}}.dump());
                        setStatus(Status::Failed);
                        return;
                    }
                    if ((int)pass.size() < self->_minPasswordLength.load())
                    {
                        impl->push("chat_error",
                                   json{{"message",
                                         "Password must be at least " +
                                             std::to_string(self->_minPasswordLength.load()) +
                                             " characters."}}
                                       .dump());
                        setStatus(Status::Failed);
                        return;
                    }
                }
                else if (info.code == 0)
                {
                    impl->push("chat_error",
                               json{{"message", "Cannot reach the backend: " + info.error}}.dump());
                    setStatus(Status::Failed);
                    return;
                }

                const json body{{"name", user}, {"password", pass}};
                const auto r = rest("POST", registerNew ? "/auth/register" : "/auth/login", body.dump(), false);

                if (r.code == 200 || r.code == 201)
                {
                    try
                    {
                        const auto j = json::parse(r.body);
                        std::lock_guard<std::mutex> lock(impl->sessionMutex);
                        impl->token = j.value("token", std::string());
                        impl->name = j.value("name", user);
                        impl->password = pass;
                        impl->haveCredentials = true;
                        // A fresh token means whatever was wrong is fixed; let
                        // the relogin budget recover.
                        impl->consecutiveRelogins = 0;
                    }
                    catch (...)
                    {
                        impl->push("chat_error", json{{"message", "Server sent a malformed token."}}.dump());
                        setStatus(Status::Failed);
                        return;
                    }
                    closeSocket();
                    openSocket();
                    return;
                }

                std::string message = "Login failed.";
                try
                {
                    const auto j = json::parse(r.body);
                    message = j.value("error", message);
                    // A ban comes back as 403 with a reason the player should see
                    // rather than a generic failure.
                    if (r.code == 403 && j.contains("reason"))
                    {
                        message = "Banned: " + j.value("reason", std::string("no reason given"));
                    }
                }
                catch (...)
                {
                    if (r.code == 0) message = "Cannot reach the backend: " + r.error;
                }
                impl->push("chat_error", json{{"message", message}}.dump());
                setStatus(Status::Failed);
            };

            // Re-login using the stored password. Budgeted, because every call
            // burns one of the account's 5 concurrent tokens and the 6th evicts
            // the oldest — an unbudgeted loop would log the player out of their
            // other client and then out of this one.
            auto relogin = [&]() {
                std::string user, pass;
                {
                    std::lock_guard<std::mutex> lock(impl->sessionMutex);
                    if (!impl->haveCredentials) return false;
                    if (impl->consecutiveRelogins >= Impl::kMaxConsecutiveRelogins)
                    {
                        impl->push(
                            "chat_error",
                            json{{"message",
                                  "Could not re-establish a session after several attempts. Stopping so "
                                  "your other sessions are not evicted. Log in again when ready."}}
                                .dump());
                        self->_status.store(Status::Failed);
                        return false;
                    }
                    ++impl->consecutiveRelogins;
                    user = impl->name;
                    pass = impl->password;
                }
                doAuth(user, pass, false);
                return true;
            };

            // ---- main loop --------------------------------------------------

            auto nextBackoffMs = [&]() {
                const int attempt = impl->reconnectAttempt < 6 ? impl->reconnectAttempt : 6;
                return 1000 * (1 << attempt); // 1s, 2s, 4s ... capped at 64s
            };
            auto reconnectDue = std::chrono::steady_clock::now();

            while (impl->running.load())
            {
                Job job;
                bool haveJob = false;
                {
                    std::unique_lock<std::mutex> lock(impl->jobMutex);
                    impl->jobCv.wait_for(lock, std::chrono::milliseconds(100), [&] {
                        return !impl->jobs.empty() || !impl->running.load();
                    });
                    if (!impl->jobs.empty())
                    {
                        job = std::move(impl->jobs.front());
                        impl->jobs.pop_front();
                        haveJob = true;
                    }
                }

                if (haveJob)
                {
                    switch (job.kind)
                    {
                        case JobKind::Shutdown:
                            impl->running.store(false);
                            break;

                        case JobKind::Auth:
                            doAuth(job.a, job.b, job.flag);
                            break;

                        case JobKind::Logout:
                            rest("POST", "/auth/logout", "", true);
                            closeSocket();
                            {
                                std::lock_guard<std::mutex> lock(impl->sessionMutex);
                                impl->token.clear();
                                impl->password.clear();
                                impl->haveCredentials = false;
                            }
                            setStatus(Status::Offline);
                            break;

                        case JobKind::Chat:
                        {
                            if (!impl->sioConnected.load())
                            {
                                impl->push("chat_error",
                                           json{{"message", "Not connected yet."}}.dump());
                                break;
                            }
                            // Base64 inflates the ciphertext by 4/3 while the
                            // server's transport buffer is only maxMessageBytes +
                            // 65536. An oversized frame is dropped by the
                            // transport as a DISCONNECT with no error, so clamp to
                            // what can actually be transmitted.
                            const int cap = self->transmittableMessageBytes() - 16;
                            if ((int)job.d.size() > cap)
                            {
                                impl->push("chat_error",
                                           json{{"message",
                                                 "That message is " + std::to_string(job.d.size()) +
                                                     " bytes; the limit is " + std::to_string(cap) + "."}}
                                               .dump());
                                break;
                            }

                            std::string envelope, err;
                            if (!impl->aes.encryptToEnvelope(
                                    job.d, (size_t)self->maxMessageBytes(), envelope, err))
                            {
                                impl->push("chat_error", json{{"message", err}}.dump());
                                break;
                            }

                            // The field is `command`, not `type` — chat.js switches
                            // on packet.command.
                            json arg;
                            arg["command"] = job.a;
                            if (!job.b.empty()) arg["target"] = job.b;
                            if (!job.c.empty()) arg["channel"] = job.c;
                            arg["message"] = json::parse(envelope);
                            emit("send_chat", arg);
                            break;
                        }

                        case JobKind::Who:
                            if (impl->sioConnected.load())
                                emitWithAck("who", json{{"query", job.a}}, "who_result", job.requestId);
                            break;

                        case JobKind::Party:
                        {
                            if (!impl->sioConnected.load()) break;
                            if (job.a == "new")
                                emitWithAck("party_create", json::object(), "party_result", job.requestId);
                            else if (job.a == "join")
                                emitWithAck("party_join", json{{"code", job.b}}, "party_result", job.requestId);
                            else if (job.a == "leave")
                                emitWithAck("party_leave", json::object(), "party_result", job.requestId);
                            else
                                emitWithAck("party_info", json::object(), "party_result", job.requestId);
                            break;
                        }

                        case JobKind::Rest:
                        {
                            std::string body = job.c;

                            // Profiles must be envelopes while the server has
                            // REQUIRE_ENCRYPTION on, which is the deployed
                            // default: a plaintext profile body is rejected. The
                            // cap here is maxProfileBytes (262144 by default), NOT
                            // the chat cap — they are different numbers and using
                            // the wrong one silently truncates long profiles.
                            if (job.flag2 && !body.empty())
                            {
                                std::string envelope, err;
                                if (!impl->aes.encryptToEnvelope(
                                        body, (size_t)self->maxProfileBytes(), envelope, err))
                                {
                                    impl->push("rest_result",
                                               json{{"ok", false}, {"status", 0}, {"body", json{{"error", err}}}}
                                                   .dump(),
                                               job.requestId);
                                    break;
                                }
                                body = envelope;
                            }

                            const auto r = rest(job.a, job.b, body, true);
                            json out;
                            out["status"] = r.code;
                            out["ok"] = (r.code >= 200 && r.code < 300);
                            if (!r.error.empty()) out["transportError"] = r.error;
                            try
                            {
                                json parsed = json::parse(r.body);

                                // A profile stored as an envelope comes back
                                // verbatim, so unwrap it here and hand Lua plain
                                // JSON either way. The account name always stays
                                // in clear text — routing and /who depend on it —
                                // so only the `fields` payload is ever sealed.
                                auto unseal = [&](json& node) {
                                    if (!node.is_object()) return;
                                    if (!node.contains("v") || !node.contains("iv") || !node.contains("ct"))
                                        return;
                                    std::string plain, err;
                                    if (impl->aes.decryptFromEnvelope(node.value("iv", std::string()),
                                                                     node.value("ct", std::string()),
                                                                     plain,
                                                                     err))
                                    {
                                        try
                                        {
                                            node = json::parse(plain);
                                        }
                                        catch (...)
                                        {
                                            node = plain;
                                        }
                                    }
                                    else
                                    {
                                        node = json{{"decryptFailed", true}, {"error", err}};
                                    }
                                };

                                unseal(parsed);
                                if (parsed.is_object() && parsed.contains("fields")) unseal(parsed["fields"]);

                                out["body"] = parsed;
                            }
                            catch (...)
                            {
                                out["body"] = r.body;
                            }
                            impl->push("rest_result", out.dump(), job.requestId);

                            // A 401 on any authenticated call means the token aged
                            // out. Re-login silently rather than making the player
                            // retype anything.
                            if (r.code == 401) relogin();
                            break;
                        }

                        case JobKind::Position:
                            // handled below, coalesced
                            break;
                    }
                    continue;
                }

                // Position, rate-limited to the configured rate and only when the
                // value actually changed.
                if (impl->sioConnected.load())
                {
                    const auto now = std::chrono::steady_clock::now();
                    const int hz = impl->cfg.positionHz > 0 ? impl->cfg.positionHz : 3;
                    const auto interval = std::chrono::milliseconds(1000 / hz);
                    bool send = false;
                    double x = 0, y = 0, z = 0;
                    {
                        std::lock_guard<std::mutex> lock(impl->posMutex);
                        if (impl->posDirty && now - impl->lastPositionSend >= interval)
                        {
                            send = true;
                            impl->posDirty = false;
                            impl->lastPositionSend = now;
                            x = impl->px;
                            y = impl->py;
                            z = impl->pz;
                        }
                    }
                    if (send) emit("update_position", json{{"x", x}, {"y", y}, {"z", z}});
                }

                // Re-read the limits. The admin overrides file can change
                // maxMessageBytes, the chat radius, the party size, the password
                // minimum and whether encryption is required, all without a server
                // restart. Fetched here rather than only at login so a long session
                // does not drift.
                {
                    const auto now = std::chrono::steady_clock::now();
                    const auto every = std::chrono::seconds(
                        impl->cfg.infoRefreshSeconds > 0 ? impl->cfg.infoRefreshSeconds : 60);
                    if (self->_status.load() == Status::Ready && now - lastInfoFetch >= every)
                    {
                        lastInfoFetch = now;
                        fetchInfo();
                    }
                }

                // Reconnect with backoff.
                const bool wantSocket = [&] {
                    std::lock_guard<std::mutex> lock(impl->sessionMutex);
                    return impl->haveCredentials;
                }();

                if (wantSocket && !impl->socketOpen.load() && self->_status.load() != Status::Failed)
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= reconnectDue)
                    {
                        bool tokenMissing;
                        {
                            std::lock_guard<std::mutex> lock(impl->sessionMutex);
                            tokenMissing = impl->token.empty();
                        }

                        setStatus(Status::Reconnecting);
                        if (tokenMissing)
                        {
                            relogin();
                        }
                        else
                        {
                            closeSocket();
                            openSocket();
                        }
                        reconnectDue = now + std::chrono::milliseconds(nextBackoffMs());
                        ++impl->reconnectAttempt;
                    }
                }
            }

            closeSocket();
            self->_status.store(Status::Offline);
        });

        return true;
    }

    void Net::stop()
    {
        if (!_impl->running.load())
        {
            if (_impl->worker.joinable()) _impl->worker.join();
            return;
        }
        _impl->submit(Job{JobKind::Shutdown});
        _impl->running.store(false);
        _impl->jobCv.notify_all();
        if (_impl->worker.joinable()) _impl->worker.join();
        _status.store(Status::Offline);
    }

    // ------------------------------------------------------------------ submits

    void Net::submitAuth(const std::string& name, const std::string& password, bool registerNew)
    {
        Job j{JobKind::Auth};
        j.a = name;
        j.b = password;
        j.flag = registerNew;
        _impl->submit(std::move(j));
    }

    void Net::submitChat(const std::string& command,
                         const std::string& target,
                         const std::string& channel,
                         const std::string& message)
    {
        Job j{JobKind::Chat};
        j.a = command;
        j.b = target;
        j.c = channel;
        j.d = message;
        _impl->submit(std::move(j));
    }

    void Net::submitPosition(double x, double y, double z)
    {
        // The server turns non-finite values into 0 and clamps to +/-1e9. Doing
        // it here too means a NaN from a mid-teleport pawn read cannot silently
        // relocate the player to the origin, where they would start hearing local
        // chat from whoever happens to be standing there.
        auto sane = [](double v) {
            if (!std::isfinite(v)) return 0.0;
            if (v > 1e9) return 1e9;
            if (v < -1e9) return -1e9;
            return v;
        };

        std::lock_guard<std::mutex> lock(_impl->posMutex);
        _impl->px = sane(x);
        _impl->py = sane(y);
        _impl->pz = sane(z);
        _impl->posDirty = true;
    }

    void Net::submitWho(const std::string& query, long long requestId)
    {
        Job j{JobKind::Who};
        j.a = query;
        j.requestId = requestId;
        _impl->submit(std::move(j));
    }

    void Net::submitParty(const std::string& action, const std::string& code, long long requestId)
    {
        Job j{JobKind::Party};
        j.a = action;
        j.b = code;
        j.requestId = requestId;
        _impl->submit(std::move(j));
    }

    void Net::submitRest(const std::string& method,
                         const std::string& path,
                         const std::string& bodyJson,
                         long long requestId,
                         bool encryptBody)
    {
        Job j{JobKind::Rest};
        j.a = method;
        j.b = path;
        j.c = bodyJson;
        j.flag2 = encryptBody;
        j.requestId = requestId;
        _impl->submit(std::move(j));
    }

    // Largest maxMessageBytes that can actually cross the wire.
    //
    // The server accepts up to maxMessageBytes of ciphertext, but the ciphertext
    // travels base64-encoded inside a JSON frame, which inflates it by 4/3 — while
    // maxHttpBufferSize is only maxMessageBytes + 65536. Solving
    // 4C/3 + overhead <= C + 65536 gives a ceiling around 196 KB. Past that, a
    // full-size message is dropped by the transport as a disconnect with no error
    // message, which is close to undiagnosable from the client.
    int Net::transmittableMessageBytes() const
    {
        const long long cap = _maxMessageBytes.load();
        const long long buffer = cap + 65536;
        const long long frameOverhead = 512; // event name, command, target, braces

        // Largest C where ceil(C/3)*4 + overhead <= buffer.
        const long long byBuffer = ((buffer - frameOverhead) / 4) * 3;
        return (int)(cap < byBuffer ? cap : byBuffer);
    }

    void Net::submitLogout()
    {
        _impl->submit(Job{JobKind::Logout});
    }

    // -------------------------------------------------------------------- drain

    bool Net::poll(Inbound& out)
    {
        std::lock_guard<std::mutex> lock(_impl->inMutex);
        if (_impl->inbox.empty()) return false;
        out = std::move(_impl->inbox.front());
        _impl->inbox.pop_front();
        return true;
    }

    size_t Net::pending() const
    {
        std::lock_guard<std::mutex> lock(_impl->inMutex);
        return _impl->inbox.size();
    }

    std::string Net::characterName() const
    {
        std::lock_guard<std::mutex> lock(_impl->sessionMutex);
        return _impl->name;
    }
} // namespace rp
