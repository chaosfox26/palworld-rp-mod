// crypto.hpp — AES-256-GCM via Windows BCrypt (CNG).
//
// No dependency to vendor: bcrypt.dll ships with Windows. Throughput is around
// a gigabyte a second with AES-NI, so a 100 KB message costs tens of
// microseconds. That is the entire reason this is not in Lua: a pure-Lua GCM
// implementation would spend 0.1-1 s per message, on the game thread.
//
// Envelope format the backend validates (src/envelope.js):
//
//   { "v": 1, "iv": "<12 bytes, base64>", "ct": "<ciphertext||tag, base64>" }
//
// The 16-byte tag is APPENDED to the ciphertext, which is what Node's crypto
// does by convention and what the receiving mod must therefore expect.
//
// The key never leaves this translation unit's callers inside the DLL. Lua sees
// plaintext going in and plaintext coming out, so no other Lua mod loaded in the
// process can read the key out of our Lua state. It is still extractable from
// the DLL by anyone who wants it — that is inherent to the design and is stated
// plainly in the backend's ENCRYPTION.md. This just avoids giving it away for
// free.

#pragma once

#include "b64.hpp"

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32

#include <windows.h>

#include <bcrypt.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// Lives in <ntstatus.h>, which cannot be included after <windows.h> without the
// WIN32_NO_STATUS dance and a pile of macro redefinition warnings. The value is
// stable and documented, so define it directly rather than reorganising every
// translation unit's include order around it.
#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH ((NTSTATUS)0xC000A002L)
#endif

namespace rp::crypto
{
    inline constexpr size_t kKeyBytes = 32; // AES-256
    inline constexpr size_t kIvBytes = 12;  // the standard GCM nonce size
    inline constexpr size_t kTagBytes = 16;
    inline constexpr int kEnvelopeVersion = 1;

    // STARTUP FALLBACKS ONLY. Never enforce against these.
    //
    // The server's limits are mutable at runtime through an admin overrides file:
    // MAX_MESSAGE_BYTES, MAX_PROFILE_BYTES and friends can change while the
    // server is up. A client that baked in 100,000 starts getting 413s the day
    // the cap is lowered. Read the real values from GET /info and the `ready`
    // frame, and pass them in per call.
    inline constexpr size_t kDefaultMaxMessageBytes = 100000;
    inline constexpr size_t kDefaultMaxProfileBytes = 262144;

    // The limit is measured against the CIPHERTEXT, and ciphertext is plaintext
    // plus the tag — so the usable plaintext is always the cap minus 16.
    inline constexpr size_t plaintextCapFor(size_t maxCiphertextBytes)
    {
        return maxCiphertextBytes > kTagBytes ? maxCiphertextBytes - kTagBytes : 0;
    }

    class Aes256Gcm
    {
    public:
        Aes256Gcm() = default;

        ~Aes256Gcm()
        {
            reset();
        }

        Aes256Gcm(const Aes256Gcm&) = delete;
        Aes256Gcm& operator=(const Aes256Gcm&) = delete;

        // key must be exactly 32 bytes. Call once at startup.
        bool init(const uint8_t* key, size_t keyLen, std::string& err)
        {
            reset();
            if (keyLen != kKeyBytes)
            {
                err = "Application key must be exactly 32 bytes (AES-256).";
                return false;
            }

            NTSTATUS st = BCryptOpenAlgorithmProvider(&_alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
            if (!NT_SUCCESS(st))
            {
                err = "BCryptOpenAlgorithmProvider failed.";
                return false;
            }

            st = BCryptSetProperty(_alg,
                                   BCRYPT_CHAINING_MODE,
                                   (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                                   sizeof(BCRYPT_CHAIN_MODE_GCM),
                                   0);
            if (!NT_SUCCESS(st))
            {
                err = "Could not select GCM chaining mode.";
                reset();
                return false;
            }

            st = BCryptGenerateSymmetricKey(_alg, &_key, nullptr, 0, (PUCHAR)key, (ULONG)keyLen, 0);
            if (!NT_SUCCESS(st))
            {
                err = "BCryptGenerateSymmetricKey failed.";
                reset();
                return false;
            }

            _ready = true;
            return true;
        }

        bool ready() const
        {
            return _ready;
        }

        // Fresh 12 random bytes per message.
        //
        // GCM nonce reuse under a fixed key is a TOTAL break, not a degradation:
        // two messages sharing an IV leak the XOR of their plaintexts and enough
        // to forge tags. There is no counter here on purpose — a counter would
        // have to survive restarts, and getting that wrong is silent. A CSPRNG
        // has a collision probability around 2^-64 after 2^32 messages, which is
        // fine for a chat mod.
        static bool randomIv(uint8_t* out, size_t len)
        {
            return NT_SUCCESS(BCryptGenRandom(
                nullptr, (PUCHAR)out, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
        }

        // Produces the JSON envelope text, ready to drop into a send_chat packet
        // or a profile body.
        //
        // maxCiphertextBytes is the server's CURRENT limit for this kind of
        // payload — maxMessageBytes for chat, maxProfileBytes for profiles. Pass
        // the live value; do not let a constant creep back in here.
        bool encryptToEnvelope(const std::string& plaintext,
                               size_t maxCiphertextBytes,
                               std::string& envelopeJson,
                               std::string& err) const
        {
            envelopeJson.clear();
            if (!_ready)
            {
                err = "Encryption is not initialised.";
                return false;
            }
            if (plaintext.empty())
            {
                // envelope.js rejects a ct shorter than tag + 1 byte, so an empty
                // message cannot be represented. Catch it here rather than eating
                // a round trip to learn that.
                err = "Message is empty.";
                return false;
            }

            const size_t cap = plaintextCapFor(maxCiphertextBytes);
            if (cap == 0)
            {
                err = "The server's size limit is too small to hold any message.";
                return false;
            }
            if (plaintext.size() > cap)
            {
                err = "Message is " + std::to_string(plaintext.size()) + " bytes; the limit is " +
                      std::to_string(cap) + ".";
                return false;
            }

            uint8_t iv[kIvBytes];
            if (!randomIv(iv, kIvBytes))
            {
                err = "Could not obtain random bytes for the IV.";
                return false;
            }

            // BCrypt overwrites the IV buffer it is handed, so give it a copy and
            // keep the original for the envelope.
            uint8_t ivForCall[kIvBytes];
            memcpy(ivForCall, iv, kIvBytes);

            BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
            BCRYPT_INIT_AUTH_MODE_INFO(info);
            info.pbNonce = ivForCall;
            info.cbNonce = kIvBytes;

            uint8_t tag[kTagBytes] = {};
            info.pbTag = tag;
            info.cbTag = kTagBytes;

            std::vector<uint8_t> ct(plaintext.size() + kTagBytes);
            ULONG written = 0;

            const NTSTATUS st = BCryptEncrypt(_key,
                                              (PUCHAR)plaintext.data(),
                                              (ULONG)plaintext.size(),
                                              &info,
                                              nullptr,
                                              0,
                                              ct.data(),
                                              (ULONG)plaintext.size(),
                                              &written,
                                              0);
            if (!NT_SUCCESS(st) || written != plaintext.size())
            {
                err = "BCryptEncrypt failed.";
                return false;
            }

            // Append the tag, matching Node's convention.
            memcpy(ct.data() + plaintext.size(), tag, kTagBytes);

            envelopeJson = "{\"v\":";
            envelopeJson += std::to_string(kEnvelopeVersion);
            envelopeJson += ",\"iv\":\"";
            envelopeJson += b64::encode(iv, kIvBytes);
            envelopeJson += "\",\"ct\":\"";
            envelopeJson += b64::encode(ct);
            envelopeJson += "\"}";
            return true;
        }

        // Inverse. ivB64 and ctB64 come straight out of the received envelope.
        //
        // A failed tag check is not an error to shrug at: it means the bytes were
        // altered, or the sender has a different key (a mod version mismatch).
        // Either way the caller should show something, not silently drop it.
        bool decryptFromEnvelope(const std::string& ivB64,
                                 const std::string& ctB64,
                                 std::string& plaintext,
                                 std::string& err) const
        {
            plaintext.clear();
            if (!_ready)
            {
                err = "Encryption is not initialised.";
                return false;
            }

            std::vector<uint8_t> iv;
            if (!b64::decode(ivB64, iv) || iv.size() != kIvBytes)
            {
                err = "Envelope iv is not 12 base64-encoded bytes.";
                return false;
            }

            std::vector<uint8_t> ct;
            if (!b64::decode(ctB64, ct) || ct.size() < kTagBytes + 1)
            {
                err = "Envelope ct is malformed or too short to hold a message and its tag.";
                return false;
            }

            const size_t bodyLen = ct.size() - kTagBytes;

            BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
            BCRYPT_INIT_AUTH_MODE_INFO(info);
            info.pbNonce = iv.data();
            info.cbNonce = (ULONG)iv.size();
            info.pbTag = ct.data() + bodyLen;
            info.cbTag = kTagBytes;

            std::vector<uint8_t> out(bodyLen);
            ULONG written = 0;

            const NTSTATUS st = BCryptDecrypt(_key,
                                              ct.data(),
                                              (ULONG)bodyLen,
                                              &info,
                                              nullptr,
                                              0,
                                              out.data(),
                                              (ULONG)out.size(),
                                              &written,
                                              0);
            if (st == STATUS_AUTH_TAG_MISMATCH)
            {
                err = "Message failed its authentication check — wrong key, or the bytes were altered.";
                return false;
            }
            if (!NT_SUCCESS(st))
            {
                err = "BCryptDecrypt failed.";
                return false;
            }

            plaintext.assign((const char*)out.data(), written);
            return true;
        }

    private:
        void reset()
        {
            if (_key)
            {
                BCryptDestroyKey(_key);
                _key = nullptr;
            }
            if (_alg)
            {
                BCryptCloseAlgorithmProvider(_alg, 0);
                _alg = nullptr;
            }
            _ready = false;
        }

        BCRYPT_ALG_HANDLE _alg = nullptr;
        BCRYPT_KEY_HANDLE _key = nullptr;
        bool _ready = false;
    };
} // namespace rp::crypto

#endif // _WIN32
