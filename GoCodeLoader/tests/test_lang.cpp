// Unit tests for the GoCode language and the shipped mods.
// Build/run from tests/:  g++ -std=c++17 -I../loader test_lang.cpp ../loader/gocode_lang.cpp ../loader/pattern.cpp && ./a.out ..
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "gocode_lang.h"
#include "pattern.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) { printf("FAIL line %d: %s == %s\n   got:  %s\n   want: %s\n", __LINE__, #a, #b, Str(_a).c_str(), Str(_b).c_str()); ++failures; } } while (0)

static std::string Str(const std::string& s) { return "\"" + s + "\""; }
static std::string Str(double d) { char b[64]; snprintf(b, sizeof b, "%g", d); return b; }
static std::string Str(bool b) { return b ? "true" : "false"; }
static std::string Str(size_t v) { return std::to_string(v); }
static std::string Str(int v) { return std::to_string(v); }

class MockHost : public gocode::Host {
public:
    std::vector<std::string> logs;
    double speed = 1.0, fps = 0.0;
    bool guard = true, tainted = false;
    std::string taintReason;
    std::map<std::string, std::string> status;
    std::map<std::string, std::string> config;  // "slug.key" -> value
    std::map<uint64_t, uint8_t> mem;            // fake game memory
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> patches;
    uint64_t scanResult = 0;
    int64_t now = 1000;
    int fpsFail = 0;

    void Log(const std::string& mod, const std::string& m) override { logs.push_back(mod + ": " + m); }
    double GetSpeed() override { return speed; }
    bool SetSpeed(double s) override {
        if (s != 1.0 && !guard) return false;
        speed = s;
        if (s != 1.0) Taint("speedhack");
        return true;
    }
    double GetFps() override { return fps; }
    bool SetFps(double f) override { if (fpsFail) return false; fps = f; return true; }
    bool Tainted() override { return tainted; }
    void Taint(const std::string& r) override { tainted = true; taintReason = r; }
    bool GuardActive() override { return guard; }
    void SetStatus(const std::string& mod, const std::string& t) override { status[mod] = t; }
    bool Patch(uint64_t a, const std::vector<uint8_t>& b) override {
        patches.push_back({a, b});
        for (size_t i = 0; i < b.size(); ++i) mem[a + i] = b[i];
        return true;
    }
    uint64_t Scan(const std::string&) override { return scanResult; }
    bool Read(uint64_t a, size_t n, std::vector<uint8_t>& out) override {
        out.clear();
        for (size_t i = 0; i < n; ++i) {
            auto it = mem.find(a + i);
            if (it == mem.end()) return false;
            out.push_back(it->second);
        }
        return true;
    }
    uint64_t GdBase() override { return 0x400000; }
    uint32_t GdStamp() override { return 0xABCD1234; }
    bool Config(const std::string& slug, const std::string& key, std::string& out) override {
        auto it = config.find(slug + "." + key);
        if (it == config.end()) return false;
        out = it->second;
        return true;
    }
    int64_t NowMs() override { return now; }
    bool Logged(const std::string& needle) const {
        for (auto& l : logs) if (l.find(needle) != std::string::npos) return true;
        return false;
    }
};

struct Ran {
    MockHost host;
    std::unique_ptr<gocode::Runtime> rt;
    std::vector<std::string> parseErrors;
    bool parsed = false, inited = false;
};

static std::unique_ptr<Ran> Run(const std::string& src, const std::string& slug = "t") {
    auto r = std::make_unique<Ran>();
    r->rt = std::make_unique<gocode::Runtime>(&r->host, slug, slug);
    r->parsed = r->rt->Parse(src, r->parseErrors);
    if (r->parsed) r->inited = r->rt->Init();
    return r;
}

// Runs `log(<expr>)` and returns what was logged.
static std::string Eval(const std::string& expr) {
    auto r = Run("log(" + expr + ")");
    if (!r->parsed) return "PARSE ERROR: " + r->parseErrors[0];
    if (r->host.logs.empty()) return "NO LOG";
    return r->host.logs.back().substr(3);  // strip "t: "
}

static std::string ErrOf(const std::string& src) {
    auto r = Run(src);
    if (!r->parsed) return r->parseErrors[0];
    if (!r->host.logs.empty()) return r->host.logs.back().substr(3);
    return "NO ERROR";
}

static int FindKey(gocode::Runtime& rt, uint32_t vk, uint32_t mods) {
    for (size_t i = 0; i < rt.events().size(); ++i)
        if (rt.events()[i].kind == gocode::EventInfo::Key && rt.events()[i].vk == vk && rt.events()[i].mods == mods) return static_cast<int>(i);
    return -1;
}
static int FindTick(gocode::Runtime& rt) {
    for (size_t i = 0; i < rt.events().size(); ++i)
        if (rt.events()[i].kind == gocode::EventInfo::Tick) return static_cast<int>(i);
    return -1;
}
static int FindStart(gocode::Runtime& rt) {
    for (size_t i = 0; i < rt.events().size(); ++i)
        if (rt.events()[i].kind == gocode::EventInfo::Start) return static_cast<int>(i);
    return -1;
}

static std::string ReadFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "..";

    // ---------- expressions ----------
    CHECK_EQ(Eval("1 + 2 * 3"), std::string("7"));
    CHECK_EQ(Eval("(1 + 2) * 3"), std::string("9"));
    CHECK_EQ(Eval("10 % 3"), std::string("1"));
    CHECK_EQ(Eval("-2 * 3"), std::string("-6"));
    CHECK_EQ(Eval("7 / 2"), std::string("3.5"));
    CHECK_EQ(Eval("0.1 + 0.2"), std::string("0.3"));
    CHECK_EQ(Eval("0xFF + 1"), std::string("256"));
    CHECK_EQ(Eval("\"a\" + 1 + true"), std::string("a1true"));
    CHECK_EQ(Eval("[1, 2] + [3]"), std::string("[1, 2, 3]"));
    CHECK_EQ(Eval("1 < 2 && 2 < 3"), std::string("true"));
    CHECK_EQ(Eval("\"abc\" < \"abd\""), std::string("true"));
    CHECK_EQ(Eval("!true || false"), std::string("false"));
    CHECK_EQ(Eval("nil == nil"), std::string("true"));
    CHECK_EQ(Eval("[1,[2]] == [1,[2]]"), std::string("true"));
    CHECK_EQ(Eval("1 == \"1\""), std::string("false"));
    CHECK_EQ(Eval("0 || 5"), std::string("0"));          // 0 is truthy
    CHECK_EQ(Eval("nil || 5"), std::string("5"));
    CHECK_EQ(Eval("false && 5"), std::string("false"));
    CHECK_EQ(Eval("\"hi\\n\\t\\\"x\\\"\""), std::string("hi\n\t\"x\""));
    CHECK_EQ(Eval("\"abc\"[1]"), std::string("b"));
    CHECK_EQ(Eval("1s + 500ms + 2"), std::string("1502"));
    CHECK_EQ(Eval("1.5s"), std::string("1500"));
    CHECK_EQ(Eval("[\"a\", 1, [true, nil]]"), std::string("[\"a\", 1, [true, nil]]"));

    // ---------- statements ----------
    {
        auto r = Run("let x = 1\nx += 4\nx *= 2\nx -= 3\nx /= 7\nlog(x)\n"
                     "let i = 0\nlet sum = 0\nwhile i < 10 { i += 1\n if i % 2 == 0 { continue }\n if i > 7 { break }\n sum += i }\nlog(sum)\n"
                     "let acc = []\nfor v in range(3) { push(acc, v * v) }\nlog(acc)\n"
                     "let s = \"\"\nfor ch in \"abc\" { s = ch + s }\nlog(s)\n"
                     "if 1 > 2 { log(\"a\") } else if 2 > 3 { log(\"b\") } else { log(\"c\") }\n"
                     "if false { log(\"x\") }\n"
                     "if true { log(\"newline-else\") }\nelse { log(\"wrong\") }\n");
        CHECK(r->parsed && r->inited);
        CHECK_EQ(r->host.logs.size(), (size_t)6);
        CHECK_EQ(r->host.logs[0], std::string("t: 1"));
        CHECK_EQ(r->host.logs[1], std::string("t: 16"));  // 1+3+5+7
        CHECK_EQ(r->host.logs[2], std::string("t: [0, 1, 4]"));
        CHECK_EQ(r->host.logs[3], std::string("t: cba"));
        CHECK_EQ(r->host.logs[4], std::string("t: c"));
        CHECK_EQ(r->host.logs[5], std::string("t: newline-else"));
    }
    {
        auto r = Run("if true { log(\"newline-else\") }\nelse { log(\"wrong\") }");
        CHECK(r->parsed && r->host.logs.size() == 1 && r->host.logs[0] == "t: newline-else");
    }
    {  // functions, recursion, scope
        auto r = Run("fn fib(n) { if n < 2 { return n }\n return fib(n - 1) + fib(n - 2) }\n"
                     "fn noret() { let q = 1 }\n"
                     "fn first(list) { for x in list { if x > 1 { return x } }\n return nil }\n"
                     "log(fib(15))\nlog(noret())\nlog(first([0, 1, 2, 3]))\nlog(first([0]))\n"
                     "let g = 1\nfn bump() { g += 1 }\nbump()\nbump()\nlog(g)\n"
                     "let v = 1\nif true { let v = 2\n log(v) }\nlog(v)\n");
        CHECK(r->parsed && r->inited);
        CHECK_EQ(r->host.logs.size(), (size_t)7);
        CHECK_EQ(r->host.logs[0], std::string("t: 610"));
        CHECK_EQ(r->host.logs[1], std::string("t: nil"));
        CHECK_EQ(r->host.logs[2], std::string("t: 2"));
        CHECK_EQ(r->host.logs[3], std::string("t: nil"));
        CHECK_EQ(r->host.logs[4], std::string("t: 3"));
        CHECK_EQ(r->host.logs[5], std::string("t: 2"));
        CHECK_EQ(r->host.logs[6], std::string("t: 1"));
    }
    {  // lists have reference semantics; nested assignment
        auto r = Run("let a = [1, [2, 3]]\nlet b = a\nb[0] = 9\na[1][0] += 10\nlog(a)\nlog(len(a))\nlog(a[1][1])");
        CHECK(r->host.logs.size() == 3 && r->host.logs[0] == "t: [9, [12, 3]]" && r->host.logs[1] == "t: 2" && r->host.logs[2] == "t: 3");
    }
    {  // newline handling
        auto r = Run("let t = [1,\n 2,\n 3,\n]\nlog(\n  len(t),\n  \"x\",\n)\nlet z = 1 +\n 2\nlog(z); log(z + 1)\n// comment\n# comment\nlog(\"done\") # trailing\n");
        CHECK(r->parsed && r->inited);
        CHECK_EQ(r->host.logs.size(), (size_t)4);
        CHECK_EQ(r->host.logs[0], std::string("t: 3 x"));
        CHECK_EQ(r->host.logs[1], std::string("t: 3"));
        CHECK_EQ(r->host.logs[2], std::string("t: 4"));
        CHECK_EQ(r->host.logs[3], std::string("t: done"));
    }
    {  // BOM and CRLF
        auto r = Run("\xEF\xBB\xBFlog(1)\r\nlog(2)\r\n");
        CHECK(r->parsed && r->host.logs.size() == 2);
    }

    // ---------- built-in functions ----------
    CHECK_EQ(Eval("format(\"%d|%5d|%05d|%x|%X|%08X\", 42, 42, 42, 255, 255, 255)"), std::string("42|   42|00042|ff|FF|000000FF"));
    CHECK_EQ(Eval("format(\"%.2f|%f|%6.1f|%s|%-4s|%%\", 3.14159, 1.5, 2.25, \"a\", \"b\")"), std::string("3.14|1.500000|   2.2|a|b   |%"));
    CHECK_EQ(Eval("format(\"%s\", [1, \"x\"])"), std::string("[1, \"x\"]"));
    CHECK_EQ(Eval("round(2.567, 2)"), std::string("2.57"));
    CHECK_EQ(Eval("round(2.5)"), std::string("3"));
    CHECK_EQ(Eval("floor(2.9) + ceil(2.1)"), std::string("5"));
    CHECK_EQ(Eval("min(3, 1, 2) + max(3, 1, 2)"), std::string("4"));
    CHECK_EQ(Eval("clamp(15, 0, 10)"), std::string("10"));
    CHECK_EQ(Eval("abs(-3) + sqrt(16) + pow(2, 3)"), std::string("15"));
    CHECK_EQ(Eval("bytes(\"90 0xEB 0F\")"), std::string("[144, 235, 15]"));
    CHECK_EQ(Eval("u32_bytes(0x12345678)"), std::string("[120, 86, 52, 18]"));
    CHECK_EQ(Eval("u16_bytes(0x1234)"), std::string("[52, 18]"));
    CHECK_EQ(Eval("f32_bytes(1.0)"), std::string("[0, 0, 128, 63]"));
    CHECK_EQ(Eval("join([1, 2, 3], \"-\")"), std::string("1-2-3"));
    CHECK_EQ(Eval("contains(\"hello\", \"ell\") && contains([1, 2], 2) && !contains([1], 5)"), std::string("true"));
    CHECK_EQ(Eval("upper(\"ab\") + lower(\"CD\")"), std::string("ABcd"));
    CHECK_EQ(Eval("num(\"1.5\") + num(\"0x10\") + num(true)"), std::string("18.5"));
    CHECK_EQ(Eval("num(\"abc\")"), std::string("nil"));
    CHECK_EQ(Eval("str(12) + type(1) + type(\"s\") + type([]) + type(nil) + type(true)"), std::string("12numberstringlistnilbool"));
    CHECK_EQ(Eval("len(range(2, 6))"), std::string("4"));

    // ---------- the game (mock host) ----------
    {
        auto r = Run("log(set_speed(2), speed(), tainted())\nstatus(\"hello\")\ntaint(\"because\")\nlog(set_fps(240), fps(), guard_active())\n"
                     "log(format(\"0x%X 0x%X\", gd_base(), gd_stamp()), now_ms())", "modslug");
        CHECK(r->parsed && r->inited);
        CHECK_EQ(r->host.logs[0], std::string("modslug: true 2 true"));
        CHECK_EQ(r->host.status["modslug"], std::string("hello"));
        CHECK_EQ(r->host.taintReason, std::string("because"));
        CHECK_EQ(r->host.logs[1], std::string("modslug: true 240 true"));
        CHECK_EQ(r->host.logs[2], std::string("modslug: 0x400000 0xABCD1234 1000"));
    }
    {  // memory: scan / patch / nop / read
        auto r0 = std::make_unique<Ran>();
        r0->host.scanResult = 0x500000;
        for (int i = 0; i < 16; ++i) r0->host.mem[0x500000 + i] = static_cast<uint8_t>(i);
        r0->rt = std::make_unique<gocode::Runtime>(&r0->host, "m", "m");
        std::string src = "let a = scan(\"8B 45 ?? 85\")\nlog(a)\nlog(patch(a + 2, [0xAA, 0xBB]), read_bytes(a, 5))\nnop(a + 8, 3)\n"
                          "log(read_u8(a + 8), read_u16(a), read_u32(a), read_bytes(a + 8, 4), read_u8(0x1))\npatch(a + 12, f32_bytes(2.5))\nlog(read_f32(a + 12))";
        r0->parsed = r0->rt->Parse(src, r0->parseErrors);
        CHECK(r0->parsed);
        r0->rt->Init();
        CHECK_EQ(r0->host.logs.size(), (size_t)4);
        CHECK_EQ(r0->host.logs[0], std::string("m: 5242880"));
        CHECK_EQ(r0->host.logs[1], std::string("m: true [0, 1, 170, 187, 4]"));
        CHECK_EQ(r0->host.logs[2], std::string("m: 144 256 3148480768 [144, 144, 144, 11] nil"));
        CHECK_EQ(r0->host.logs[3], std::string("m: 2.5"));
    }
    {  // cfg()
        auto r0 = std::make_unique<Ran>();
        r0->host.config["c.n"] = "2.5";
        r0->host.config["c.b"] = "yes";
        r0->host.config["c.s"] = "text";
        r0->host.config["c.bad"] = "oops";
        r0->rt = std::make_unique<gocode::Runtime>(&r0->host, "c", "c");
        r0->rt->Parse("log(cfg(\"n\", 1), cfg(\"b\", false), cfg(\"s\", \"d\"), cfg(\"bad\", 7), cfg(\"missing\", 9), cfg(\"missing\"))", r0->parseErrors);
        r0->rt->Init();
        CHECK_EQ(r0->host.logs.back(), std::string("c: 2.5 true text 7 9 nil"));
    }

    // ---------- events ----------
    {
        auto r = Run("let n = 0\non start { n = 100 }\non key F5 { n += 1 }\non key ctrl+shift+a { n += 10 }\non key 7 { n += 1000 }\n"
                     "on tick 500ms { n += 1 }\non tick 2s { log(n) }\non tick 250 { }\non key alt+space { }\non key numplus { }\n");
        CHECK(r->parsed && r->inited);
        auto& ev = r->rt->events();
        CHECK_EQ(ev.size(), (size_t)9);
        CHECK(ev[0].kind == gocode::EventInfo::Start);
        CHECK(ev[1].kind == gocode::EventInfo::Key && ev[1].vk == 0x74 && ev[1].mods == 0);
        CHECK(ev[2].vk == 'A' && ev[2].mods == 3);
        CHECK(ev[3].vk == '7');
        CHECK(ev[4].kind == gocode::EventInfo::Tick && ev[4].intervalMs == 500);
        CHECK(ev[5].intervalMs == 2000);
        CHECK(ev[6].intervalMs == 250);
        CHECK(ev[7].vk == 0x20 && ev[7].mods == 4);
        CHECK(ev[8].vk == 0x6B);
        CHECK(r->rt->RunEvent(0));
        CHECK(r->rt->RunEvent(1));
        CHECK(r->rt->RunEvent(2));
        CHECK(r->rt->RunEvent(3));
        CHECK(r->rt->RunEvent(5));
        CHECK_EQ(r->host.logs.back(), std::string("t: 1111"));
        CHECK(!r->rt->RunEvent(99));
    }
    {  // a handler that keeps failing is switched off after 5 errors
        auto r = Run("on key F1 { log(undefined_thing) }");
        CHECK(r->parsed && r->inited);
        for (int i = 0; i < 5; ++i) CHECK(!r->rt->RunEvent(0));
        CHECK(r->host.Logged("undefined variable 'undefined_thing'"));
        CHECK(r->host.Logged("switched off"));
        size_t before = r->host.logs.size();
        CHECK(!r->rt->RunEvent(0));
        CHECK_EQ(r->host.logs.size(), before);
    }
    {  // metadata
        auto r = Run("mod \"My Mod\"\nauthor \"Me\"\nversion \"2.0\"\nrequire_stamp 0xDEADBEEF\nlet version = 3\nlog(version)\n");
        CHECK(r->parsed && r->rt->name() == "My Mod" && r->rt->author() == "Me" && r->rt->version() == "2.0");
        CHECK(r->rt->hasStamp() && r->rt->stamp() == 0xDEADBEEFu);
        CHECK_EQ(r->host.logs.back(), std::string("t: 3"));
        auto r2 = Run("log(1)");
        CHECK(!r2->rt->hasStamp() && r2->rt->name().empty());
    }

    // ---------- errors: syntax ----------
    CHECK_EQ(ErrOf("log(\"abc)"), std::string("line 1: unterminated string"));
    CHECK_EQ(ErrOf("let x = 1\nlet y = @"), std::string("line 2: unexpected character '@'"));
    CHECK_EQ(ErrOf("if true {\nlog(1)\n"), std::string("line 3: missing '}' (block never closed)"));
    CHECK_EQ(ErrOf("on foo { }"), std::string("line 1: unknown event 'foo' (use start, key or tick)"));
    CHECK_EQ(ErrOf("on key Banana { }"), std::string("line 1: unknown key 'Banana'"));
    CHECK_EQ(ErrOf("on key ctrl { }"), std::string("line 1: hotkey needs a key (like F5 or ctrl+K)"));
    CHECK_EQ(ErrOf("on key a+b { }"), std::string("line 1: a hotkey can have only one main key (found 'b' as well)"));
    CHECK_EQ(ErrOf("on tick 5ms { }"), std::string("line 1: tick interval must be at least 10ms"));
    CHECK_EQ(ErrOf("on tick { }"), std::string("line 1: expected an interval after 'on tick' (like 500ms or 1s)"));
    CHECK_EQ(ErrOf("if true { fn f() { } }"), std::string("line 1: 'fn' can only be used at the top level of a script"));
    CHECK_EQ(ErrOf("fn log() { }"), std::string("line 1: 'log' is a built-in function and cannot be redefined"));
    CHECK_EQ(ErrOf("fn a() { }\nfn a() { }"), std::string("line 2: function 'a' is already defined"));
    CHECK_EQ(ErrOf("let = 5"), std::string("line 1: expected a variable name but found '='"));
    CHECK_EQ(ErrOf("let x 5"), std::string("line 1: expected '=' but found number '5'"));
    CHECK_EQ(ErrOf("log(1 2)"), std::string("line 1: expected ')' but found number '2'"));
    CHECK_EQ(ErrOf("1 = 2"), std::string("line 1: left side of '=' must be a variable or list element"));
    CHECK_EQ(ErrOf("let a = [1\nlet b = 2"), std::string("line 2: expected ']' but found 'let'"));
    CHECK_EQ(ErrOf("(1)(2)"), std::string("line 1: only named functions can be called"));
    CHECK_EQ(ErrOf("let a = 0x"), std::string("line 1: malformed hex number"));
    CHECK_EQ(ErrOf("for x range(3) { }"), std::string("line 1: expected 'in' in for loop"));
    CHECK_EQ(ErrOf("let x = 1 2"), std::string("line 1: unexpected number '2' (expected end of line)"));

    // ---------- errors: runtime (reported with line numbers, never a crash) ----------
    CHECK_EQ(ErrOf("let a = 1\nlog(b)"), std::string("error at line 2: undefined variable 'b'"));
    CHECK_EQ(ErrOf("x = 1"), std::string("error at line 1: undefined variable 'x' (declare it with let)"));
    CHECK_EQ(ErrOf("log(1 / 0)"), std::string("error at line 1: division by zero"));
    CHECK_EQ(ErrOf("log(5 % 0)"), std::string("error at line 1: division by zero"));
    CHECK_EQ(ErrOf("log(\"a\" - 1)"), std::string("error at line 1: '-' needs two numbers, got string and number"));
    CHECK_EQ(ErrOf("log(nil + 1)"), std::string("error at line 1: cannot add nil and number"));
    CHECK_EQ(ErrOf("log(1 < \"a\")"), std::string("error at line 1: cannot compare number and string"));
    CHECK_EQ(ErrOf("let l = [1]\nlog(l[3])"), std::string("error at line 2: index 3 is out of range (size 1)"));
    CHECK_EQ(ErrOf("let l = [1]\nlog(l[-1])"), std::string("error at line 2: index -1 is out of range (size 1)"));
    CHECK_EQ(ErrOf("let l = [1]\nlog(l[0.5])"), std::string("error at line 2: index must be a whole number"));
    CHECK_EQ(ErrOf("log(5[0])"), std::string("error at line 1: cannot index a number"));
    CHECK_EQ(ErrOf("log(nope(1))"), std::string("error at line 1: unknown function 'nope'"));
    CHECK_EQ(ErrOf("fn f(a) { }\nf(1, 2)"), std::string("error at line 2: f() takes 1 argument(s), got 2"));
    CHECK_EQ(ErrOf("log(abs())"), std::string("error at line 1: abs() takes 1 argument(s), got 0"));
    CHECK_EQ(ErrOf("log(abs(\"x\"))"), std::string("error at line 1: abs() argument 1 must be a number, got string"));
    CHECK_EQ(ErrOf("patch(1, [300])"), std::string("error at line 1: patch(): every byte must be a whole number from 0 to 255"));
    CHECK_EQ(ErrOf("patch(-1, [1])"), std::string("error at line 1: patch(): address must be a whole number between 0 and 0xFFFFFFFF"));
    CHECK_EQ(ErrOf("patch(1, 5)"), std::string("error at line 1: patch() needs a list of bytes, e.g. [0x90, 0x90]"));
    CHECK_EQ(ErrOf("log(format(\"%d\"))"), std::string("error at line 1: format(): not enough arguments for the format string"));
    CHECK_EQ(ErrOf("log(format(\"%q\", 1))"), std::string("error at line 1: format(): unknown conversion '%q' (use %d %f %s %x %X %%)"));
    CHECK_EQ(ErrOf("for x in 5 { }"), std::string("error at line 1: cannot loop over a number"));
    CHECK_EQ(ErrOf("fn f() { break }\nf()"), std::string("error at line 2: break/continue outside of a loop"));
    CHECK_EQ(ErrOf("while true { }"), std::string("error at line 1: script ran too long (possible endless loop)"));
    CHECK_EQ(ErrOf("fn f(n) { return f(n + 1) }\nf(0)"), std::string("error at line 1: function calls nested too deeply (endless recursion?)"));
    CHECK_EQ(ErrOf("let l = []\nwhile true { push(l, 1) }"), std::string("error at line 2: list too large"));
    {  // a runtime error in init stops that script's init but never throws out
        auto r = Run("log(1)\nlog(1/0)\nlog(3)");
        CHECK(r->parsed && !r->inited && r->host.logs.size() == 2);
    }
    {  // memory read of unmapped address returns nil, not an error
        auto r = Run("log(read_u8(0x1234), read_bytes(0x10, 4), read_f32(0))");
        CHECK_EQ(r->host.logs.back(), std::string("t: nil nil nil"));
    }

    // ---------- key combos ----------
    {
        uint32_t vk, mods;
        std::string err;
        CHECK(gocode::ParseKeyCombo({"Ctrl", "SHIFT", "F12"}, vk, mods, err) && vk == 0x7B && mods == 3);
        CHECK(gocode::ParseKeyCombo({"f1"}, vk, mods, err) && vk == 0x70 && mods == 0);
        CHECK(gocode::ParseKeyCombo({"F24"}, vk, mods, err) && vk == 0x87);
        CHECK(!gocode::ParseKeyCombo({"F25"}, vk, mods, err));
        CHECK(!gocode::ParseKeyCombo({"F"}, vk, mods, err) == false);  // plain letter F is fine
        CHECK(gocode::ParseKeyCombo({"num5"}, vk, mods, err) && vk == 0x65);
        CHECK(gocode::ParseKeyCombo({"escape"}, vk, mods, err) && vk == 0x1B);
        CHECK(!gocode::ParseKeyCombo({"F1x"}, vk, mods, err));
        CHECK(!gocode::ParseKeyCombo({}, vk, mods, err));
    }

    // ---------- patterns ----------
    {
        gocode::Pattern p;
        CHECK(gocode::parsePattern("8B 45 ?? 85 C0", p) && p.bytes.size() == 5 && p.bytes[2] == -1);
        CHECK(!gocode::parsePattern("", p));
        CHECK(!gocode::parsePattern("8B GG", p));
        const uint8_t mem[] = {0x00, 0x11, 0x8B, 0x45, 0x10, 0x85, 0xC0, 0x90};
        gocode::parsePattern("8B 45 ?? 85 C0", p);
        CHECK(gocode::findPattern(mem, sizeof mem, p) == 2);
        gocode::parsePattern("85 C0 90", p);
        CHECK(gocode::findPattern(mem, sizeof mem, p) == 5);
        gocode::parsePattern("C0 90 91", p);
        CHECK(gocode::findPattern(mem, sizeof mem, p) == gocode::npos);
    }

    // ---------- the shipped mods, driven like the loader drives them ----------
    {  // speedhack
        auto r0 = std::make_unique<Ran>();
        r0->rt = std::make_unique<gocode::Runtime>(&r0->host, "speedhack", "speedhack");
        CHECK(r0->rt->Parse(ReadFile(root + "/mods/speedhack.gocode"), r0->parseErrors));
        CHECK(r0->parseErrors.empty());
        CHECK(r0->rt->Init());
        CHECK_EQ(r0->rt->name(), std::string("Speedhack"));
        int f5 = FindKey(*r0->rt, 0x74, 0), f6 = FindKey(*r0->rt, 0x75, 0), f7 = FindKey(*r0->rt, 0x76, 0), tk = FindTick(*r0->rt);
        CHECK(f5 >= 0 && f6 >= 0 && f7 >= 0 && tk >= 0);
        r0->rt->SetLabel("Speedhack");
        r0->rt->RunEvent(f6);
        CHECK_EQ(r0->host.speed, 1.1);
        CHECK(r0->host.tainted);
        CHECK_EQ(r0->host.status["Speedhack"], std::string("speed 1.10x"));
        r0->rt->RunEvent(f6);
        r0->rt->RunEvent(f6);
        CHECK_EQ(r0->host.speed, 1.3);
        r0->rt->RunEvent(f5);
        CHECK_EQ(r0->host.speed, 1.2);
        r0->rt->RunEvent(f7);
        CHECK_EQ(r0->host.speed, 1.0);
        CHECK_EQ(r0->host.status["Speedhack"], std::string(""));
        CHECK(r0->host.tainted);  // stays tainted
        r0->host.config["speedhack.speed"] = "2.5";
        r0->rt->RunEvent(tk);
        CHECK_EQ(r0->host.speed, 2.5);
        r0->rt->RunEvent(tk);  // unchanged cfg: no second change
        size_t n = r0->host.logs.size();
        r0->rt->RunEvent(tk);
        CHECK_EQ(r0->host.logs.size(), n);
        // guard missing: refused
        auto r1 = std::make_unique<Ran>();
        r1->host.guard = false;
        r1->rt = std::make_unique<gocode::Runtime>(&r1->host, "speedhack", "speedhack");
        r1->rt->Parse(ReadFile(root + "/mods/speedhack.gocode"), r1->parseErrors);
        r1->rt->Init();
        r1->rt->RunEvent(FindKey(*r1->rt, 0x75, 0));
        CHECK_EQ(r1->host.speed, 1.0);
        CHECK(!r1->host.tainted);
        CHECK(r1->host.Logged("was refused"));
        // custom step from cfg
        auto r2 = std::make_unique<Ran>();
        r2->host.config["speedhack.speed_step"] = "0.25";
        r2->rt = std::make_unique<gocode::Runtime>(&r2->host, "speedhack", "speedhack");
        r2->rt->Parse(ReadFile(root + "/mods/speedhack.gocode"), r2->parseErrors);
        r2->rt->Init();
        r2->rt->RunEvent(FindKey(*r2->rt, 0x75, 0));
        CHECK_EQ(r2->host.speed, 1.25);
    }
    {  // fps-bypass
        auto r = std::make_unique<Ran>();
        r->rt = std::make_unique<gocode::Runtime>(&r->host, "fps-bypass", "fps-bypass");
        CHECK(r->rt->Parse(ReadFile(root + "/mods/fps-bypass.gocode"), r->parseErrors));
        CHECK(r->rt->Init());
        r->rt->SetLabel("FPS Bypass");
        int f8 = FindKey(*r->rt, 0x77, 0), sf8 = FindKey(*r->rt, 0x77, 2), tk = FindTick(*r->rt);
        CHECK(f8 >= 0 && sf8 >= 0 && tk >= 0);
        double expect[] = {60, 120, 240, 360, 480, 1000, 0, 60};
        for (double e : expect) {
            r->rt->RunEvent(f8);
            CHECK_EQ(r->host.fps, e);
        }
        CHECK(!r->host.tainted);  // FPS alone does not taint
        CHECK_EQ(r->host.status["FPS Bypass"], std::string("60 FPS"));
        r->rt->RunEvent(sf8);
        CHECK_EQ(r->host.fps, 0.0);
        CHECK_EQ(r->host.status["FPS Bypass"], std::string(""));
        r->host.config["fps-bypass.fps"] = "777";
        r->rt->RunEvent(tk);
        CHECK_EQ(r->host.fps, 777.0);
        CHECK_EQ(r->host.status["FPS Bypass"], std::string("777 FPS"));
        r->rt->RunEvent(f8);  // 777 is not a preset -> first preset
        CHECK_EQ(r->host.fps, 60.0);
        r->host.config["fps-bypass.fps"] = "0";
        r->rt->RunEvent(tk);
        CHECK_EQ(r->host.fps, 0.0);
        // frame-rate control unavailable
        r->host.fpsFail = 1;
        r->rt->RunEvent(f8);
        CHECK(r->host.Logged("frame-rate control is unavailable"));
    }
    {  // safe-mode
        auto r = std::make_unique<Ran>();
        r->rt = std::make_unique<gocode::Runtime>(&r->host, "safe-mode", "safe-mode");
        CHECK(r->rt->Parse(ReadFile(root + "/mods/safe-mode.gocode"), r->parseErrors) && r->rt->Init());
        r->rt->SetLabel("Safe Mode");
        r->rt->RunEvent(FindStart(*r->rt));
        CHECK_EQ(r->host.status["Safe Mode"], std::string("safe mode armed"));
        r->host.tainted = true;
        r->rt->RunEvent(FindTick(*r->rt));
        CHECK_EQ(r->host.status["Safe Mode"], std::string(""));
        auto r2 = std::make_unique<Ran>();
        r2->host.guard = false;
        r2->rt = std::make_unique<gocode::Runtime>(&r2->host, "safe-mode", "safe-mode");
        r2->rt->Parse(ReadFile(root + "/mods/safe-mode.gocode"), r2->parseErrors);
        r2->rt->Init();
        r2->rt->SetLabel("Safe Mode");
        r2->rt->RunEvent(FindStart(*r2->rt));
        CHECK_EQ(r2->host.status["Safe Mode"], std::string("safe mode OFFLINE: speed locked"));
    }
    {  // examples
        for (const char* f : {"/examples/hello.gocode", "/examples/memory-patch.gocode"}) {
            auto r = std::make_unique<Ran>();
            r->rt = std::make_unique<gocode::Runtime>(&r->host, "ex", "ex");
            std::string src = ReadFile(root + f);
            CHECK(!src.empty());
            CHECK(r->rt->Parse(src, r->parseErrors));
            if (!r->parseErrors.empty()) printf("   %s: %s\n", f, r->parseErrors[0].c_str());
            CHECK(r->rt->Init());
        }
        auto r = std::make_unique<Ran>();
        r->host.scanResult = 0x600000;
        for (int i = 0; i < 64; ++i) r->host.mem[0x600000 + i] = 0x11;
        r->rt = std::make_unique<gocode::Runtime>(&r->host, "ex", "ex");
        r->rt->Parse(ReadFile(root + "/examples/memory-patch.gocode"), r->parseErrors);
        r->rt->Init();
        r->rt->RunEvent(FindStart(*r->rt));
        CHECK(r->host.tainted);
        CHECK_EQ(r->host.patches.size(), (size_t)3);
        CHECK_EQ(r->host.patches[0].first, (uint64_t)0x600002);
        CHECK(r->host.Logged("patched at 0x600000"));
        // pattern missing -> logs and stops
        auto r3 = std::make_unique<Ran>();
        r3->rt = std::make_unique<gocode::Runtime>(&r3->host, "ex", "ex");
        r3->rt->Parse(ReadFile(root + "/examples/memory-patch.gocode"), r3->parseErrors);
        r3->rt->Init();
        r3->rt->RunEvent(FindStart(*r3->rt));
        CHECK(r3->host.Logged("pattern not found"));
        CHECK(r3->host.patches.empty() && !r3->host.tainted);
    }

    if (failures == 0) printf("All GoCode language tests passed.\n");
    return failures ? 1 : 0;
}
