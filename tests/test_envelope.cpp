// test_envelope.cpp — the C++ half of the interop check.
//
// Reads envelopes produced by Node, decodes each field with the mod's own
// base64, re-encodes it, and echoes both back so the Node side can confirm they
// are byte-identical. Any difference means the server would see something other
// than what Node would have sent.
//
//   g++ -std=c++17 -I../src test_envelope.cpp -o test_envelope
//   node envelope_interop.js emit | ./test_envelope | node envelope_interop.js check

#include "b64.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    // Just enough JSON scraping for a flat object of string and number fields.
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

    long long numberField(const std::string& line, const std::string& key)
    {
        const std::string needle = "\"" + key + "\":";
        const size_t p = line.find(needle);
        if (p == std::string::npos) return -1;
        size_t i = p + needle.size();
        long long v = 0;
        bool any = false;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9')
        {
            v = v * 10 + (line[i] - '0');
            ++i;
            any = true;
        }
        return any ? v : -1;
    }
} // namespace

int main()
{
    std::string line;
    int failures = 0;
    int count = 0;

    while (std::getline(std::cin, line))
    {
        if (line.empty()) continue;
        ++count;

        const std::string label = field(line, "label");
        const std::string iv = field(line, "iv");
        const std::string ct = field(line, "ct");
        const long long plainLen = numberField(line, "plainLen");

        std::vector<uint8_t> ivBytes, ctBytes;
        if (!rp::b64::decode(iv, ivBytes))
        {
            std::fprintf(stderr, "FAIL %s: could not decode iv\n", label.c_str());
            ++failures;
            continue;
        }
        if (!rp::b64::decode(ct, ctBytes))
        {
            std::fprintf(stderr, "FAIL %s: could not decode ct\n", label.c_str());
            ++failures;
            continue;
        }

        if (ivBytes.size() != 12)
        {
            std::fprintf(stderr, "FAIL %s: iv decoded to %zu bytes\n", label.c_str(), ivBytes.size());
            ++failures;
        }
        if ((long long)ctBytes.size() != plainLen + 16)
        {
            std::fprintf(stderr,
                         "FAIL %s: ct is %zu bytes, expected plaintext %lld + 16 tag\n",
                         label.c_str(),
                         ctBytes.size(),
                         plainLen);
            ++failures;
        }
        if (rp::b64::decodedLength(ct) != (long long)ctBytes.size())
        {
            std::fprintf(stderr, "FAIL %s: decodedLength disagrees with decode\n", label.c_str());
            ++failures;
        }

        const std::string reIv = rp::b64::encode(ivBytes);
        const std::string reCt = rp::b64::encode(ctBytes);

        std::printf("{\"label\":\"%s\",\"iv\":\"%s\",\"ct\":\"%s\",\"cppIv\":\"%s\",\"cppCt\":\"%s\"}\n",
                    label.c_str(),
                    iv.c_str(),
                    ct.c_str(),
                    reIv.c_str(),
                    reCt.c_str());
    }

    std::fprintf(stderr, "test_envelope: %d envelopes, %d failures\n", count, failures);
    return failures == 0 ? 0 : 1;
}
