// g++ -std=c++17 -I../loader test_core_logic.cpp ../loader/core_logic.cpp
#include <cmath>
#include <cstdio>
#include "core_logic.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

int main() {
    using namespace gocode;

    // --- TimeScaler: 1.0 is transparent ---
    TimeScaler t;
    CHECK(t.Map(1000) == 1000);
    CHECK(t.Map(1500) == 1500);
    CHECK(t.Map(1500) == 1500);

    // --- 2x: elapsed time doubles ---
    t.SetScale(2.0, 2000);               // real 2000 -> virtual 2000
    CHECK(t.Map(2000) == 2000);
    CHECK(t.Map(3000) == 4000);          // +1000 real = +2000 virtual
    // --- change to 0.5 mid-stream: continuous, no jump ---
    t.SetScale(0.5, 3000);
    CHECK(t.Map(3000) == 4000);
    CHECK(t.Map(5000) == 5000);          // +2000 real = +1000 virtual
    // --- back to 1.0 ---
    t.SetScale(1.0, 5000);
    CHECK(t.Map(6000) == 6000);
    CHECK(std::fabs(t.Scale() - 1.0) < 1e-12);
    // --- clock never goes backwards ---
    CHECK(t.Map(5000) == 6000);
    // --- first call after SetScale on a fresh scaler ---
    TimeScaler u;
    u.SetScale(3.0, 100);
    CHECK(u.Map(100) == 100);
    CHECK(u.Map(200) == 400);
    // --- big values stay exact at 1.0 (QPC at 10 MHz after ~10 days) ---
    TimeScaler big;
    int64_t start = 8640000000000LL;
    CHECK(big.Map(start) == start);
    CHECK(big.Map(start + 12345) == start + 12345);

    // --- URL blocking ---
    std::vector<std::string> bl = {"uploadGJLevel", "updateGJUserScore", "getGJLevelScores", "uploadGJComment"};
    CHECK(ShouldBlockUrl("http://www.boomlings.com/database/uploadGJLevel21.php", bl));
    CHECK(ShouldBlockUrl("http://www.boomlings.com/database/UPDATEGJUSERSCORE22.php", bl));
    CHECK(ShouldBlockUrl("http://www.boomlings.com/database/getGJLevelScores211.php", bl));
    CHECK(!ShouldBlockUrl("http://www.boomlings.com/database/downloadGJLevel22.php", bl));
    CHECK(!ShouldBlockUrl("http://www.boomlings.com/database/getGJLevels21.php", bl));
    CHECK(!ShouldBlockUrl("", bl));
    CHECK(!ShouldBlockUrl("http://x/uploadGJLevel21.php", {}));
    CHECK(!ShouldBlockUrl("http://x/uploadGJLevel21.php", {""}));

    // --- csv ---
    auto v = SplitCsv(" a, b ,,c ,");
    CHECK(v.size() == 3 && v[0] == "a" && v[1] == "b" && v[2] == "c");
    CHECK(SplitCsv("").empty());
    CHECK(SplitCsv("GeometryDash.exe,libcocos2d.dll").size() == 2);

    // --- clamp ---
    CHECK(ClampDouble(5, 0.1, 3) == 3);
    CHECK(ClampDouble(-1, 0.1, 3) == 0.1);
    CHECK(ClampDouble(1, 0.1, 3) == 1);
    CHECK(ClampDouble(NAN, 0.1, 3) == 0.1);

    if (!failures) printf("All core logic tests passed.\n");
    return failures ? 1 : 0;
}
