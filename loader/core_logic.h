// Portable (no Windows) logic used by the GoCode core: clock scaling, URL filtering, small helpers.
// Kept separate so it can be unit tested on any OS.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gocode {

// Turns a real, monotonically increasing clock into one that runs at `scale` x real speed.
// Continuous: changing the scale never makes the clock jump. NOT thread-safe - lock around it.
class TimeScaler {
public:
    int64_t Map(int64_t real);                       // feed a real reading, get the scaled reading
    void SetScale(double scale, int64_t realNow);    // takes effect exactly at realNow
    double Scale() const { return scale_; }

private:
    double scale_ = 1.0;
    int64_t lastReal_ = 0;
    double virt_ = 0.0;
    bool init_ = false;
};

// Case-insensitive: does the URL contain any entry of `blocklist` (e.g. "uploadGJLevel")?
bool ShouldBlockUrl(const std::string& url, const std::vector<std::string>& blocklist);

// "a, b ,,c" -> {"a","b","c"}
std::vector<std::string> SplitCsv(const std::string& s);

double ClampDouble(double v, double lo, double hi);

}  // namespace gocode
