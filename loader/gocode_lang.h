// The GoCode scripting language: lexer, parser and interpreter. Portable C++17 - no Windows dependencies, so the
// whole language can be unit tested anywhere. Everything a script can do to the outside world goes through Host.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gocode {

// Everything a GoCode script is allowed to touch. The loader implements this on top of the game; tests mock it.
class Host {
public:
    virtual ~Host() = default;
    virtual void Log(const std::string& mod, const std::string& message) = 0;
    virtual double GetSpeed() = 0;
    virtual bool SetSpeed(double speed) = 0;
    virtual double GetFps() = 0;
    virtual bool SetFps(double fps) = 0;
    virtual bool Tainted() = 0;
    virtual void Taint(const std::string& reason) = 0;
    virtual bool GuardActive() = 0;
    virtual void SetStatus(const std::string& mod, const std::string& text) = 0;
    virtual bool Patch(uint64_t address, const std::vector<uint8_t>& bytes) = 0;
    virtual uint64_t Scan(const std::string& pattern) = 0;  // 0 = not found
    virtual bool Read(uint64_t address, size_t count, std::vector<uint8_t>& out) = 0;
    virtual uint64_t GdBase() = 0;
    virtual uint32_t GdStamp() = 0;
    virtual bool Config(const std::string& slug, const std::string& key, std::string& out) = 0;
    virtual int64_t NowMs() = 0;
};

struct EventInfo {
    enum Kind { Start, Key, Tick } kind = Start;
    uint32_t vk = 0, mods = 0;  // Key (mods: 1 ctrl, 2 shift, 4 alt)
    uint32_t intervalMs = 0;    // Tick
    int line = 0;
};

class Runtime {
public:
    Runtime(Host* host, std::string label, std::string slug);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Parses the source. Fills `errors` ("line N: message") and returns false on a syntax error.
    bool Parse(const std::string& source, std::vector<std::string>& errors);
    // Runs the top-level statements (variables, setup calls). Runtime errors are logged through the host.
    bool Init();
    // Runs the handler for events()[index]. Returns false if it raised an error (a handler that fails
    // repeatedly is switched off).
    bool RunEvent(size_t index);

    void SetLabel(const std::string& label);
    const std::string& label() const;
    const std::string& name() const;     // from  mod "..."
    const std::string& author() const;   // from  author "..."
    const std::string& version() const;  // from  version "..."
    bool hasStamp() const;               // require_stamp 0x...
    uint32_t stamp() const;
    const std::vector<EventInfo>& events() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// "ctrl", "shift", "F5" -> virtual-key code + modifier bits. Exactly one non-modifier key is required.
bool ParseKeyCombo(const std::vector<std::string>& parts, uint32_t& vk, uint32_t& mods, std::string& error);

}  // namespace gocode
