#include "core_logic.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace gocode {

int64_t TimeScaler::Map(int64_t real) {
    if (!init_) {
        init_ = true;
        lastReal_ = real;
        virt_ = static_cast<double>(real);
        return real;
    }
    int64_t delta = real - lastReal_;
    if (delta < 0) delta = 0;  // never run backwards
    virt_ += static_cast<double>(delta) * scale_;
    lastReal_ = real;
    return static_cast<int64_t>(virt_);
}

void TimeScaler::SetScale(double scale, int64_t realNow) {
    Map(realNow);  // account for time elapsed under the old scale
    scale_ = scale;
}

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ShouldBlockUrl(const std::string& url, const std::vector<std::string>& blocklist) {
    const std::string u = Lower(url);
    for (const auto& b : blocklist) {
        if (!b.empty() && u.find(Lower(b)) != std::string::npos) return true;
    }
    return false;
}

std::vector<std::string> SplitCsv(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        size_t a = i, b = j;
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        if (b > a) out.push_back(s.substr(a, b - a));
        i = j + 1;
    }
    return out;
}

double ClampDouble(double v, double lo, double hi) {
    if (std::isnan(v)) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace gocode
