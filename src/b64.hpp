// b64.hpp — canonical base64, no platform or third-party dependencies.
//
// The backend's src/envelope.js validates with /^[A-Za-z0-9+/]+={0,2}$/ and
// requires length % 4 == 0. That means:
//
//   * no line wrapping (the classic 76-column MIME style is REJECTED)
//   * padding is mandatory
//   * no whitespace anywhere, including a trailing newline
//
// Most copy-paste base64 helpers wrap. This one never does.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rp::b64
{
    inline constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    inline std::string encode(const uint8_t* data, size_t len)
    {
        std::string out;
        out.reserve(((len + 2) / 3) * 4);

        size_t i = 0;
        for (; i + 3 <= len; i += 3)
        {
            const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
            out += kAlphabet[(n >> 18) & 63];
            out += kAlphabet[(n >> 12) & 63];
            out += kAlphabet[(n >> 6) & 63];
            out += kAlphabet[n & 63];
        }

        const size_t rem = len - i;
        if (rem == 1)
        {
            const uint32_t n = uint32_t(data[i]) << 16;
            out += kAlphabet[(n >> 18) & 63];
            out += kAlphabet[(n >> 12) & 63];
            out += '=';
            out += '=';
        }
        else if (rem == 2)
        {
            const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
            out += kAlphabet[(n >> 18) & 63];
            out += kAlphabet[(n >> 12) & 63];
            out += kAlphabet[(n >> 6) & 63];
            out += '=';
        }
        return out;
    }

    inline std::string encode(const std::vector<uint8_t>& v)
    {
        return encode(v.data(), v.size());
    }

    namespace detail
    {
        // -1 = not a base64 character.
        inline int value(char c)
        {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        }
    } // namespace detail

    // Strict decode. Returns false on anything the server would also reject,
    // so a bad envelope fails locally instead of costing a round trip.
    inline bool decode(const std::string& in, std::vector<uint8_t>& out)
    {
        out.clear();
        if (in.empty() || in.size() % 4 != 0) return false;

        size_t pad = 0;
        if (in[in.size() - 1] == '=') pad = (in[in.size() - 2] == '=') ? 2 : 1;

        // Padding may only appear in the final quantum.
        const size_t body = in.size() - pad;
        for (size_t i = 0; i < body; ++i)
        {
            if (detail::value(in[i]) < 0) return false;
        }

        out.reserve((in.size() / 4) * 3 - pad);

        for (size_t i = 0; i < in.size(); i += 4)
        {
            const bool last = (i + 4 == in.size());
            const int c0 = detail::value(in[i]);
            const int c1 = detail::value(in[i + 1]);
            if (c0 < 0 || c1 < 0) return false;

            int c2 = -1, c3 = -1;
            const size_t quantumPad = last ? pad : 0;
            if (quantumPad < 2)
            {
                c2 = detail::value(in[i + 2]);
                if (c2 < 0) return false;
            }
            if (quantumPad < 1)
            {
                c3 = detail::value(in[i + 3]);
                if (c3 < 0) return false;
            }

            out.push_back(uint8_t((c0 << 2) | (c1 >> 4)));
            if (quantumPad < 2) out.push_back(uint8_t(((c1 & 15) << 4) | (c2 >> 2)));
            if (quantumPad < 1) out.push_back(uint8_t(((c2 & 3) << 6) | c3));
        }
        return true;
    }

    // Number of bytes `in` decodes to, or -1 if it is not well formed.
    // Mirrors decodedLength() in the backend's envelope.js.
    inline long long decodedLength(const std::string& in)
    {
        if (in.empty() || in.size() % 4 != 0) return -1;
        size_t pad = 0;
        if (in[in.size() - 1] == '=') pad = (in[in.size() - 2] == '=') ? 2 : 1;
        return (long long)((in.size() / 4) * 3 - pad);
    }
} // namespace rp::b64
