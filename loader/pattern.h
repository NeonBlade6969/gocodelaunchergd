// Byte patterns ("8B 45 ?? 85 C0") - portable.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gocode {

constexpr size_t npos = static_cast<size_t>(-1);

// Each entry is 0..255, or -1 for a wildcard.
struct Pattern {
    std::vector<int> bytes;
};

bool parsePattern(const std::string& text, Pattern& out);
size_t findPattern(const uint8_t* data, size_t len, const Pattern& p);  // npos if not found

}  // namespace gocode
