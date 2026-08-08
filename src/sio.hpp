// sio.hpp — the Engine.IO v4 / Socket.IO v5 subset this mod actually needs.
//
// Deliberately free of platform and JSON dependencies so it can be unit tested
// on any compiler. Frames come in as text, go out as text; the caller does the
// JSON.
//
// Wire format, for reference:
//
//   Engine.IO frame  = <digit><payload>
//     0{...}   open      — handshake: sid, pingInterval, pingTimeout, maxPayload
//     1        close
//     2        ping      — the SERVER pings in EIO4; we answer with 3
//     3        pong
//     4<...>   message   — a Socket.IO packet follows
//
//   Socket.IO packet = <digit>[<namespace>,][<ack id>]<json>
//     0  CONNECT        40{"token":"..."}   <- how { auth: { token } } goes on the wire
//     1  DISCONNECT
//     2  EVENT          42["send_chat",{...}]
//     3  ACK            43<id>[{...}]
//     4  CONNECT_ERROR  44{"message":"unauthorized"}
//
// We never negotiate a transport upgrade: connect straight to
//   wss://host/socket.io/?EIO=4&transport=websocket
// and the long-poll handshake never happens.
#pragma once

#include <cstdint>
#include <string>

namespace rp::sio
{
    enum class Eio : int
    {
        Open = 0,
        Close = 1,
        Ping = 2,
        Pong = 3,
        Message = 4,
        Upgrade = 5,
        Noop = 6,
        Unknown = -1,
    };

    enum class Sio : int
    {
        Connect = 0,
        Disconnect = 1,
        Event = 2,
        Ack = 3,
        ConnectError = 4,
        BinaryEvent = 5,
        BinaryAck = 6,
        Unknown = -1,
    };

    struct Frame
    {
        bool ok = false;
        Eio eio = Eio::Unknown;
        Sio sio = Sio::Unknown; // meaningful only when eio == Message
        std::string nsp = "/";
        long long ack = -1; // -1 when the packet carries no ack id
        std::string payload; // raw JSON text, or "" when absent
    };

    // ---------------------------------------------------------------- inbound

    inline Frame parse(const std::string& raw)
    {
        Frame f;
        if (raw.empty()) return f;
        if (raw[0] < '0' || raw[0] > '9') return f;

        f.eio = static_cast<Eio>(raw[0] - '0');

        if (f.eio != Eio::Message)
        {
            f.payload = raw.substr(1);
            f.ok = true;
            return f;
        }

        if (raw.size() < 2 || raw[1] < '0' || raw[1] > '9') return f;
        f.sio = static_cast<Sio>(raw[1] - '0');

        size_t i = 2;

        // Namespace, present only when it is not the default "/".
        if (i < raw.size() && raw[i] == '/')
        {
            const size_t comma = raw.find(',', i);
            if (comma == std::string::npos)
            {
                // A namespace with no payload at all, e.g. "40/admin".
                f.nsp = raw.substr(i);
                f.ok = true;
                return f;
            }
            f.nsp = raw.substr(i, comma - i);
            i = comma + 1;
        }

        // Ack id.
        size_t digitsStart = i;
        while (i < raw.size() && raw[i] >= '0' && raw[i] <= '9') ++i;
        if (i > digitsStart)
        {
            f.ack = 0;
            for (size_t d = digitsStart; d < i; ++d)
            {
                f.ack = f.ack * 10 + (raw[d] - '0');
            }
        }

        f.payload = raw.substr(i);
        f.ok = true;
        return f;
    }

    // Split an EVENT payload `["name", <arg>]` into its name and first argument.
    // Returns false if it does not have that shape. Handles escapes in the name
    // so a hostile server cannot walk us off the end of the buffer.
    inline bool splitEvent(const std::string& payload, std::string& name, std::string& arg)
    {
        name.clear();
        arg.clear();

        size_t i = 0;
        while (i < payload.size() && (payload[i] == ' ' || payload[i] == '\t')) ++i;
        if (i >= payload.size() || payload[i] != '[') return false;
        ++i;
        while (i < payload.size() && (payload[i] == ' ' || payload[i] == '\t')) ++i;
        if (i >= payload.size() || payload[i] != '"') return false;
        ++i;

        for (; i < payload.size(); ++i)
        {
            const char c = payload[i];
            if (c == '\\')
            {
                if (i + 1 >= payload.size()) return false;
                ++i;
                switch (payload[i])
                {
                    case 'n': name += '\n'; break;
                    case 't': name += '\t'; break;
                    case 'r': name += '\r'; break;
                    case 'b': name += '\b'; break;
                    case 'f': name += '\f'; break;
                    case 'u': return false; // event names are ASCII; refuse rather than guess
                    default: name += payload[i]; break;
                }
                continue;
            }
            if (c == '"') break;
            name += c;
        }
        if (i >= payload.size() || payload[i] != '"') return false;
        ++i;

        while (i < payload.size() && (payload[i] == ' ' || payload[i] == '\t')) ++i;

        // No argument: `["ready"]`.
        if (i < payload.size() && payload[i] == ']') return true;
        if (i >= payload.size() || payload[i] != ',') return false;
        ++i;

        // Everything up to the closing bracket is the argument, verbatim. We do
        // not reserialize it, so an encrypted envelope's base64 survives byte for
        // byte and the receiver's GCM tag check still passes.
        size_t end = payload.rfind(']');
        if (end == std::string::npos || end < i) return false;
        arg = payload.substr(i, end - i);

        // Trim surrounding whitespace without touching the interior.
        size_t a = 0;
        while (a < arg.size() && (arg[a] == ' ' || arg[a] == '\t' || arg[a] == '\n')) ++a;
        size_t b = arg.size();
        while (b > a && (arg[b - 1] == ' ' || arg[b - 1] == '\t' || arg[b - 1] == '\n')) --b;
        arg = arg.substr(a, b - a);
        return true;
    }

    // --------------------------------------------------------------- outbound

    inline std::string pong()
    {
        return "3";
    }

    // 40{"token":"..."} — the CONNECT packet carries the auth payload. The
    // backend's io.use() middleware reads it from handshake.auth and refuses a
    // bad token before any event handler runs.
    //
    // Note it deliberately does NOT go in the query string: Caddy logs request
    // URLs, so a token there would sit in plaintext in the access log. The
    // server enforces this by only ever reading handshake.auth.
    inline std::string connect(const std::string& authJson)
    {
        return "40" + authJson;
    }

    inline std::string disconnect()
    {
        return "41";
    }

    // 42["name",<arg>] or 42<ack>["name",<arg>] when an ack is wanted.
    inline std::string event(const std::string& name, const std::string& argJson, long long ackId = -1)
    {
        std::string out = "42";
        if (ackId >= 0) out += std::to_string(ackId);
        out += "[\"";
        out += name;
        out += "\"";
        if (!argJson.empty())
        {
            out += ",";
            out += argJson;
        }
        out += "]";
        return out;
    }

    // Handshake values we care about from the `0{...}` open frame. Parsed with a
    // scan rather than a JSON library so this header stays dependency free; the
    // three fields are all plain integers or a simple string.
    struct Handshake
    {
        std::string sid;
        int pingIntervalMs = 25000;
        int pingTimeoutMs = 20000;
        long long maxPayload = 0;
    };

    inline bool parseHandshake(const std::string& json, Handshake& hs)
    {
        auto number = [&](const char* key, long long& into) {
            const std::string needle = std::string("\"") + key + "\"";
            size_t p = json.find(needle);
            if (p == std::string::npos) return false;
            p = json.find(':', p + needle.size());
            if (p == std::string::npos) return false;
            ++p;
            while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
            bool any = false;
            long long v = 0;
            while (p < json.size() && json[p] >= '0' && json[p] <= '9')
            {
                v = v * 10 + (json[p] - '0');
                ++p;
                any = true;
            }
            if (any) into = v;
            return any;
        };

        size_t p = json.find("\"sid\"");
        if (p == std::string::npos) return false;
        p = json.find('"', json.find(':', p + 5));
        if (p == std::string::npos) return false;
        const size_t end = json.find('"', p + 1);
        if (end == std::string::npos) return false;
        hs.sid = json.substr(p + 1, end - p - 1);
        if (hs.sid.empty()) return false;

        long long v = 0;
        if (number("pingInterval", v)) hs.pingIntervalMs = static_cast<int>(v);
        if (number("pingTimeout", v)) hs.pingTimeoutMs = static_cast<int>(v);
        number("maxPayload", hs.maxPayload);
        return true;
    }
} // namespace rp::sio
