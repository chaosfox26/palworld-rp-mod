// test_crypto_win.cpp — Windows-only, but needs NO UE4SS and NO Palworld.
//
// This is the highest-value test in the project, because it proves the BCrypt
// path interoperates with Node in BOTH directions. If C++ and Node disagree
// about tag placement, IV size, or base64, chat is unreadable and the only
// symptom in game is "[unreadable: ...]" on every line.
//
//   cl /std:c++17 /EHsc /I..\src test_crypto_win.cpp /link bcrypt.lib
//   node envelope_interop.js emit | test_crypto_win.exe | node envelope_interop.js verify
//
// Phase 1  decrypt envelopes Node produced
// Phase 2  local checks: tamper detection, IV freshness, the size cap
// Phase 3  emit envelopes for Node to decrypt

#include "crypto.hpp"

#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace
{
    int failures = 0;
    int checks = 0;

    void check(bool cond, const std::string& what)
    {
        ++checks;
        if (!cond)
        {
            std::fprintf(stderr, "FAIL %s\n", what.c_str());
            ++failures;
        }
    }

    // The same fixed test key envelope_interop.js uses: 32 bytes of 0x2b.
    // Not a real key, and never shipped.
    std::vector<unsigned char> testKey()
    {
        return std::vector<unsigned char>(32, 0x2b);
    }

    std::string field(const std::string& line, const std::string& key)
    {
        const std::string needle = "\"" + key + "\":\"";
        const size_t p = line.find(needle);
        if (p == std::string::npos) return {};
        const size_t start = p + needle.size();
        const size_t end = line.find('"', start);
        if (end == std::string::npos) return {};
        return line.substr(start, end - start);
    }

    std::string jsonEscape(const std::string& s)
    {
        std::string out;
        for (unsigned char c : s)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                    {
                        out += char(c);
                    }
            }
        }
        return out;
    }
} // namespace

int main()
{
    // The cap is a per-call argument now, not a constant: the server's limits are
    // mutable at runtime through its admin overrides file. Tests use the default.
    const size_t CAP = rp::crypto::kDefaultMaxMessageBytes;

    rp::crypto::Aes256Gcm aes;
    std::string err;
    const auto key = testKey();
    if (!aes.init(key.data(), key.size(), err))
    {
        std::fprintf(stderr, "FATAL could not initialise AES: %s\n", err.c_str());
        return 2;
    }

    // ---- key length must be enforced -------------------------------------
    {
        rp::crypto::Aes256Gcm wrong;
        std::string e;
        check(!wrong.init(key.data(), 16, e), "a 16-byte key is rejected (this is AES-256)");
        check(!wrong.ready(), "a failed init leaves the object unusable");
    }

    // ---- Phase 1: decrypt what Node encrypted ----------------------------
    int fromNode = 0;
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty()) continue;
        ++fromNode;

        const std::string label = field(line, "label");
        const std::string iv = field(line, "iv");
        const std::string ct = field(line, "ct");
        const std::string plainB64 = field(line, "plainB64");

        std::vector<uint8_t> expected;
        if (!rp::b64::decode(plainB64, expected) && !plainB64.empty())
        {
            check(false, "could not decode plainB64 for " + label);
            continue;
        }

        std::string plain, e;
        if (!aes.decryptFromEnvelope(iv, ct, plain, e))
        {
            check(false, "decrypt Node's envelope [" + label + "]: " + e);
            continue;
        }

        const std::vector<uint8_t> got(plain.begin(), plain.end());
        check(got == expected, "Node -> C++ round trip matches for [" + label + "]");
    }
    check(fromNode > 0, "received envelopes from Node on stdin");

    // ---- Phase 2: local behaviour ----------------------------------------

    // Tag placement and tamper detection. Flipping one ciphertext bit must fail,
    // not silently return garbage.
    {
        std::string envelope, e;
        check(aes.encryptToEnvelope("tamper me", CAP, envelope, e), "encrypt for the tamper test");

        const std::string iv = field(envelope, "iv");
        const std::string ct = field(envelope, "ct");

        std::vector<uint8_t> raw;
        check(rp::b64::decode(ct, raw), "decode our own ciphertext");
        check(raw.size() == std::string("tamper me").size() + 16,
              "ciphertext is plaintext + a 16-byte tag, appended");

        raw[0] ^= 0x01;
        std::string plain;
        const bool ok = aes.decryptFromEnvelope(iv, rp::b64::encode(raw), plain, e);
        check(!ok, "a flipped ciphertext bit fails authentication");
        check(plain.empty(), "a failed decrypt yields nothing, not partial plaintext");

        // And a flipped tag bit.
        std::vector<uint8_t> raw2;
        rp::b64::decode(ct, raw2);
        raw2[raw2.size() - 1] ^= 0x80;
        check(!aes.decryptFromEnvelope(iv, rp::b64::encode(raw2), plain, e),
              "a flipped tag bit fails authentication");
    }

    // IV freshness. Nonce reuse under a fixed key is a total break, so the same
    // plaintext must never produce the same envelope twice.
    {
        std::set<std::string> ivs;
        std::set<std::string> cts;
        for (int i = 0; i < 500; ++i)
        {
            std::string envelope, e;
            if (!aes.encryptToEnvelope("the same text every time", CAP, envelope, e))
            {
                check(false, "encrypt during the IV freshness loop");
                break;
            }
            ivs.insert(field(envelope, "iv"));
            cts.insert(field(envelope, "ct"));
        }
        check(ivs.size() == 500, "500 encryptions produced 500 distinct IVs");
        check(cts.size() == 500, "identical plaintext produced 500 distinct ciphertexts");

        for (const auto& iv : ivs)
        {
            if (iv.size() != 16)
            {
                check(false, "every IV is 16 base64 characters (12 bytes)");
                break;
            }
        }
    }

    // The size cap is on ciphertext, so the plaintext limit is 16 bytes lower.
    {
        std::string envelope, e;
        const std::string atCap(rp::crypto::plaintextCapFor(CAP), 'A');
        check(aes.encryptToEnvelope(atCap, CAP, envelope, e),
              "a plaintext exactly at the 99,984-byte cap is accepted");
        check(rp::b64::decodedLength(field(envelope, "ct")) == 100000,
              "that produces exactly 100,000 ciphertext bytes, the server's limit");

        const std::string overCap(rp::crypto::plaintextCapFor(CAP) + 1, 'A');
        check(!aes.encryptToEnvelope(overCap, CAP, envelope, e),
              "one byte over the cap is refused locally, not by the server");

        check(!aes.encryptToEnvelope("", CAP, envelope, e),
              "an empty message is refused (ct must exceed the tag length)");
    }

    // The cap is honoured per call, which is what lets chat (maxMessageBytes) and
    // profiles (maxProfileBytes) share one implementation. If this regresses, a
    // long profile is silently refused or an oversized chat frame gets the player
    // disconnected with no error.
    {
        std::string envelope, e;
        check(!aes.encryptToEnvelope(std::string(100, 'x'), 64, envelope, e),
              "a 100-byte payload is refused against a 64-byte cap");
        check(aes.encryptToEnvelope(std::string(48, 'x'), 64, envelope, e),
              "a 48-byte payload fits a 64-byte cap (48 + 16 tag)");
        check(!aes.encryptToEnvelope(std::string(49, 'x'), 64, envelope, e),
              "49 bytes does not, because the tag takes the last 16");

        const size_t profileCap = rp::crypto::kDefaultMaxProfileBytes;
        check(aes.encryptToEnvelope(std::string(200000, 'p'), profileCap, envelope, e),
              "a 200 KB profile fits maxProfileBytes but would never fit the chat cap");
        check(!aes.encryptToEnvelope(std::string(200000, 'p'), CAP, envelope, e),
              "the same payload is refused against the chat cap");
    }

    // A large message must survive intact, since that is the whole point of the
    // mod's own input box.
    {
        std::string big;
        big.reserve(90000);
        for (int i = 0; i < 90000; ++i) big += char('a' + (i % 26));

        std::string envelope, e;
        check(aes.encryptToEnvelope(big, CAP, envelope, e), "encrypt a 90 KB message");
        std::string back;
        check(aes.decryptFromEnvelope(field(envelope, "iv"), field(envelope, "ct"), back, e),
              "decrypt a 90 KB message");
        check(back == big, "90 KB survives the round trip byte for byte");
    }

    // A different key must not be able to read our messages. This is the check
    // that catches a mod built with the wrong key.
    {
        std::vector<unsigned char> other(32, 0x99);
        rp::crypto::Aes256Gcm stranger;
        std::string e;
        check(stranger.init(other.data(), other.size(), e), "initialise a second key");

        std::string envelope;
        aes.encryptToEnvelope("secret", CAP, envelope, e);
        std::string plain;
        check(!stranger.decryptFromEnvelope(field(envelope, "iv"), field(envelope, "ct"), plain, e),
              "a build with a different key cannot decrypt");
    }

    // Malformed envelopes must be rejected rather than crashing.
    {
        std::string plain, e;
        check(!aes.decryptFromEnvelope("", "", plain, e), "empty fields rejected");
        check(!aes.decryptFromEnvelope("AAAA", "AAAAAAAAAAAAAAAAAAAAAAAA", plain, e),
              "a 3-byte iv is rejected");
        check(!aes.decryptFromEnvelope("q83vASNFZ4mrze8B", "AAAA", plain, e),
              "a ct shorter than the tag is rejected");
        check(!aes.decryptFromEnvelope("q83vASNFZ4mrze8B", "not base64!!", plain, e),
              "non-base64 is rejected");
    }

    // ---- Phase 3: hand envelopes back for Node to decrypt ----------------
    {
        const std::pair<const char*, std::string> cases[] = {
            {"cpp ascii", "Hello from the mod."},
            {"cpp one byte", "x"},
            {"cpp quotes", "She said \"no\" \\ then left"},
            {"cpp newlines", "line one\nline two\r\nline three"},
            {"cpp unicode", "chaosfox26 nods \xE2\x80\x94 \xE3\x80\x8C\xE3\x81\xAF\xE3\x81\x84\xE3\x80\x8D"},
            {"cpp len 2", "zz"},
            {"cpp len 3", "zzz"},
            {"cpp at cap", std::string(rp::crypto::plaintextCapFor(CAP), 'A')},
        };

        for (const auto& c : cases)
        {
            std::string envelope, e;
            if (!aes.encryptToEnvelope(c.second, CAP, envelope, e))
            {
                check(false, std::string("encrypt for phase 3: ") + c.first);
                continue;
            }
            std::printf("{\"label\":\"%s\",\"iv\":\"%s\",\"ct\":\"%s\",\"plainB64\":\"%s\"}\n",
                        jsonEscape(c.first).c_str(),
                        field(envelope, "iv").c_str(),
                        field(envelope, "ct").c_str(),
                        rp::b64::encode((const uint8_t*)c.second.data(), c.second.size()).c_str());
        }
    }

    std::fprintf(stderr,
                 "test_crypto_win: %d checks (%d envelopes from Node), %d failures\n",
                 checks,
                 fromNode,
                 failures);
    return failures == 0 ? 0 : 1;
}
