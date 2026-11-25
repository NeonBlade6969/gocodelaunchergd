#include "pattern.h"

#include <cctype>
#include <cstdlib>

namespace gocode {

static bool parseHexByte(const std::string& tok, uint8_t& out) {
    std::string t = tok;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    if (t.empty() || t.size() > 2) return false;
    for (char c : t)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    out = static_cast<uint8_t>(std::strtoul(t.c_str(), nullptr, 16));
    return true;
}

bool parsePattern(const std::string& text, Pattern& out) {
    out.bytes.clear();
    size_t i = 0, n = text.size();
    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= n) break;
        size_t j = i;
        while (j < n && !std::isspace(static_cast<unsigned char>(text[j]))) ++j;
        std::string tok = text.substr(i, j - i);
        i = j;
        if (tok == "?" || tok == "??") {
            out.bytes.push_back(-1);
            continue;
        }
        uint8_t b;
        if (!parseHexByte(tok, b)) return false;
        out.bytes.push_back(b);
    }
    return !out.bytes.empty();
}

size_t findPattern(const uint8_t* data, size_t len, const Pattern& p) {
    const size_t m = p.bytes.size();
    if (m == 0 || len < m) return npos;
    for (size_t i = 0; i + m <= len; ++i) {
        size_t k = 0;
        while (k < m && (p.bytes[k] < 0 || data[i + k] == static_cast<uint8_t>(p.bytes[k]))) ++k;
        if (k == m) return i;
    }
    return npos;
}

}  // namespace gocode
