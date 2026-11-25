#include "gocode_lang.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>

#include "pattern.h"

namespace gocode {

namespace {

struct ScriptError {
    std::string msg;
    int line;
};

constexpr long kMaxSteps = 3000000;  // per event / init, stops runaway loops
constexpr int kMaxDepth = 100;       // function call nesting
constexpr int kMaxHandlerErrors = 5;

// =============================================================================================
// Values
// =============================================================================================
struct Value;
using ListP = std::shared_ptr<std::vector<Value>>;

struct Value {
    enum Type { Nil, Bool, Num, Str, List } t = Nil;
    bool b = false;
    double n = 0;
    std::string s;
    ListP l;

    static Value Boolean(bool v) { Value x; x.t = Bool; x.b = v; return x; }
    static Value Number(double v) { Value x; x.t = Num; x.n = v; return x; }
    static Value String(std::string v) { Value x; x.t = Str; x.s = std::move(v); return x; }
    static Value MakeList(std::vector<Value> v) {
        Value x;
        x.t = List;
        x.l = std::make_shared<std::vector<Value>>(std::move(v));
        return x;
    }
};

const char* TypeName(const Value& v) {
    switch (v.t) {
        case Value::Nil: return "nil";
        case Value::Bool: return "bool";
        case Value::Num: return "number";
        case Value::Str: return "string";
        case Value::List: return "list";
    }
    return "?";
}

std::string NumToStr(double n) {
    if (std::isnan(n)) return "nan";
    if (std::isinf(n)) return n > 0 ? "inf" : "-inf";
    if (n == 0) return "0";
    char b[48];
    if (n == std::floor(n) && std::fabs(n) < 1e15) snprintf(b, sizeof(b), "%.0f", n);
    else snprintf(b, sizeof(b), "%.10g", n);
    return b;
}

std::string ToDisplay(const Value& v);

std::string ToRepr(const Value& v) {
    if (v.t == Value::Str) return "\"" + v.s + "\"";
    return ToDisplay(v);
}

std::string ToDisplay(const Value& v) {
    switch (v.t) {
        case Value::Nil: return "nil";
        case Value::Bool: return v.b ? "true" : "false";
        case Value::Num: return NumToStr(v.n);
        case Value::Str: return v.s;
        case Value::List: {
            std::string out = "[";
            for (size_t i = 0; i < v.l->size(); ++i) {
                if (i) out += ", ";
                out += ToRepr((*v.l)[i]);
            }
            return out + "]";
        }
    }
    return "";
}

bool Truthy(const Value& v) { return !(v.t == Value::Nil || (v.t == Value::Bool && !v.b)); }

bool Equals(const Value& a, const Value& b) {
    if (a.t != b.t) return false;
    switch (a.t) {
        case Value::Nil: return true;
        case Value::Bool: return a.b == b.b;
        case Value::Num: return a.n == b.n;
        case Value::Str: return a.s == b.s;
        case Value::List:
            if (a.l == b.l) return true;
            if (a.l->size() != b.l->size()) return false;
            for (size_t i = 0; i < a.l->size(); ++i)
                if (!Equals((*a.l)[i], (*b.l)[i])) return false;
            return true;
    }
    return false;
}

// =============================================================================================
// Lexer
// =============================================================================================
struct Token {
    enum Type { End, Newline, Num, Dur, Str, Ident, Punct } t = End;
    std::string s;  // Ident / Punct / Str contents; original text for numbers
    double n = 0;   // Num value, or milliseconds for Dur
    int line = 0;
};

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::vector<Token> Lex(const std::string& src) {
    std::vector<Token> out;
    int line = 1;
    size_t i = 0, n = src.size();
    if (n >= 3 && static_cast<uint8_t>(src[0]) == 0xEF && static_cast<uint8_t>(src[1]) == 0xBB &&
        static_cast<uint8_t>(src[2]) == 0xBF)
        i = 3;  // UTF-8 BOM
    auto push = [&](Token::Type t, const std::string& s, double num = 0) {
        Token k;
        k.t = t;
        k.s = s;
        k.n = num;
        k.line = line;
        out.push_back(k);
    };
    while (i < n) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '\n') { push(Token::Newline, "\n"); ++line; ++i; continue; }
        if (c == ';') { push(Token::Newline, ";"); ++i; continue; }
        if (c == '#' || (c == '/' && i + 1 < n && src[i + 1] == '/')) {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i;
            double value = 0;
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                i += 2;
                size_t digits = i;
                while (i < n && std::isxdigit(static_cast<unsigned char>(src[i]))) ++i;
                if (i == digits) throw ScriptError{"malformed hex number", line};
                value = static_cast<double>(std::strtoull(src.substr(digits, i - digits).c_str(), nullptr, 16));
                push(Token::Num, src.substr(start, i - start), value);
                continue;
            }
            while (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
            if (i + 1 < n && src[i] == '.' && std::isdigit(static_cast<unsigned char>(src[i + 1]))) {
                ++i;
                while (i < n && std::isdigit(static_cast<unsigned char>(src[i]))) ++i;
            }
            std::string text = src.substr(start, i - start);
            value = std::strtod(text.c_str(), nullptr);
            // duration suffixes: 500ms, 2s
            if (i + 1 < n && src[i] == 'm' && src[i + 1] == 's' && !(i + 2 < n && IsIdentChar(src[i + 2]))) {
                i += 2;
                push(Token::Dur, text, value);
            } else if (i < n && src[i] == 's' && !(i + 1 < n && IsIdentChar(src[i + 1]))) {
                i += 1;
                push(Token::Dur, text, value * 1000.0);
            } else {
                push(Token::Num, text, value);
            }
            continue;
        }
        if (IsIdentStart(c)) {
            size_t start = i;
            while (i < n && IsIdentChar(src[i])) ++i;
            push(Token::Ident, src.substr(start, i - start));
            continue;
        }
        if (c == '"') {
            ++i;
            std::string s;
            bool closed = false;
            while (i < n) {
                char d = src[i];
                if (d == '\n') break;
                if (d == '"') { closed = true; ++i; break; }
                if (d == '\\' && i + 1 < n) {
                    char e = src[i + 1];
                    switch (e) {
                        case 'n': s += '\n'; break;
                        case 't': s += '\t'; break;
                        case 'r': s += '\r'; break;
                        case '"': s += '"'; break;
                        case '\\': s += '\\'; break;
                        default: s += e; break;
                    }
                    i += 2;
                    continue;
                }
                s += d;
                ++i;
            }
            if (!closed) throw ScriptError{"unterminated string", line};
            push(Token::Str, s);
            continue;
        }
        static const char* two[] = {"==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/="};
        bool matched = false;
        if (i + 1 < n) {
            for (const char* op : two) {
                if (src[i] == op[0] && src[i + 1] == op[1]) {
                    push(Token::Punct, op);
                    i += 2;
                    matched = true;
                    break;
                }
            }
        }
        if (matched) continue;
        if (std::strchr("+-*/%=<>!(){}[],", c)) {
            push(Token::Punct, std::string(1, c));
            ++i;
            continue;
        }
        throw ScriptError{std::string("unexpected character '") + c + "'", line};
    }
    push(Token::End, "");
    return out;
}

// =============================================================================================
// AST
// =============================================================================================
struct Expr;
using ExprP = std::unique_ptr<Expr>;
struct Stmt;
using StmtP = std::unique_ptr<Stmt>;
using Block = std::vector<StmtP>;

struct Expr {
    enum Kind { Lit, Var, ListLit, Unary, Binary, Index, Call } kind = Lit;
    int line = 0;
    Value lit;
    std::string name;  // Var name, Call name, operator for Unary/Binary
    ExprP a, b;        // operands; Index: a[b]
    std::vector<ExprP> items;  // list items / call arguments
};

struct Stmt {
    enum Kind { Let, Assign, If, While, For, Return, Break, Continue, ExprS } kind = ExprS;
    int line = 0;
    std::string name;  // Let: variable; For: loop variable; Assign: operator ("=", "+=", ...)
    ExprP target;      // Assign: Var or Index expression
    ExprP value;       // Let / Assign / Return / ExprS value; While: condition; For: iterable
    std::vector<std::pair<ExprP, Block>> branches;  // If / else-if chain
    Block body;                                     // While / For body; If: the else block
    bool hasElse = false;
};

struct Func {
    std::string name;
    std::vector<std::string> params;
    Block body;
    int line = 0;
};

struct EventDef {
    EventInfo info;
    Block body;
};

struct Program {
    std::string name, author, version;
    bool hasStamp = false;
    uint32_t stamp = 0;
    std::map<std::string, Func> funcs;
    std::vector<EventDef> events;
    Block top;
};

// Names that are built in (a script may not define a function with one of these names).
const char* const kBuiltinNames[] = {
    "log", "str", "num", "len", "type", "abs", "floor", "ceil", "round", "sqrt", "pow", "min", "max", "clamp",
    "format", "push", "range", "bytes", "join", "contains", "lower", "upper", "u16_bytes", "u32_bytes", "f32_bytes",
    "speed", "set_speed", "fps", "set_fps", "tainted", "taint", "guard_active", "status", "gd_base", "gd_stamp",
    "now_ms", "scan", "patch", "nop", "read_bytes", "read_u8", "read_u16", "read_u32", "read_f32", "cfg"};

bool IsBuiltinName(const std::string& n) {
    for (const char* b : kBuiltinNames)
        if (n == b) return true;
    return false;
}

bool IsReserved(const std::string& n) {
    static const char* const kw[] = {"let", "fn", "if", "else", "while", "for", "in", "return", "break",
                                     "continue", "on", "true", "false", "nil"};
    for (const char* k : kw)
        if (n == k) return true;
    return false;
}

// =============================================================================================
// Parser
// =============================================================================================
class Parser {
public:
    Parser(std::vector<Token> toks, Program& prog) : t_(std::move(toks)), prog_(prog) {}

    void ParseProgram() {
        for (;;) {
            SkipNewlines();
            if (Peek().t == Token::End) break;
            ParseTop();
        }
    }

private:
    std::vector<Token> t_;
    size_t p_ = 0;
    Program& prog_;

    const Token& Peek() const { return t_[p_]; }
    const Token& Next() { const Token& k = t_[p_]; if (p_ + 1 < t_.size()) ++p_; return k; }
    [[noreturn]] void Fail(const std::string& msg, int line = -1) const { throw ScriptError{msg, line < 0 ? Peek().line : line}; }
    bool IsPunct(const char* s) const { return Peek().t == Token::Punct && Peek().s == s; }
    bool IsIdent(const char* s) const { return Peek().t == Token::Ident && Peek().s == s; }
    bool AcceptPunct(const char* s) { if (IsPunct(s)) { Next(); return true; } return false; }
    void ExpectPunct(const char* s) {
        if (!AcceptPunct(s)) Fail(std::string("expected '") + s + "' but found " + Describe(Peek()));
    }
    void SkipNewlines() { while (Peek().t == Token::Newline) Next(); }

    static std::string Describe(const Token& k) {
        switch (k.t) {
            case Token::End: return "end of file";
            case Token::Newline: return "end of line";
            case Token::Str: return "a string";
            case Token::Num: case Token::Dur: return "number '" + k.s + "'";
            default: return "'" + k.s + "'";
        }
    }

    std::string ExpectIdent(const char* what) {
        if (Peek().t != Token::Ident || IsReserved(Peek().s)) Fail(std::string("expected ") + what + " but found " + Describe(Peek()));
        return Next().s;
    }

    void EndOfStatement() {
        if (Peek().t == Token::Newline) { Next(); return; }
        if (Peek().t == Token::End || IsPunct("}")) return;
        Fail("unexpected " + Describe(Peek()) + " (expected end of line)");
    }

    // ---- top level ----
    void ParseTop() {
        const Token& k = Peek();
        if (k.t == Token::Ident) {
            const std::string& w = k.s;
            const Token& nx = t_[std::min(p_ + 1, t_.size() - 1)];
            if ((w == "mod" || w == "author" || w == "version") && nx.t == Token::Str) {
                Next();
                std::string v = Next().s;
                (w == "mod" ? prog_.name : w == "author" ? prog_.author : prog_.version) = v;
                EndOfStatement();
                return;
            }
            if (w == "require_stamp" && nx.t == Token::Num) {
                Next();
                prog_.hasStamp = true;
                prog_.stamp = static_cast<uint32_t>(Next().n);
                EndOfStatement();
                return;
            }
            if (w == "fn") { ParseFunc(); return; }
            if (w == "on") { ParseEvent(); return; }
        }
        prog_.top.push_back(ParseStatement());
    }

    void ParseFunc() {
        int line = Next().line;  // fn
        Func f;
        f.line = line;
        f.name = ExpectIdent("a function name");
        if (IsBuiltinName(f.name)) Fail("'" + f.name + "' is a built-in function and cannot be redefined", line);
        if (prog_.funcs.count(f.name)) Fail("function '" + f.name + "' is already defined", line);
        ExpectPunct("(");
        SkipNewlines();
        if (!IsPunct(")")) {
            for (;;) {
                f.params.push_back(ExpectIdent("a parameter name"));
                SkipNewlines();
                if (AcceptPunct(",")) { SkipNewlines(); continue; }
                break;
            }
        }
        ExpectPunct(")");
        f.body = ParseBlock();
        EndOfStatement();
        std::string name = f.name;
        prog_.funcs.emplace(name, std::move(f));
    }

    void ParseEvent() {
        int line = Next().line;  // on
        EventDef ev;
        ev.info.line = line;
        if (Peek().t != Token::Ident) Fail("expected 'start', 'key' or 'tick' after 'on'");
        std::string kind = Next().s;
        if (kind == "start") {
            ev.info.kind = EventInfo::Start;
        } else if (kind == "key") {
            ev.info.kind = EventInfo::Key;
            std::vector<std::string> parts;
            for (;;) {
                if (Peek().t != Token::Ident && Peek().t != Token::Num) Fail("expected a key name (like F5 or ctrl+shift+K)");
                parts.push_back(Next().s);
                if (AcceptPunct("+")) continue;
                break;
            }
            std::string err;
            if (!ParseKeyCombo(parts, ev.info.vk, ev.info.mods, err)) Fail(err, line);
        } else if (kind == "tick") {
            ev.info.kind = EventInfo::Tick;
            if (Peek().t != Token::Dur && Peek().t != Token::Num) Fail("expected an interval after 'on tick' (like 500ms or 1s)");
            double ms = Next().n;
            if (ms < 10) Fail("tick interval must be at least 10ms", line);
            if (ms > 3600000) Fail("tick interval must be at most 1 hour", line);
            ev.info.intervalMs = static_cast<uint32_t>(ms);
        } else {
            Fail("unknown event '" + kind + "' (use start, key or tick)", line);
        }
        ev.body = ParseBlock();
        EndOfStatement();
        prog_.events.push_back(std::move(ev));
    }

    // ---- statements ----
    Block ParseBlock() {
        ExpectPunct("{");
        Block b;
        for (;;) {
            SkipNewlines();
            if (Peek().t == Token::End) Fail("missing '}' (block never closed)");
            if (AcceptPunct("}")) break;
            b.push_back(ParseStatement());
        }
        return b;
    }

    StmtP ParseStatement() {
        auto s = std::make_unique<Stmt>();
        s->line = Peek().line;
        if (Peek().t == Token::Ident) {
            const std::string w = Peek().s;
            if (w == "let") {
                Next();
                s->kind = Stmt::Let;
                s->name = ExpectIdent("a variable name");
                ExpectPunct("=");
                s->value = ParseExpr();
                EndOfStatement();
                return s;
            }
            if (w == "if") {
                Next();
                s->kind = Stmt::If;
                for (;;) {
                    ExprP cond = ParseExpr();
                    Block blk = ParseBlock();
                    s->branches.emplace_back(std::move(cond), std::move(blk));
                    size_t save = p_;
                    SkipNewlines();
                    if (IsIdent("else")) {
                        Next();
                        if (IsIdent("if")) { Next(); continue; }
                        s->hasElse = true;
                        s->body = ParseBlock();
                    } else {
                        p_ = save;
                    }
                    break;
                }
                EndOfStatement();
                return s;
            }
            if (w == "while") {
                Next();
                s->kind = Stmt::While;
                s->value = ParseExpr();
                s->body = ParseBlock();
                EndOfStatement();
                return s;
            }
            if (w == "for") {
                Next();
                s->kind = Stmt::For;
                s->name = ExpectIdent("a loop variable");
                if (!IsIdent("in")) Fail("expected 'in' in for loop");
                Next();
                s->value = ParseExpr();
                s->body = ParseBlock();
                EndOfStatement();
                return s;
            }
            if (w == "return") {
                Next();
                s->kind = Stmt::Return;
                if (Peek().t != Token::Newline && Peek().t != Token::End && !IsPunct("}")) s->value = ParseExpr();
                EndOfStatement();
                return s;
            }
            if (w == "break" || w == "continue") {
                Next();
                s->kind = (w == "break") ? Stmt::Break : Stmt::Continue;
                EndOfStatement();
                return s;
            }
            if (w == "fn" || w == "on") Fail("'" + w + "' can only be used at the top level of a script");
        }
        ExprP e = ParseExpr();
        if (Peek().t == Token::Punct && (Peek().s == "=" || Peek().s == "+=" || Peek().s == "-=" || Peek().s == "*=" || Peek().s == "/=")) {
            if (e->kind != Expr::Var && e->kind != Expr::Index) Fail("left side of '" + Peek().s + "' must be a variable or list element");
            s->kind = Stmt::Assign;
            s->name = Next().s;
            s->target = std::move(e);
            s->value = ParseExpr();
        } else {
            s->kind = Stmt::ExprS;
            s->value = std::move(e);
        }
        EndOfStatement();
        return s;
    }

    // ---- expressions (precedence climbing) ----
    ExprP MakeBin(const std::string& op, ExprP a, ExprP b, int line) {
        auto e = std::make_unique<Expr>();
        e->kind = Expr::Binary;
        e->line = line;
        e->name = op;
        e->a = std::move(a);
        e->b = std::move(b);
        return e;
    }

    ExprP ParseExpr() { return ParseOr(); }

    ExprP ParseOr() {
        ExprP l = ParseAnd();
        while (IsPunct("||")) {
            int line = Next().line;
            SkipNewlines();
            l = MakeBin("||", std::move(l), ParseAnd(), line);
        }
        return l;
    }
    ExprP ParseAnd() {
        ExprP l = ParseEq();
        while (IsPunct("&&")) {
            int line = Next().line;
            SkipNewlines();
            l = MakeBin("&&", std::move(l), ParseEq(), line);
        }
        return l;
    }
    ExprP ParseEq() {
        ExprP l = ParseCmp();
        while (IsPunct("==") || IsPunct("!=")) {
            std::string op = Peek().s;
            int line = Next().line;
            SkipNewlines();
            l = MakeBin(op, std::move(l), ParseCmp(), line);
        }
        return l;
    }
    ExprP ParseCmp() {
        ExprP l = ParseAdd();
        while (IsPunct("<") || IsPunct("<=") || IsPunct(">") || IsPunct(">=")) {
            std::string op = Peek().s;
            int line = Next().line;
            SkipNewlines();
            l = MakeBin(op, std::move(l), ParseAdd(), line);
        }
        return l;
    }
    ExprP ParseAdd() {
        ExprP l = ParseMul();
        while (IsPunct("+") || IsPunct("-")) {
            std::string op = Peek().s;
            int line = Next().line;
            SkipNewlines();
            l = MakeBin(op, std::move(l), ParseMul(), line);
        }
        return l;
    }
    ExprP ParseMul() {
        ExprP l = ParseUnary();
        while (IsPunct("*") || IsPunct("/") || IsPunct("%")) {
            std::string op = Peek().s;
            int line = Next().line;
            SkipNewlines();
            l = MakeBin(op, std::move(l), ParseUnary(), line);
        }
        return l;
    }
    ExprP ParseUnary() {
        if (IsPunct("-") || IsPunct("!")) {
            auto e = std::make_unique<Expr>();
            e->kind = Expr::Unary;
            e->name = Peek().s;
            e->line = Next().line;
            e->a = ParseUnary();
            return e;
        }
        return ParsePostfix();
    }
    ExprP ParsePostfix() {
        ExprP e = ParsePrimary();
        for (;;) {
            if (IsPunct("(")) {
                if (e->kind != Expr::Var) Fail("only named functions can be called");
                int line = Next().line;
                auto c = std::make_unique<Expr>();
                c->kind = Expr::Call;
                c->line = line;
                c->name = e->name;
                SkipNewlines();
                if (!IsPunct(")")) {
                    for (;;) {
                        c->items.push_back(ParseExpr());
                        SkipNewlines();
                        if (AcceptPunct(",")) { SkipNewlines(); if (IsPunct(")")) break; continue; }
                        break;
                    }
                }
                ExpectPunct(")");
                e = std::move(c);
            } else if (IsPunct("[")) {
                int line = Next().line;
                auto ix = std::make_unique<Expr>();
                ix->kind = Expr::Index;
                ix->line = line;
                ix->a = std::move(e);
                SkipNewlines();
                ix->b = ParseExpr();
                SkipNewlines();
                ExpectPunct("]");
                e = std::move(ix);
            } else {
                break;
            }
        }
        return e;
    }
    ExprP ParsePrimary() {
        auto e = std::make_unique<Expr>();
        e->line = Peek().line;
        const Token& k = Peek();
        if (k.t == Token::Num || k.t == Token::Dur) {
            e->kind = Expr::Lit;
            e->lit = Value::Number(Next().n);
            return e;
        }
        if (k.t == Token::Str) {
            e->kind = Expr::Lit;
            e->lit = Value::String(Next().s);
            return e;
        }
        if (k.t == Token::Ident) {
            std::string w = Next().s;
            if (w == "true" || w == "false") { e->kind = Expr::Lit; e->lit = Value::Boolean(w == "true"); return e; }
            if (w == "nil") { e->kind = Expr::Lit; return e; }
            if (IsReserved(w)) Fail("unexpected keyword '" + w + "'", e->line);
            e->kind = Expr::Var;
            e->name = w;
            return e;
        }
        if (IsPunct("(")) {
            Next();
            SkipNewlines();
            ExprP inner = ParseExpr();
            SkipNewlines();
            ExpectPunct(")");
            return inner;
        }
        if (IsPunct("[")) {
            Next();
            e->kind = Expr::ListLit;
            SkipNewlines();
            if (!IsPunct("]")) {
                for (;;) {
                    e->items.push_back(ParseExpr());
                    SkipNewlines();
                    if (AcceptPunct(",")) { SkipNewlines(); if (IsPunct("]")) break; continue; }
                    break;
                }
            }
            ExpectPunct("]");
            return e;
        }
        Fail("unexpected " + Describe(k));
    }
};

// =============================================================================================
// Interpreter
// =============================================================================================
using Scope = std::unordered_map<std::string, Value>;

struct Frame {
    std::vector<Scope> scopes;
};

enum class Flow { Normal, Break, Continue, Return };

class Interp;
using Args = std::vector<Value>;
using Builtin = Value (*)(Interp&, Args&, int);

class Interp {
public:
    Host* host = nullptr;
    std::string label, slug;
    const Program* prog = nullptr;
    Scope globals;
    long steps = 0;
    int depth = 0;
    Value retval;

    void Step(int line) {
        if (++steps > kMaxSteps) throw ScriptError{"script ran too long (possible endless loop)", line};
    }

    // ---- variables ----
    Value* Find(const std::string& name, Frame& f) {
        for (size_t i = f.scopes.size(); i-- > 0;) {
            auto it = f.scopes[i].find(name);
            if (it != f.scopes[i].end()) return &it->second;
        }
        auto it = globals.find(name);
        return it == globals.end() ? nullptr : &it->second;
    }
    void Declare(const std::string& name, Value v, Frame& f) {
        if (f.scopes.empty()) globals[name] = std::move(v);
        else f.scopes.back()[name] = std::move(v);
    }

    // ---- execution ----
    struct ScopeGuard {
        Frame& f;
        explicit ScopeGuard(Frame& fr) : f(fr) { f.scopes.emplace_back(); }
        ~ScopeGuard() { f.scopes.pop_back(); }
    };

    Flow ExecBlock(const Block& b, Frame& f, bool newScope = true) {
        if (newScope) {
            ScopeGuard g(f);
            return ExecStmts(b, f);
        }
        return ExecStmts(b, f);
    }

    Flow ExecStmts(const Block& b, Frame& f) {
        for (const auto& s : b) {
            Flow fl = Exec(*s, f);
            if (fl != Flow::Normal) return fl;
        }
        return Flow::Normal;
    }

    Flow Exec(const Stmt& s, Frame& f) {
        Step(s.line);
        switch (s.kind) {
            case Stmt::Let:
                Declare(s.name, Eval(*s.value, f), f);
                return Flow::Normal;
            case Stmt::Assign: Assign(s, f); return Flow::Normal;
            case Stmt::ExprS: Eval(*s.value, f); return Flow::Normal;
            case Stmt::If:
                for (const auto& br : s.branches)
                    if (Truthy(Eval(*br.first, f))) return ExecBlock(br.second, f);
                if (s.hasElse) return ExecBlock(s.body, f);
                return Flow::Normal;
            case Stmt::While:
                while (Truthy(Eval(*s.value, f))) {
                    Step(s.line);
                    Flow fl = ExecBlock(s.body, f);
                    if (fl == Flow::Break) break;
                    if (fl == Flow::Return) return fl;
                }
                return Flow::Normal;
            case Stmt::For: {
                Value it = Eval(*s.value, f);
                if (it.t != Value::List && it.t != Value::Str) throw ScriptError{std::string("cannot loop over a ") + TypeName(it), s.line};
                size_t count = it.t == Value::List ? it.l->size() : it.s.size();
                for (size_t i = 0; i < (it.t == Value::List ? it.l->size() : count); ++i) {
                    Step(s.line);
                    ScopeGuard g(f);
                    f.scopes.back()[s.name] = it.t == Value::List ? (*it.l)[i] : Value::String(std::string(1, it.s[i]));
                    Flow fl = ExecStmts(s.body, f);
                    if (fl == Flow::Break) break;
                    if (fl == Flow::Return) return fl;
                }
                return Flow::Normal;
            }
            case Stmt::Return:
                retval = s.value ? Eval(*s.value, f) : Value();
                return Flow::Return;
            case Stmt::Break: return Flow::Break;
            case Stmt::Continue: return Flow::Continue;
        }
        return Flow::Normal;
    }

    void Assign(const Stmt& s, Frame& f) {
        Value rhs = Eval(*s.value, f);
        const Expr& tg = *s.target;
        if (tg.kind == Expr::Var) {
            Value* slot = Find(tg.name, f);
            if (!slot) throw ScriptError{"undefined variable '" + tg.name + "' (declare it with let)", s.line};
            *slot = s.name == "=" ? rhs : BinaryOp(s.name.substr(0, 1), *slot, rhs, s.line);
            return;
        }
        Value base = Eval(*tg.a, f);
        Value idx = Eval(*tg.b, f);
        if (base.t != Value::List) throw ScriptError{std::string("cannot assign into a ") + TypeName(base), s.line};
        size_t i = CheckIndex(idx, base.l->size(), s.line);
        Value& slot = (*base.l)[i];
        slot = s.name == "=" ? rhs : BinaryOp(s.name.substr(0, 1), slot, rhs, s.line);
    }

    size_t CheckIndex(const Value& idx, size_t size, int line) {
        if (idx.t != Value::Num || idx.n != std::floor(idx.n)) throw ScriptError{"index must be a whole number", line};
        if (idx.n < 0 || idx.n >= static_cast<double>(size))
            throw ScriptError{"index " + NumToStr(idx.n) + " is out of range (size " + std::to_string(size) + ")", line};
        return static_cast<size_t>(idx.n);
    }

    // ---- expressions ----
    Value Eval(const Expr& e, Frame& f) {
        Step(e.line);
        switch (e.kind) {
            case Expr::Lit: return e.lit;
            case Expr::Var: {
                Value* v = Find(e.name, f);
                if (!v) throw ScriptError{"undefined variable '" + e.name + "'", e.line};
                return *v;
            }
            case Expr::ListLit: {
                std::vector<Value> items;
                for (const auto& it : e.items) items.push_back(Eval(*it, f));
                return Value::MakeList(std::move(items));
            }
            case Expr::Unary: {
                Value v = Eval(*e.a, f);
                if (e.name == "!") return Value::Boolean(!Truthy(v));
                if (v.t != Value::Num) throw ScriptError{std::string("cannot negate a ") + TypeName(v), e.line};
                return Value::Number(-v.n);
            }
            case Expr::Binary: {
                if (e.name == "&&") {
                    Value l = Eval(*e.a, f);
                    return Truthy(l) ? Eval(*e.b, f) : l;
                }
                if (e.name == "||") {
                    Value l = Eval(*e.a, f);
                    return Truthy(l) ? l : Eval(*e.b, f);
                }
                Value l = Eval(*e.a, f);
                Value r = Eval(*e.b, f);
                return BinaryOp(e.name, l, r, e.line);
            }
            case Expr::Index: {
                Value base = Eval(*e.a, f);
                Value idx = Eval(*e.b, f);
                if (base.t == Value::List) return (*base.l)[CheckIndex(idx, base.l->size(), e.line)];
                if (base.t == Value::Str) return Value::String(std::string(1, base.s[CheckIndex(idx, base.s.size(), e.line)]));
                throw ScriptError{std::string("cannot index a ") + TypeName(base), e.line};
            }
            case Expr::Call: return Call(e, f);
        }
        return Value();
    }

    Value BinaryOp(const std::string& op, const Value& l, const Value& r, int line) {
        if (op == "==") return Value::Boolean(Equals(l, r));
        if (op == "!=") return Value::Boolean(!Equals(l, r));
        if (op == "+") {
            if (l.t == Value::Num && r.t == Value::Num) return Value::Number(l.n + r.n);
            if (l.t == Value::Str || r.t == Value::Str) return Value::String(ToDisplay(l) + ToDisplay(r));
            if (l.t == Value::List && r.t == Value::List) {
                std::vector<Value> v = *l.l;
                v.insert(v.end(), r.l->begin(), r.l->end());
                return Value::MakeList(std::move(v));
            }
            throw ScriptError{std::string("cannot add ") + TypeName(l) + " and " + TypeName(r), line};
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            int c;
            if (l.t == Value::Num && r.t == Value::Num) c = l.n < r.n ? -1 : (l.n > r.n ? 1 : 0);
            else if (l.t == Value::Str && r.t == Value::Str) c = l.s < r.s ? -1 : (l.s > r.s ? 1 : 0);
            else throw ScriptError{std::string("cannot compare ") + TypeName(l) + " and " + TypeName(r), line};
            return Value::Boolean(op == "<" ? c < 0 : op == "<=" ? c <= 0 : op == ">" ? c > 0 : c >= 0);
        }
        if (l.t != Value::Num || r.t != Value::Num)
            throw ScriptError{"'" + op + "' needs two numbers, got " + TypeName(l) + " and " + TypeName(r), line};
        if (op == "-") return Value::Number(l.n - r.n);
        if (op == "*") return Value::Number(l.n * r.n);
        if (op == "/" || op == "%") {
            if (r.n == 0) throw ScriptError{"division by zero", line};
            return Value::Number(op == "/" ? l.n / r.n : std::fmod(l.n, r.n));
        }
        throw ScriptError{"unknown operator '" + op + "'", line};
    }

    Value Call(const Expr& e, Frame& f);
    Value CallUser(const Func& fn, Args& args, int line);
};

// ---- argument helpers ----
void NeedArgs(const Args& a, size_t lo, size_t hi, const char* name, int line) {
    if (a.size() < lo || a.size() > hi) {
        std::string want = lo == hi ? std::to_string(lo) : std::to_string(lo) + (hi == static_cast<size_t>(-1) ? "+" : "-" + std::to_string(hi));
        throw ScriptError{std::string(name) + "() takes " + want + " argument(s), got " + std::to_string(a.size()), line};
    }
}
double ArgNum(const Args& a, size_t i, const char* name, int line) {
    if (a[i].t != Value::Num) throw ScriptError{std::string(name) + "() argument " + std::to_string(i + 1) + " must be a number, got " + TypeName(a[i]), line};
    return a[i].n;
}
const std::string& ArgStr(const Args& a, size_t i, const char* name, int line) {
    if (a[i].t != Value::Str) throw ScriptError{std::string(name) + "() argument " + std::to_string(i + 1) + " must be a string, got " + TypeName(a[i]), line};
    return a[i].s;
}
const std::vector<Value>& ArgList(const Args& a, size_t i, const char* name, int line) {
    if (a[i].t != Value::List) throw ScriptError{std::string(name) + "() argument " + std::to_string(i + 1) + " must be a list, got " + TypeName(a[i]), line};
    return *a[i].l;
}
uint64_t ArgAddr(const Args& a, size_t i, const char* name, int line) {
    double d = ArgNum(a, i, name, line);
    if (d < 0 || d > 4294967295.0 || d != std::floor(d)) throw ScriptError{std::string(name) + "(): address must be a whole number between 0 and 0xFFFFFFFF", line};
    return static_cast<uint64_t>(d);
}
std::vector<uint8_t> ByteList(const Value& v, const char* name, int line) {
    if (v.t != Value::List) throw ScriptError{std::string(name) + "() needs a list of bytes, e.g. [0x90, 0x90]", line};
    std::vector<uint8_t> out;
    for (const auto& x : *v.l) {
        if (x.t != Value::Num || x.n < 0 || x.n > 255 || x.n != std::floor(x.n))
            throw ScriptError{std::string(name) + "(): every byte must be a whole number from 0 to 255", line};
        out.push_back(static_cast<uint8_t>(x.n));
    }
    return out;
}
Value BytesToList(const std::vector<uint8_t>& b) {
    std::vector<Value> v;
    v.reserve(b.size());
    for (uint8_t x : b) v.push_back(Value::Number(x));
    return Value::MakeList(std::move(v));
}

std::string FormatValues(const std::string& fmt, const Args& a, size_t firstArg, int line) {
    std::string out;
    size_t ai = firstArg;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        if (++i >= fmt.size()) throw ScriptError{"format(): dangling '%'", line};
        if (fmt[i] == '%') { out += '%'; continue; }
        std::string flags;
        while (i < fmt.size() && (fmt[i] == '0' || fmt[i] == '-')) flags += fmt[i++];
        std::string width;
        while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) width += fmt[i++];
        std::string prec;
        bool hasPrec = false;
        if (i < fmt.size() && fmt[i] == '.') {
            hasPrec = true;
            ++i;
            while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) prec += fmt[i++];
        }
        if (i >= fmt.size()) throw ScriptError{"format(): incomplete format", line};
        char conv = fmt[i];
        if (ai >= a.size()) throw ScriptError{"format(): not enough arguments for the format string", line};
        const Value& v = a[ai++];
        if (width.size() > 3 || prec.size() > 2) throw ScriptError{"format(): width/precision too large", line};
        char buf[512];
        if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'X') {
            if (v.t != Value::Num) throw ScriptError{std::string("format(): %") + conv + " needs a number", line};
            std::string f = "%" + flags + width + "ll" + (conv == 'i' ? 'd' : conv);
            if (conv == 'd' || conv == 'i') snprintf(buf, sizeof(buf), f.c_str(), static_cast<long long>(std::llround(v.n)));
            else snprintf(buf, sizeof(buf), f.c_str(), static_cast<unsigned long long>(std::llround(v.n)));
            out += buf;
        } else if (conv == 'f') {
            if (v.t != Value::Num) throw ScriptError{"format(): %f needs a number", line};
            std::string f = "%" + flags + width + (hasPrec ? "." + prec : "") + "f";
            snprintf(buf, sizeof(buf), f.c_str(), v.n);
            out += buf;
        } else if (conv == 's') {
            std::string s = ToDisplay(v);
            size_t w = width.empty() ? 0 : static_cast<size_t>(std::atoi(width.c_str()));
            if (s.size() < w) {
                if (flags.find('-') != std::string::npos) s += std::string(w - s.size(), ' ');
                else s = std::string(w - s.size(), ' ') + s;
            }
            out += s;
        } else {
            throw ScriptError{std::string("format(): unknown conversion '%") + conv + "' (use %d %f %s %x %X %%)", line};
        }
    }
    return out;
}

bool ParseNumberText(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) {
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            char* e2 = nullptr;
            out = static_cast<double>(std::strtoull(text.c_str() + 2, &e2, 16));
            return *e2 == '\0';
        }
        return false;
    }
    while (*end == ' ' || *end == '\t' || *end == '\r') ++end;
    return *end == '\0';
}

// ---- builtins ----
const std::unordered_map<std::string, Builtin>& Builtins() {
    static std::unordered_map<std::string, Builtin> B;
    if (!B.empty()) return B;

    B["log"] = [](Interp& in, Args& a, int) -> Value {
        std::string s;
        for (size_t i = 0; i < a.size(); ++i) s += (i ? " " : "") + ToDisplay(a[i]);
        in.host->Log(in.label, s);
        return Value();
    };
    B["str"] = [](Interp&, Args& a, int l) -> Value { NeedArgs(a, 1, 1, "str", l); return Value::String(ToDisplay(a[0])); };
    B["num"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "num", l);
        if (a[0].t == Value::Num) return a[0];
        if (a[0].t == Value::Bool) return Value::Number(a[0].b ? 1 : 0);
        double d;
        if (a[0].t == Value::Str && ParseNumberText(a[0].s, d)) return Value::Number(d);
        return Value();
    };
    B["len"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "len", l);
        if (a[0].t == Value::Str) return Value::Number(static_cast<double>(a[0].s.size()));
        if (a[0].t == Value::List) return Value::Number(static_cast<double>(a[0].l->size()));
        throw ScriptError{std::string("len() needs a string or list, got ") + TypeName(a[0]), l};
    };
    B["type"] = [](Interp&, Args& a, int l) -> Value { NeedArgs(a, 1, 1, "type", l); return Value::String(TypeName(a[0])); };

    B["abs"] = [](Interp&, Args& a, int l) -> Value { NeedArgs(a, 1, 1, "abs", l); return Value::Number(std::fabs(ArgNum(a, 0, "abs", l))); };
    B["floor"] = [](Interp&, Args& a, int l) -> Value { NeedArgs(a, 1, 1, "floor", l); return Value::Number(std::floor(ArgNum(a, 0, "floor", l))); };
    B["ceil"] = [](Interp&, Args& a, int l) -> Value { NeedArgs(a, 1, 1, "ceil", l); return Value::Number(std::ceil(ArgNum(a, 0, "ceil", l))); };
    B["sqrt"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "sqrt", l);
        double x = ArgNum(a, 0, "sqrt", l);
        if (x < 0) throw ScriptError{"sqrt() of a negative number", l};
        return Value::Number(std::sqrt(x));
    };
    B["pow"] = [](Interp&, Args& a, int l) -> Value { NeedArgs(a, 2, 2, "pow", l); return Value::Number(std::pow(ArgNum(a, 0, "pow", l), ArgNum(a, 1, "pow", l))); };
    B["round"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 2, "round", l);
        double x = ArgNum(a, 0, "round", l);
        double d = a.size() > 1 ? ArgNum(a, 1, "round", l) : 0;
        if (d < 0 || d > 9) throw ScriptError{"round(): digits must be 0-9", l};
        double m = std::pow(10.0, d);
        return Value::Number(std::round(x * m) / m);
    };
    B["min"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 2, static_cast<size_t>(-1), "min", l);
        double r = ArgNum(a, 0, "min", l);
        for (size_t i = 1; i < a.size(); ++i) r = std::min(r, ArgNum(a, i, "min", l));
        return Value::Number(r);
    };
    B["max"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 2, static_cast<size_t>(-1), "max", l);
        double r = ArgNum(a, 0, "max", l);
        for (size_t i = 1; i < a.size(); ++i) r = std::max(r, ArgNum(a, i, "max", l));
        return Value::Number(r);
    };
    B["clamp"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 3, 3, "clamp", l);
        double x = ArgNum(a, 0, "clamp", l), lo = ArgNum(a, 1, "clamp", l), hi = ArgNum(a, 2, "clamp", l);
        return Value::Number(x < lo ? lo : (x > hi ? hi : x));
    };
    B["format"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, static_cast<size_t>(-1), "format", l);
        return Value::String(FormatValues(ArgStr(a, 0, "format", l), a, 1, l));
    };

    B["push"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 2, 2, "push", l);
        ArgList(a, 0, "push", l);
        if (a[0].l->size() >= 100000) throw ScriptError{"list too large", l};
        a[0].l->push_back(a[1]);
        return Value();
    };
    B["range"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 2, "range", l);
        double lo = a.size() == 2 ? ArgNum(a, 0, "range", l) : 0;
        double hi = ArgNum(a, a.size() == 2 ? 1 : 0, "range", l);
        if (hi - lo > 100000) throw ScriptError{"range too large", l};
        std::vector<Value> v;
        for (double x = lo; x < hi; x += 1) v.push_back(Value::Number(x));
        return Value::MakeList(std::move(v));
    };
    B["bytes"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "bytes", l);
        Pattern p;
        if (!parsePattern(ArgStr(a, 0, "bytes", l), p)) throw ScriptError{"bytes(): expected hex bytes like \"90 90 EB\"", l};
        std::vector<Value> v;
        for (int b : p.bytes) {
            if (b < 0) throw ScriptError{"bytes(): wildcards are only allowed in scan()", l};
            v.push_back(Value::Number(b));
        }
        return Value::MakeList(std::move(v));
    };
    B["join"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 2, "join", l);
        const auto& list = ArgList(a, 0, "join", l);
        std::string sep = a.size() > 1 ? ArgStr(a, 1, "join", l) : "";
        std::string out;
        for (size_t i = 0; i < list.size(); ++i) out += (i ? sep : "") + ToDisplay(list[i]);
        return Value::String(out);
    };
    B["contains"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 2, 2, "contains", l);
        if (a[0].t == Value::Str) return Value::Boolean(a[0].s.find(ArgStr(a, 1, "contains", l)) != std::string::npos);
        const auto& list = ArgList(a, 0, "contains", l);
        for (const auto& x : list)
            if (Equals(x, a[1])) return Value::Boolean(true);
        return Value::Boolean(false);
    };
    B["lower"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "lower", l);
        std::string s = ArgStr(a, 0, "lower", l);
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return Value::String(s);
    };
    B["upper"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "upper", l);
        std::string s = ArgStr(a, 0, "upper", l);
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return Value::String(s);
    };
    B["u16_bytes"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "u16_bytes", l);
        uint16_t v = static_cast<uint16_t>(static_cast<long long>(ArgNum(a, 0, "u16_bytes", l)));
        return BytesToList({static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8)});
    };
    B["u32_bytes"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "u32_bytes", l);
        uint32_t v = static_cast<uint32_t>(static_cast<long long>(ArgNum(a, 0, "u32_bytes", l)));
        return BytesToList({static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)});
    };
    B["f32_bytes"] = [](Interp&, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "f32_bytes", l);
        float f = static_cast<float>(ArgNum(a, 0, "f32_bytes", l));
        uint8_t raw[4];
        std::memcpy(raw, &f, 4);
        return BytesToList({raw[0], raw[1], raw[2], raw[3]});
    };

    // ---- the game ----
    B["speed"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 0, 0, "speed", l); return Value::Number(in.host->GetSpeed()); };
    B["set_speed"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 1, 1, "set_speed", l); return Value::Boolean(in.host->SetSpeed(ArgNum(a, 0, "set_speed", l))); };
    B["fps"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 0, 0, "fps", l); return Value::Number(in.host->GetFps()); };
    B["set_fps"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 1, 1, "set_fps", l); return Value::Boolean(in.host->SetFps(ArgNum(a, 0, "set_fps", l))); };
    B["tainted"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 0, 0, "tainted", l); return Value::Boolean(in.host->Tainted()); };
    B["taint"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 1, 1, "taint", l); in.host->Taint(ToDisplay(a[0])); return Value(); };
    B["guard_active"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 0, 0, "guard_active", l); return Value::Boolean(in.host->GuardActive()); };
    B["status"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 1, 1, "status", l); in.host->SetStatus(in.label, ToDisplay(a[0])); return Value(); };
    B["gd_base"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 0, 0, "gd_base", l); return Value::Number(static_cast<double>(in.host->GdBase())); };
    B["gd_stamp"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 0, 0, "gd_stamp", l); return Value::Number(in.host->GdStamp()); };
    B["now_ms"] = [](Interp& in, Args& a, int l) -> Value { NeedArgs(a, 0, 0, "now_ms", l); return Value::Number(static_cast<double>(in.host->NowMs())); };

    // ---- memory ----
    B["scan"] = [](Interp& in, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "scan", l);
        Pattern p;
        if (!parsePattern(ArgStr(a, 0, "scan", l), p)) throw ScriptError{"scan(): bad pattern, expected hex bytes with ?? wildcards", l};
        return Value::Number(static_cast<double>(in.host->Scan(ArgStr(a, 0, "scan", l))));
    };
    B["patch"] = [](Interp& in, Args& a, int l) -> Value {
        NeedArgs(a, 2, 2, "patch", l);
        auto bytes = ByteList(a[1], "patch", l);
        if (bytes.empty() || bytes.size() > 4096) throw ScriptError{"patch(): between 1 and 4096 bytes", l};
        return Value::Boolean(in.host->Patch(ArgAddr(a, 0, "patch", l), bytes));
    };
    B["nop"] = [](Interp& in, Args& a, int l) -> Value {
        NeedArgs(a, 2, 2, "nop", l);
        double n = ArgNum(a, 1, "nop", l);
        if (n < 1 || n > 4096 || n != std::floor(n)) throw ScriptError{"nop(): count must be 1-4096", l};
        return Value::Boolean(in.host->Patch(ArgAddr(a, 0, "nop", l), std::vector<uint8_t>(static_cast<size_t>(n), 0x90)));
    };
    B["read_bytes"] = [](Interp& in, Args& a, int l) -> Value {
        NeedArgs(a, 2, 2, "read_bytes", l);
        double n = ArgNum(a, 1, "read_bytes", l);
        if (n < 1 || n > 4096 || n != std::floor(n)) throw ScriptError{"read_bytes(): count must be 1-4096", l};
        std::vector<uint8_t> out;
        if (!in.host->Read(ArgAddr(a, 0, "read_bytes", l), static_cast<size_t>(n), out)) return Value();
        return BytesToList(out);
    };
    B["read_u8"] = [](Interp& in, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "read_u8", l);
        std::vector<uint8_t> o;
        if (!in.host->Read(ArgAddr(a, 0, "read_u8", l), 1, o) || o.size() != 1) return Value();
        return Value::Number(o[0]);
    };
    B["read_u16"] = [](Interp& in, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "read_u16", l);
        std::vector<uint8_t> o;
        if (!in.host->Read(ArgAddr(a, 0, "read_u16", l), 2, o) || o.size() != 2) return Value();
        return Value::Number(o[0] | (o[1] << 8));
    };
    B["read_u32"] = [](Interp& in, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "read_u32", l);
        std::vector<uint8_t> o;
        if (!in.host->Read(ArgAddr(a, 0, "read_u32", l), 4, o) || o.size() != 4) return Value();
        uint32_t v = static_cast<uint32_t>(o[0]) | (static_cast<uint32_t>(o[1]) << 8) | (static_cast<uint32_t>(o[2]) << 16) | (static_cast<uint32_t>(o[3]) << 24);
        return Value::Number(v);
    };
    B["read_f32"] = [](Interp& in, Args& a, int l) -> Value {
        NeedArgs(a, 1, 1, "read_f32", l);
        std::vector<uint8_t> o;
        if (!in.host->Read(ArgAddr(a, 0, "read_f32", l), 4, o) || o.size() != 4) return Value();
        float f;
        std::memcpy(&f, o.data(), 4);
        return Value::Number(f);
    };

    // ---- settings: gocode\config\<script name>.cfg, re-read on every call so edits apply live ----
    B["cfg"] = [](Interp& in, Args& a, int l) -> Value {
        NeedArgs(a, 1, 2, "cfg", l);
        Value def = a.size() > 1 ? a[1] : Value();
        std::string raw;
        if (!in.host->Config(in.slug, ArgStr(a, 0, "cfg", l), raw)) return def;
        double d;
        if (def.t == Value::Num) return ParseNumberText(raw, d) ? Value::Number(d) : def;
        if (def.t == Value::Bool) {
            if (raw == "true" || raw == "1" || raw == "yes" || raw == "on") return Value::Boolean(true);
            if (raw == "false" || raw == "0" || raw == "no" || raw == "off") return Value::Boolean(false);
            return def;
        }
        if (def.t == Value::Str) return Value::String(raw);
        if (raw == "true") return Value::Boolean(true);
        if (raw == "false") return Value::Boolean(false);
        if (ParseNumberText(raw, d)) return Value::Number(d);
        return Value::String(raw);
    };
    return B;
}

Value Interp::Call(const Expr& e, Frame& f) {
    Args args;
    args.reserve(e.items.size());
    for (const auto& it : e.items) args.push_back(Eval(*it, f));
    auto uf = prog->funcs.find(e.name);
    if (uf != prog->funcs.end()) return CallUser(uf->second, args, e.line);
    const auto& b = Builtins();
    auto bi = b.find(e.name);
    if (bi == b.end()) throw ScriptError{"unknown function '" + e.name + "'", e.line};
    return bi->second(*this, args, e.line);
}

Value Interp::CallUser(const Func& fn, Args& args, int line) {
    if (args.size() != fn.params.size())
        throw ScriptError{fn.name + "() takes " + std::to_string(fn.params.size()) + " argument(s), got " + std::to_string(args.size()), line};
    if (depth >= kMaxDepth) throw ScriptError{"function calls nested too deeply (endless recursion?)", line};
    ++depth;
    Frame fr;
    fr.scopes.emplace_back();
    for (size_t i = 0; i < fn.params.size(); ++i) fr.scopes.back()[fn.params[i]] = args[i];
    retval = Value();
    Flow fl;
    try {
        fl = ExecStmts(fn.body, fr);
    } catch (...) {
        --depth;
        throw;
    }
    --depth;
    if (fl == Flow::Break || fl == Flow::Continue) throw ScriptError{"break/continue outside of a loop", line};
    Value r = fl == Flow::Return ? retval : Value();
    retval = Value();
    return r;
}

}  // namespace

// =============================================================================================
// Key names
// =============================================================================================
bool ParseKeyCombo(const std::vector<std::string>& parts, uint32_t& vk, uint32_t& mods, std::string& error) {
    vk = 0;
    mods = 0;
    static const std::map<std::string, uint32_t> named = {
        {"space", 0x20}, {"enter", 0x0D}, {"return", 0x0D}, {"escape", 0x1B}, {"esc", 0x1B}, {"tab", 0x09},
        {"backspace", 0x08}, {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28}, {"home", 0x24},
        {"end", 0x23}, {"pageup", 0x21}, {"pagedown", 0x22}, {"insert", 0x2D}, {"delete", 0x2E},
        {"minus", 0xBD}, {"equals", 0xBB}, {"comma", 0xBC}, {"period", 0xBE}, {"slash", 0xBF},
        {"semicolon", 0xBA}, {"quote", 0xDE}, {"lbracket", 0xDB}, {"rbracket", 0xDD}, {"backslash", 0xDC},
        {"backtick", 0xC0}, {"numplus", 0x6B}, {"numminus", 0x6D}, {"nummul", 0x6A}, {"numdiv", 0x6F}};
    bool haveKey = false;
    for (const std::string& raw : parts) {
        std::string p = raw;
        for (char& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        uint32_t code = 0;
        if (p == "ctrl" || p == "control") { mods |= 1; continue; }
        if (p == "shift") { mods |= 2; continue; }
        if (p == "alt") { mods |= 4; continue; }
        if (p.size() >= 2 && p[0] == 'f' && std::isdigit(static_cast<unsigned char>(p[1]))) {
            int n = std::atoi(p.c_str() + 1);
            if (n < 1 || n > 24 || p.find_first_not_of("0123456789", 1) != std::string::npos) { error = "unknown key '" + raw + "'"; return false; }
            code = 0x6F + static_cast<uint32_t>(n);
        } else if (p.size() == 1 && std::isalpha(static_cast<unsigned char>(p[0]))) {
            code = static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(p[0])));
        } else if (p.size() == 1 && std::isdigit(static_cast<unsigned char>(p[0]))) {
            code = static_cast<uint32_t>(p[0]);
        } else if (p.size() == 4 && p.compare(0, 3, "num") == 0 && std::isdigit(static_cast<unsigned char>(p[3]))) {
            code = 0x60 + static_cast<uint32_t>(p[3] - '0');
        } else {
            auto it = named.find(p);
            if (it == named.end()) { error = "unknown key '" + raw + "'"; return false; }
            code = it->second;
        }
        if (haveKey) { error = "a hotkey can have only one main key (found '" + raw + "' as well)"; return false; }
        vk = code;
        haveKey = true;
    }
    if (!haveKey) { error = "hotkey needs a key (like F5 or ctrl+K)"; return false; }
    return true;
}

// =============================================================================================
// Runtime
// =============================================================================================
struct Runtime::Impl {
    Program prog;
    Interp in;
    std::vector<EventInfo> eventInfos;
    std::vector<int> errors;
    std::vector<bool> disabled;
    bool parsed = false;
};

Runtime::Runtime(Host* host, std::string label, std::string slug) : impl_(new Impl) {
    impl_->in.host = host;
    impl_->in.label = std::move(label);
    impl_->in.slug = std::move(slug);
    impl_->in.prog = &impl_->prog;
}
Runtime::~Runtime() = default;

void Runtime::SetLabel(const std::string& label) { impl_->in.label = label; }
const std::string& Runtime::label() const { return impl_->in.label; }
const std::string& Runtime::name() const { return impl_->prog.name; }
const std::string& Runtime::author() const { return impl_->prog.author; }
const std::string& Runtime::version() const { return impl_->prog.version; }
bool Runtime::hasStamp() const { return impl_->prog.hasStamp; }
uint32_t Runtime::stamp() const { return impl_->prog.stamp; }
const std::vector<EventInfo>& Runtime::events() const { return impl_->eventInfos; }

bool Runtime::Parse(const std::string& source, std::vector<std::string>& errors) {
    impl_->prog = Program();
    try {
        Parser parser(Lex(source), impl_->prog);
        parser.ParseProgram();
    } catch (const ScriptError& e) {
        errors.push_back("line " + std::to_string(e.line) + ": " + e.msg);
        return false;
    }
    impl_->eventInfos.clear();
    for (const auto& ev : impl_->prog.events) impl_->eventInfos.push_back(ev.info);
    impl_->errors.assign(impl_->eventInfos.size(), 0);
    impl_->disabled.assign(impl_->eventInfos.size(), false);
    impl_->parsed = true;
    return true;
}

bool Runtime::Init() {
    if (!impl_->parsed) return false;
    Interp& in = impl_->in;
    in.steps = 0;
    in.depth = 0;
    try {
        Frame f;
        in.ExecStmts(impl_->prog.top, f);
    } catch (const ScriptError& e) {
        in.host->Log(in.label, "error at line " + std::to_string(e.line) + ": " + e.msg);
        return false;
    }
    return true;
}

bool Runtime::RunEvent(size_t index) {
    if (!impl_->parsed || index >= impl_->prog.events.size() || impl_->disabled[index]) return false;
    Interp& in = impl_->in;
    in.steps = 0;
    in.depth = 0;
    try {
        Frame f;
        in.ExecBlock(impl_->prog.events[index].body, f);
        return true;
    } catch (const ScriptError& e) {
        in.host->Log(in.label, "error at line " + std::to_string(e.line) + ": " + e.msg);
    } catch (const std::exception& e) {
        in.host->Log(in.label, std::string("internal error: ") + e.what());
    }
    if (++impl_->errors[index] >= kMaxHandlerErrors) {
        impl_->disabled[index] = true;
        in.host->Log(in.label, "the handler at line " + std::to_string(impl_->eventInfos[index].line) + " failed " +
                                   std::to_string(kMaxHandlerErrors) + " times and has been switched off");
    }
    return false;
}

}  // namespace gocode
