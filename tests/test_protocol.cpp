// Unit tests for the platform-free halves of the mod: base64 canonicality and
// Engine.IO/Socket.IO framing. Builds anywhere:
//
//   g++ -std=c++17 -I../src test_protocol.cpp -o test_protocol && ./test_protocol
//
// These are the two places a bug is silent rather than loud. A wrapped base64
// string is rejected by the server with a generic validation error, and a
// mis-split frame just looks like "the mod doesn't receive messages".

#include "b64.hpp"
#include "sio.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

// Render either side of a comparison for the failure message, whatever it is.
static std::string show(const std::string& s) { return s; }
[[maybe_unused]] static std::string show(const char* s) { return s ? std::string(s) : std::string("(null)"); }
template <typename T> static std::string show(const T& v) { return std::to_string(v); }

#define CHECK(cond)                                                                      \
    do                                                                                   \
    {                                                                                    \
        ++checks;                                                                        \
        if (!(cond))                                                                     \
        {                                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                   \
            ++failures;                                                                   \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                   \
    do                                                                                   \
    {                                                                                    \
        ++checks;                                                                        \
        if (!((a) == (b)))                                                               \
        {                                                                                \
            std::printf("FAIL %s:%d  %s != %s\n", __FILE__, __LINE__, #a, #b);            \
            std::printf("     left  = [%s]\n", show(a).c_str());                          \
            std::printf("     right = [%s]\n", show(b).c_str());                          \
            ++failures;                                                                   \
        }                                                                                \
    } while (0)

static std::vector<uint8_t> bytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

// --------------------------------------------------------------------- base64

static void test_base64_known_vectors()
{
    // RFC 4648 section 10.
    CHECK_EQ(rp::b64::encode(bytes("")), std::string(""));
    CHECK_EQ(rp::b64::encode(bytes("f")), std::string("Zg=="));
    CHECK_EQ(rp::b64::encode(bytes("fo")), std::string("Zm8="));
    CHECK_EQ(rp::b64::encode(bytes("foo")), std::string("Zm9v"));
    CHECK_EQ(rp::b64::encode(bytes("foob")), std::string("Zm9vYg=="));
    CHECK_EQ(rp::b64::encode(bytes("fooba")), std::string("Zm9vYmE="));
    CHECK_EQ(rp::b64::encode(bytes("foobar")), std::string("Zm9vYmFy"));
}

static void test_base64_never_wraps()
{
    // The failure mode the backend's regex catches: a 76-column wrap. 3 KB of
    // input is 40+ lines under a wrapping encoder.
    std::vector<uint8_t> big(3000, 0xAB);
    const std::string enc = rp::b64::encode(big);
    CHECK(enc.find('\n') == std::string::npos);
    CHECK(enc.find('\r') == std::string::npos);
    CHECK(enc.find(' ') == std::string::npos);
    CHECK_EQ(enc.size(), size_t(4000));
}

static void test_base64_matches_server_regex()
{
    // /^[A-Za-z0-9+/]+={0,2}$/ plus length % 4 == 0.
    for (size_t len = 1; len <= 200; ++len)
    {
        std::vector<uint8_t> v(len);
        for (size_t i = 0; i < len; ++i) v[i] = uint8_t((i * 37 + 11) & 0xFF);
        const std::string enc = rp::b64::encode(v);

        CHECK(enc.size() % 4 == 0);

        size_t pad = 0;
        while (pad < enc.size() && enc[enc.size() - 1 - pad] == '=') ++pad;
        CHECK(pad <= 2);

        for (size_t i = 0; i + pad < enc.size(); ++i)
        {
            const char c = enc[i];
            const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9') || c == '+' || c == '/';
            CHECK(valid);
        }

        CHECK_EQ(rp::b64::decodedLength(enc), (long long)len);
    }
}

static void test_base64_iv_is_16_chars()
{
    // A 12-byte GCM IV must encode to exactly 16 base64 characters, and
    // envelope.js requires decodedLength(iv) == 12 exactly.
    std::vector<uint8_t> iv(12, 0x5A);
    const std::string enc = rp::b64::encode(iv);
    CHECK_EQ(enc.size(), size_t(16));
    CHECK_EQ(rp::b64::decodedLength(enc), 12LL);
}

static void test_base64_ciphertext_budget()
{
    // The server measures its 100,000 byte limit against the CIPHERTEXT, which
    // is plaintext + a 16-byte GCM tag. So the real plaintext cap is 99,984.
    const size_t maxMessageBytes = 100000;
    const size_t tag = 16;
    const size_t maxPlaintext = maxMessageBytes - tag;
    CHECK_EQ(maxPlaintext, size_t(99984));

    // And that ciphertext must still fit inside maxHttpBufferSize.
    const size_t b64Len = ((maxMessageBytes + 2) / 3) * 4;
    CHECK_EQ(b64Len, size_t(133336));
    const size_t maxHttpBufferSize = maxMessageBytes + 65536;
    CHECK(b64Len < maxHttpBufferSize);
}

static void test_base64_round_trip()
{
    for (size_t len = 0; len <= 300; ++len)
    {
        std::vector<uint8_t> v(len);
        for (size_t i = 0; i < len; ++i) v[i] = uint8_t((i * 91 + 7) & 0xFF);
        const std::string enc = rp::b64::encode(v);
        std::vector<uint8_t> back;
        if (len == 0)
        {
            CHECK(!rp::b64::decode(enc, back)); // empty is not a valid envelope field
            continue;
        }
        CHECK(rp::b64::decode(enc, back));
        CHECK(back == v);
    }
}

static void test_base64_rejects_junk()
{
    std::vector<uint8_t> out;
    CHECK(!rp::b64::decode("", out));                 // empty
    CHECK(!rp::b64::decode("Zm9", out));              // not a multiple of 4
    CHECK(!rp::b64::decode("Zm9v\n", out));           // trailing newline
    CHECK(!rp::b64::decode("Zm9 v", out));            // interior space
    CHECK(!rp::b64::decode("Zm9v Zm9v", out));        // wrapped-style
    CHECK(!rp::b64::decode("Zm=v", out));             // padding in the middle
    CHECK(!rp::b64::decode("Zm9*", out));             // outside the alphabet
    CHECK(!rp::b64::decode("Zm9-", out));             // url-safe alphabet is not accepted
    CHECK_EQ(rp::b64::decodedLength("Zm9"), -1LL);
}

// ------------------------------------------------------------------ Engine.IO

static void test_parse_open()
{
    const std::string raw =
        "0{\"sid\":\"lv_VI97HAXNKbyyw\",\"upgrades\":[],\"pingInterval\":25000,"
        "\"pingTimeout\":20000,\"maxPayload\":165536}";
    const auto f = rp::sio::parse(raw);
    CHECK(f.ok);
    CHECK(f.eio == rp::sio::Eio::Open);

    rp::sio::Handshake hs;
    CHECK(rp::sio::parseHandshake(f.payload, hs));
    CHECK_EQ(hs.sid, std::string("lv_VI97HAXNKbyyw"));
    // These are the values src/chat.js actually configures.
    CHECK_EQ(hs.pingIntervalMs, 25000);
    CHECK_EQ(hs.pingTimeoutMs, 20000);
    CHECK_EQ(hs.maxPayload, 165536LL);
}

static void test_parse_ping_and_pong()
{
    const auto ping = rp::sio::parse("2");
    CHECK(ping.ok);
    CHECK(ping.eio == rp::sio::Eio::Ping);
    // In Engine.IO v4 the server pings and the client answers. Getting this
    // backwards means a silent disconnect every pingTimeout.
    CHECK_EQ(rp::sio::pong(), std::string("3"));
}

static void test_parse_connect_ok_and_error()
{
    const auto ok = rp::sio::parse("40{\"sid\":\"abc123\"}");
    CHECK(ok.ok);
    CHECK(ok.eio == rp::sio::Eio::Message);
    CHECK(ok.sio == rp::sio::Sio::Connect);
    CHECK_EQ(ok.nsp, std::string("/"));
    CHECK_EQ(ok.payload, std::string("{\"sid\":\"abc123\"}"));

    // What a rejected token looks like coming back from io.use().
    const auto bad = rp::sio::parse("44{\"message\":\"unauthorized\"}");
    CHECK(bad.ok);
    CHECK(bad.sio == rp::sio::Sio::ConnectError);
    CHECK(bad.payload.find("unauthorized") != std::string::npos);

    const auto full = rp::sio::parse("44{\"message\":\"too many connections\"}");
    CHECK(full.sio == rp::sio::Sio::ConnectError);
}

static void test_parse_event()
{
    const auto f = rp::sio::parse(
        "42[\"chat_receive\",{\"type\":\"say\",\"sender\":\"chaosfox26\",\"at\":1738900000000}]");
    CHECK(f.ok);
    CHECK(f.sio == rp::sio::Sio::Event);
    CHECK_EQ(f.ack, -1LL);

    std::string name, arg;
    CHECK(rp::sio::splitEvent(f.payload, name, arg));
    CHECK_EQ(name, std::string("chat_receive"));
    CHECK_EQ(arg, std::string("{\"type\":\"say\",\"sender\":\"chaosfox26\",\"at\":1738900000000}"));
}

static void test_parse_event_with_no_argument()
{
    const auto f = rp::sio::parse("42[\"server_shutdown\"]");
    std::string name, arg;
    CHECK(rp::sio::splitEvent(f.payload, name, arg));
    CHECK_EQ(name, std::string("server_shutdown"));
    CHECK(arg.empty());
}

static void test_parse_ack()
{
    // party_create with an ack callback comes back as 43<id>[{...}].
    const auto f = rp::sio::parse("431[{\"ok\":true,\"code\":\"K7QM2X\",\"leader\":\"chaosfox26\"}]");
    CHECK(f.ok);
    CHECK(f.sio == rp::sio::Sio::Ack);
    CHECK_EQ(f.ack, 1LL);
    CHECK_EQ(f.payload, std::string("[{\"ok\":true,\"code\":\"K7QM2X\",\"leader\":\"chaosfox26\"}]"));

    const auto wide = rp::sio::parse("43127[{\"ok\":true}]");
    CHECK_EQ(wide.ack, 127LL);
}

static void test_parse_namespace()
{
    const auto f = rp::sio::parse("42/admin,3[\"stats\",{}]");
    CHECK(f.ok);
    CHECK_EQ(f.nsp, std::string("/admin"));
    CHECK_EQ(f.ack, 3LL);
    CHECK_EQ(f.payload, std::string("[\"stats\",{}]"));
}

static void test_parse_rejects_garbage()
{
    CHECK(!rp::sio::parse("").ok);
    CHECK(!rp::sio::parse("hello").ok);
    CHECK(!rp::sio::parse("4x[\"nope\"]").ok);   // message with a non-numeric sio type
    CHECK(!rp::sio::parse("4").ok);              // message with nothing after it

    std::string name, arg;
    CHECK(!rp::sio::splitEvent("", name, arg));
    CHECK(!rp::sio::splitEvent("{\"not\":\"an array\"}", name, arg));
    CHECK(!rp::sio::splitEvent("[123,{}]", name, arg));       // name is not a string
    CHECK(!rp::sio::splitEvent("[\"unterminated", name, arg));
    CHECK(!rp::sio::splitEvent("[\"\\u0041\",{}]", name, arg)); // refuse rather than guess
}

// ------------------------------------------------------------------- outbound

static void test_serialize_connect_with_token()
{
    const std::string frame = rp::sio::connect("{\"token\":\"deadbeef\"}");
    CHECK_EQ(frame, std::string("40{\"token\":\"deadbeef\"}"));
    // The token must never end up in the URL, where Caddy would log it.
    CHECK(frame.find('?') == std::string::npos);
}

static void test_serialize_events()
{
    // The field is `command`, not `type`. chat.js switches on packet.command.
    CHECK_EQ(rp::sio::event("send_chat", "{\"command\":\"say\",\"message\":\"Hello\"}"),
             std::string("42[\"send_chat\",{\"command\":\"say\",\"message\":\"Hello\"}]"));

    CHECK_EQ(rp::sio::event("update_position", "{\"x\":1,\"y\":2,\"z\":3}"),
             std::string("42[\"update_position\",{\"x\":1,\"y\":2,\"z\":3}]"));

    CHECK_EQ(rp::sio::event("party_create", "{}", 1),
             std::string("42" "1" "[\"party_create\",{}]"));

    CHECK_EQ(rp::sio::event("who", "{\"query\":\"ae\"}", 42),
             std::string("4242[\"who\",{\"query\":\"ae\"}]"));
}

static void test_round_trip_through_the_parser()
{
    // Anything we emit, we can read back.
    struct Case
    {
        const char* name;
        const char* arg;
        long long ack;
    };
    const Case cases[] = {
        {"send_chat", "{\"command\":\"emote\",\"message\":\"waves.\"}", -1},
        {"send_chat", "{\"command\":\"whisper\",\"target\":\"aiden\",\"message\":\"Hey!\"}", -1},
        {"send_chat", "{\"command\":\"channel\",\"channel\":\"ooc\",\"message\":\"LFG\"}", -1},
        {"update_position", "{\"x\":-12.5,\"y\":0,\"z\":900.25}", -1},
        {"party_join", "{\"code\":\"K7QM2X\"}", 7},
        {"who", "{\"query\":\"ae\"}", 12},
    };

    for (const auto& c : cases)
    {
        const std::string wire = rp::sio::event(c.name, c.arg, c.ack);
        const auto f = rp::sio::parse(wire);
        CHECK(f.ok);
        CHECK(f.eio == rp::sio::Eio::Message);
        CHECK(f.sio == rp::sio::Sio::Event);
        CHECK_EQ(f.ack, c.ack);

        std::string name, arg;
        CHECK(rp::sio::splitEvent(f.payload, name, arg));
        CHECK_EQ(name, std::string(c.name));
        CHECK_EQ(arg, std::string(c.arg));
    }
}

static void test_envelope_survives_the_parser_byte_for_byte()
{
    // The whole point of relaying ciphertext untouched: if splitEvent reformats
    // the argument at all, the receiver's GCM tag check fails and the message is
    // unreadable. Base64 contains '+' and '/' and ends in '=', which are exactly
    // the characters a naive parser mangles.
    const std::string envelope =
        "{\"v\":1,\"iv\":\"q83vASNFZ4mrze8B\",\"ct\":\"a+b/c+d/eFGH12345678==\"}";
    const std::string arg = "{\"command\":\"say\",\"message\":" + envelope + "}";
    const std::string wire = rp::sio::event("send_chat", arg);

    const auto f = rp::sio::parse(wire);
    std::string name, back;
    CHECK(rp::sio::splitEvent(f.payload, name, back));
    CHECK_EQ(back, arg);
    CHECK(back.find("a+b/c+d/eFGH12345678==") != std::string::npos);
}

int main()
{
    test_base64_known_vectors();
    test_base64_never_wraps();
    test_base64_matches_server_regex();
    test_base64_iv_is_16_chars();
    test_base64_ciphertext_budget();
    test_base64_round_trip();
    test_base64_rejects_junk();

    test_parse_open();
    test_parse_ping_and_pong();
    test_parse_connect_ok_and_error();
    test_parse_event();
    test_parse_event_with_no_argument();
    test_parse_ack();
    test_parse_namespace();
    test_parse_rejects_garbage();

    test_serialize_connect_with_token();
    test_serialize_events();
    test_round_trip_through_the_parser();
    test_envelope_survives_the_parser_byte_for_byte();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
