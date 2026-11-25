// GoCode loader (core) - a 32-bit proxy for XInput9_1_0.dll.
//
// Geometry Dash loads XInput9_1_0.dll at startup. We ship our own DLL with that name; it forwards the real
// XInput calls to the system copy and, inside Geometry Dash only, starts a thread that
//   * installs the core services (game speed, frame rate, Safe Mode network guard, hotkeys, title status)
//   * runs GoCode mods     <GD>\gocode\mods\*.gocode   (the GoCode language: see gocode_lang.cpp)
//   * loads native mods    <GD>\gocode\mods\*.dll       (optional, for C/C++ authors: see sdk\gocode.h)
//
// Build: see build.bat (32-bit MinGW-w64, -Wl,--kill-at).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../sdk/gocode.h"
#include "core_logic.h"
#include "gocode_lang.h"
#include "pattern.h"

#ifdef _WIN32
static_assert(sizeof(void*) == 4, "GoCode's loader must be built 32-bit (Geometry Dash is a 32-bit game)");
#endif

#define GOCODE_LOADER_VERSION "0.2.0"

// The installer looks for this string to tell our DLL apart from a stock/other XInput9_1_0.dll.
extern "C" __declspec(dllexport) const char GoCode_Marker[] = "GOCODE_LOADER_MARKER_v1";

// =============================================================================================
// XInput forwarding
// =============================================================================================
static HMODULE RealXInput() {
    static HMODULE h = nullptr;
    if (!h) {
        char dir[MAX_PATH] = {0};
        UINT n = GetSystemDirectoryA(dir, MAX_PATH);  // SysWOW64 for a 32-bit process on 64-bit Windows
        if (n == 0 || n >= MAX_PATH) return nullptr;
        std::string p = std::string(dir) + "\\xinput9_1_0.dll";
        h = LoadLibraryA(p.c_str());
    }
    return h;
}

static FARPROC RealProc(const char* name) {
    HMODULE h = RealXInput();
    return h ? GetProcAddress(h, name) : nullptr;
}

extern "C" {

__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD index, void* state) {
    typedef DWORD(WINAPI * Fn)(DWORD, void*);
    static Fn fn = reinterpret_cast<Fn>(RealProc("XInputGetState"));
    return fn ? fn(index, state) : ERROR_DEVICE_NOT_CONNECTED;
}

__declspec(dllexport) DWORD WINAPI XInputSetState(DWORD index, void* vibration) {
    typedef DWORD(WINAPI * Fn)(DWORD, void*);
    static Fn fn = reinterpret_cast<Fn>(RealProc("XInputSetState"));
    return fn ? fn(index, vibration) : ERROR_DEVICE_NOT_CONNECTED;
}

__declspec(dllexport) DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, void* caps) {
    typedef DWORD(WINAPI * Fn)(DWORD, DWORD, void*);
    static Fn fn = reinterpret_cast<Fn>(RealProc("XInputGetCapabilities"));
    return fn ? fn(index, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

__declspec(dllexport) DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, void* render, void* capture) {
    typedef DWORD(WINAPI * Fn)(DWORD, void*, void*);
    static Fn fn = reinterpret_cast<Fn>(RealProc("XInputGetDSoundAudioDeviceGuids"));
    return fn ? fn(index, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

}  // extern "C"

// =============================================================================================
// Globals, config, logging
// =============================================================================================
struct Config {
    int startupDelayMs = 1500;
    bool nativeMods = true;
    std::vector<std::string> speedModules = {"GeometryDash.exe", "libcocos2d.dll", "libExtensions.dll"};
    double minSpeed = 0.05, maxSpeed = 10.0;
    double baseFps = 60.0, maxFps = 100000.0;
    bool taintOnFps = false;
    std::vector<std::string> blockList = {"uploadGJLevel", "updateGJUserScore", "getGJLevelScores", "uploadGJComment"};
};

static Config g_cfg;
static HMODULE g_self = nullptr;
static CRITICAL_SECTION g_logLock;   // log file
static CRITICAL_SECTION g_lock;      // status text, taint reason, hotkeys, ticks, fps target
static CRITICAL_SECTION g_timeLock;  // the hot path: scaled clocks
static std::string g_gdDir, g_root, g_modsDir, g_logPath, g_iniPath;
static uintptr_t g_base = 0;
static size_t g_imageSize = 0;
static uint32_t g_stamp = 0;

static void LogLine(const std::string& who, const std::string& msg) {
    EnterCriticalSection(&g_logLock);
    if (!g_logPath.empty()) {
        FILE* f = fopen(g_logPath.c_str(), "a");
        if (f) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(f, "[%02d:%02d:%02d] [%s] %s\n", t.wHour, t.wMinute, t.wSecond, who.c_str(), msg.c_str());
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_logLock);
}

static std::string IniStr(const char* section, const char* key, const std::string& def) {
    char buf[2048] = {0};
    GetPrivateProfileStringA(section, key, def.c_str(), buf, sizeof(buf), g_iniPath.c_str());
    return buf;
}

static double IniDouble(const char* section, const char* key, double def) {
    std::string s = IniStr(section, key, "");
    if (s.empty()) return def;
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    return (end && end != s.c_str()) ? v : def;
}

static void LoadConfig() {
    g_cfg.startupDelayMs = static_cast<int>(GetPrivateProfileIntA("loader", "startup_delay_ms", 1500, g_iniPath.c_str()));
    g_cfg.nativeMods = GetPrivateProfileIntA("loader", "native_mods", 1, g_iniPath.c_str()) != 0;
    auto sm = gocode::SplitCsv(IniStr("speedhack", "modules", ""));
    if (!sm.empty()) g_cfg.speedModules = sm;
    g_cfg.minSpeed = IniDouble("speedhack", "min_speed", 0.05);
    g_cfg.maxSpeed = IniDouble("speedhack", "max_speed", 10.0);
    if (g_cfg.minSpeed <= 0.0) g_cfg.minSpeed = 0.05;
    if (g_cfg.maxSpeed < 1.0) g_cfg.maxSpeed = 1.0;
    g_cfg.baseFps = gocode::ClampDouble(IniDouble("fps", "base_fps", 60.0), 1.0, 100000.0);
    g_cfg.maxFps = gocode::ClampDouble(IniDouble("fps", "max_fps", 100000.0), 1.0, 1000000.0);
    g_cfg.taintOnFps = GetPrivateProfileIntA("fps", "taint_on_fps", 0, g_iniPath.c_str()) != 0;
    auto bl = gocode::SplitCsv(IniStr("safe_mode", "block", ""));
    if (!bl.empty()) g_cfg.blockList = bl;
}

// =============================================================================================
// Image info, memory patching, pattern scanning
// =============================================================================================
static void InitImageInfo() {
    HMODULE m = GetModuleHandleA(nullptr);
    g_base = reinterpret_cast<uintptr_t>(m);
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(m);
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(g_base + dos->e_lfanew);
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    g_stamp = nt->FileHeader.TimeDateStamp;
}

static bool InImage(uintptr_t addr, size_t len) {
    return addr >= g_base && len <= g_imageSize && addr - g_base <= g_imageSize - len;
}

static bool PatchBytes(uintptr_t addr, const uint8_t* bytes, size_t len) {
    if (!InImage(addr, len) || len == 0) return false;
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    memcpy(reinterpret_cast<void*>(addr), bytes, len);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), len, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
    return true;
}

// Scans the readable, committed parts of GeometryDash.exe's image. Returns 0 if not found.
static uintptr_t ScanImage(const gocode::Pattern& pat) {
    uintptr_t cur = g_base, end = g_base + g_imageSize;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi))) break;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        uintptr_t scanEnd = std::min(regionEnd, end);
        bool readable = mbi.State == MEM_COMMIT && mbi.Protect != 0 && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (readable && scanEnd > cur) {
            size_t off = gocode::findPattern(reinterpret_cast<const uint8_t*>(cur), scanEnd - cur, pat);
            if (off != gocode::npos) return cur + off;
        }
        cur = regionEnd;
    }
    return 0;
}

// =============================================================================================
// PE helpers: module list, export lookup, import-table (IAT) hooking
// =============================================================================================
struct ModInfo {
    std::string name;
    HMODULE handle;
};

static std::vector<ModInfo> ListModules() {
    std::vector<ModInfo> out;
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 5; ++attempt) {  // can fail with ERROR_BAD_LENGTH while modules load
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE) break;
        Sleep(20);
    }
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32 me;
    memset(&me, 0, sizeof(me));
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            out.push_back({me.szModule, reinterpret_cast<HMODULE>(me.hModule)});
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return out;
}

static bool NameEquals(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

// First non-forwarded export whose (mangled) name contains both substrings. Used so we do not depend on
// exact C++ name mangling.
static void* FindExportContaining(HMODULE mod, const char* a, const char* b) {
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress) return nullptr;
    auto exp = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(base + dd.VirtualAddress);
    auto names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
    auto ords = reinterpret_cast<WORD*>(base + exp->AddressOfNameOrdinals);
    auto funcs = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* n = reinterpret_cast<const char*>(base + names[i]);
        if (strstr(n, a) && strstr(n, b)) {
            DWORD rva = funcs[ords[i]];
            if (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size) continue;  // forwarder
            return base + rva;
        }
    }
    return nullptr;
}

// Replaces every import of `funcName` (from any DLL) in `mod`'s import table. Returns how many entries changed.
static int HookIAT(HMODULE mod, const char* funcName, void* replacement) {
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return 0;

    int patched = 0;
    auto imp = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + dd.VirtualAddress);
    for (; imp->Name; ++imp) {
        auto lookup = reinterpret_cast<PIMAGE_THUNK_DATA32>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = reinterpret_cast<PIMAGE_THUNK_DATA32>(base + imp->FirstThunk);
        for (; lookup->u1.AddressOfData; ++lookup, ++iat) {
            if (lookup->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;  // imported by ordinal
            auto ibn = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + lookup->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) != 0) continue;
            DWORD oldProt = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(DWORD), PAGE_READWRITE, &oldProt)) continue;
            iat->u1.Function = static_cast<DWORD>(reinterpret_cast<uintptr_t>(replacement));
            DWORD tmp = 0;
            VirtualProtect(&iat->u1.Function, sizeof(DWORD), oldProt, &tmp);
            ++patched;
        }
    }
    return patched;
}

// =============================================================================================
// Safe Mode: taint flag + network guard (blocks score / level uploads once tainted)
// =============================================================================================
static volatile LONG g_tainted = 0;
static std::string g_taintReason;  // guarded by g_lock
static bool g_guardActive = false;

static void UpdateTitle();

static void TaintSession(const char* reason) {
    bool first = InterlockedCompareExchange(&g_tainted, 1, 0) == 0;
    if (first) {
        EnterCriticalSection(&g_lock);
        g_taintReason = reason ? reason : "unspecified";
        LeaveCriticalSection(&g_lock);
        LogLine("safe-mode", std::string("session tainted by '") + (reason ? reason : "unspecified") +
                                 "': score / level uploads are blocked until Geometry Dash is restarted");
    }
}

static const char* const kBlockedUrl = "http://127.0.0.1:1/gocode-safe-mode-blocked";

extern "C" {
// The real curl_easy_setopt. Must be set before any import is patched.
__attribute__((used)) void* g_realCurlSetopt = nullptr;

// Called from the thunk below with the URL being set. Returns the URL curl should actually use.
__attribute__((used)) const char* __cdecl GoCode_FilterUrl(const char* url) {
    if (!url || !g_tainted) return url;
    if (gocode::ShouldBlockUrl(url, g_cfg.blockList)) {
        LogLine("safe-mode", std::string("blocked request: ") + url);
        return kBlockedUrl;
    }
    return url;
}

void GoCode_CurlSetoptThunk();  // defined in the asm block below
}

// curl_easy_setopt(CURL*, CURLoption, ...) is cdecl + varargs, so it cannot be wrapped by a normal function without
// mangling the arguments of other options. This thunk only touches the stack when the option is CURLOPT_URL (10002)
// and otherwise jumps straight to the real function.
asm(".text\n"
    ".globl _GoCode_CurlSetoptThunk\n"
    "_GoCode_CurlSetoptThunk:\n"
    "    cmpl $10002, 8(%esp)\n"
    "    jne 1f\n"
    "    pushl 12(%esp)\n"
    "    call _GoCode_FilterUrl\n"
    "    addl $4, %esp\n"
    "    movl %eax, 12(%esp)\n"
    "1:\n"
    "    jmp *_g_realCurlSetopt\n");

static bool InstallGuard() {
    auto mods = ListModules();
    void* real = nullptr;
    for (auto& m : mods) {
        if (m.handle == g_self) continue;
        FARPROC p = GetProcAddress(m.handle, "curl_easy_setopt");
        if (p) {
            real = reinterpret_cast<void*>(p);
            LogLine("safe-mode", "found curl in " + m.name);
            break;
        }
    }
    if (!real) {
        LogLine("safe-mode", "curl_easy_setopt not found in any loaded module - network guard NOT installed");
        return false;
    }
    g_realCurlSetopt = real;
    int patched = 0;
    for (auto& m : mods) {
        if (m.handle == g_self) continue;
        int n = HookIAT(m.handle, "curl_easy_setopt", reinterpret_cast<void*>(&GoCode_CurlSetoptThunk));
        if (n) LogLine("safe-mode", "guarding " + std::to_string(n) + " curl import(s) in " + m.name);
        patched += n;
    }
    if (!patched) {
        LogLine("safe-mode", "no module imports curl_easy_setopt - network guard NOT installed");
        return false;
    }
    return true;
}

// =============================================================================================
// Game speed: scaled clocks (QueryPerformanceCounter, GetTickCount[64], timeGetTime)
// =============================================================================================
typedef BOOL(WINAPI* QPC_t)(LARGE_INTEGER*);
typedef ULONGLONG(WINAPI* GTC64_t)(void);
typedef DWORD(WINAPI* TGT_t)(void);

static QPC_t g_realQPC = nullptr;
static GTC64_t g_realGTC64 = nullptr;
static TGT_t g_realTGT = nullptr;
static gocode::TimeScaler g_qpc, g_tick, g_mm;  // guarded by g_timeLock

static BOOL WINAPI Hook_QPC(LARGE_INTEGER* out) {
    LARGE_INTEGER real;
    if (!g_realQPC(&real)) return FALSE;
    EnterCriticalSection(&g_timeLock);
    out->QuadPart = g_qpc.Map(real.QuadPart);
    LeaveCriticalSection(&g_timeLock);
    return TRUE;
}

static ULONGLONG WINAPI Hook_GetTickCount64() {
    ULONGLONG real = g_realGTC64();
    EnterCriticalSection(&g_timeLock);
    ULONGLONG v = static_cast<ULONGLONG>(g_tick.Map(static_cast<int64_t>(real)));
    LeaveCriticalSection(&g_timeLock);
    return v;
}

static DWORD WINAPI Hook_GetTickCount() { return static_cast<DWORD>(Hook_GetTickCount64()); }

static DWORD WINAPI Hook_timeGetTime() {
    DWORD real = g_realTGT();
    EnterCriticalSection(&g_timeLock);
    DWORD v = static_cast<DWORD>(g_mm.Map(static_cast<int64_t>(real)));
    LeaveCriticalSection(&g_timeLock);
    return v;
}

static bool InstallTimeHooks() {
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_realQPC = reinterpret_cast<QPC_t>(GetProcAddress(k32, "QueryPerformanceCounter"));
    g_realGTC64 = reinterpret_cast<GTC64_t>(GetProcAddress(k32, "GetTickCount64"));
    HMODULE wm = LoadLibraryA("winmm.dll");
    g_realTGT = wm ? reinterpret_cast<TGT_t>(GetProcAddress(wm, "timeGetTime")) : nullptr;
    if (!g_realQPC || !g_realGTC64) {
        LogLine("speed", "kernel32 timing functions not found - game speed unavailable");
        return false;
    }
    int patched = 0;
    for (auto& m : ListModules()) {
        bool wanted = false;
        for (auto& w : g_cfg.speedModules) wanted = wanted || NameEquals(m.name, w);
        if (!wanted || m.handle == g_self) continue;
        int n = HookIAT(m.handle, "QueryPerformanceCounter", reinterpret_cast<void*>(&Hook_QPC));
        n += HookIAT(m.handle, "GetTickCount64", reinterpret_cast<void*>(&Hook_GetTickCount64));
        n += HookIAT(m.handle, "GetTickCount", reinterpret_cast<void*>(&Hook_GetTickCount));
        if (g_realTGT) n += HookIAT(m.handle, "timeGetTime", reinterpret_cast<void*>(&Hook_timeGetTime));
        if (n) LogLine("speed", "hooked " + std::to_string(n) + " clock import(s) in " + m.name);
        patched += n;
    }
    return patched > 0;
}

static bool g_timeHooked = false;

static void RescaleClocks(double s) {
    LARGE_INTEGER q;
    q.QuadPart = 0;
    g_realQPC(&q);
    int64_t t = static_cast<int64_t>(g_realGTC64());
    int64_t m = g_realTGT ? static_cast<int64_t>(g_realTGT()) : 0;
    EnterCriticalSection(&g_timeLock);
    g_qpc.SetScale(s, q.QuadPart);
    g_tick.SetScale(s, t);
    if (g_realTGT) g_mm.SetScale(s, m);
    LeaveCriticalSection(&g_timeLock);
}

static double CurrentSpeed() {
    EnterCriticalSection(&g_timeLock);
    double s = g_qpc.Scale();
    LeaveCriticalSection(&g_timeLock);
    return s;
}

// =============================================================================================
// Frame rate: CCApplication::setAnimationInterval (exported by libcocos2d.dll)
// =============================================================================================
typedef void*(__cdecl* SharedApp_t)(void);
typedef void(__attribute__((thiscall)) * SetInterval_t)(void* self, double seconds);

static SharedApp_t g_sharedApp = nullptr;
static SetInterval_t g_setInterval = nullptr;
static double g_fpsTarget = 0.0;  // 0 = bypass off; guarded by g_lock
static bool g_fpsTouched = false; // we have changed the interval at least once

static bool ResolveFps() {
    if (g_sharedApp && g_setInterval) return true;
    HMODULE cc = GetModuleHandleA("libcocos2d.dll");
    if (!cc) return false;
    g_sharedApp = reinterpret_cast<SharedApp_t>(FindExportContaining(cc, "sharedApplication", "CCApplication"));
    g_setInterval = reinterpret_cast<SetInterval_t>(FindExportContaining(cc, "setAnimationInterval", "CCApplication"));
    return g_sharedApp && g_setInterval;
}

// Applies the current FPS target, compensating for the clock scale so the REAL frame rate stays what was asked for.
static void ApplyFps() {
    if (!ResolveFps()) return;
    void* app = g_sharedApp();
    if (!app) return;
    EnterCriticalSection(&g_lock);
    double fps = g_fpsTarget > 0.0 ? g_fpsTarget : g_cfg.baseFps;
    LeaveCriticalSection(&g_lock);
    double interval = CurrentSpeed() / fps;  // seconds of *game* time between frames
    g_setInterval(app, interval);
    g_fpsTouched = true;
}

// =============================================================================================
// Hotkeys, ticks, window-title status
// =============================================================================================
struct Hotkey {
    uint32_t vk, mods;
    GoCodeCallback cb;
    void* user;
    bool down;
};
struct Tick {
    uint32_t intervalMs;
    GoCodeCallback cb;
    void* user;
    DWORD next;
};
static std::vector<Hotkey> g_hotkeys;                        // guarded by g_lock
static std::vector<Tick> g_ticks;                            // guarded by g_lock
static std::vector<std::pair<std::string, std::string>> g_status;  // mod -> text; guarded by g_lock

static bool GameFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static BOOL CALLBACK FindGameWindowProc(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr) return TRUE;
    char title[256] = {0};
    if (GetWindowTextA(h, title, sizeof(title)) > 0 && strstr(title, "Geometry Dash")) {
        *reinterpret_cast<HWND*>(lp) = h;
        return FALSE;
    }
    return TRUE;
}

static void UpdateTitle() {
    static HWND wnd = nullptr;
    static std::string baseTitle, lastSent;
    if (!wnd || !IsWindow(wnd)) {
        wnd = nullptr;
        EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&wnd));
        if (!wnd) return;
        char title[256] = {0};
        GetWindowTextA(wnd, title, sizeof(title));
        baseTitle = title;
        size_t bar = baseTitle.find(" | ");
        if (bar != std::string::npos) baseTitle.resize(bar);
        lastSent.clear();
    }
    std::string text = baseTitle;
    EnterCriticalSection(&g_lock);
    for (auto& s : g_status)
        if (!s.second.empty()) text += " | " + s.second;
    if (g_tainted) text += " | SAFE MODE: uploads blocked (" + g_taintReason + ")";
    LeaveCriticalSection(&g_lock);
    if (text == lastSent) return;
    DWORD_PTR res = 0;
    SendMessageTimeoutA(wnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()), SMTO_ABORTIFHUNG, 200, &res);
    lastSent = text;
}

static DWORD WINAPI ControlThread(LPVOID) {
    DWORD lastSlow = GetTickCount();
    for (;;) {
        Sleep(15);
        DWORD now = GetTickCount();  // our own module's import is not hooked: this is real time
        bool focused = GameFocused();

        std::vector<std::pair<GoCodeCallback, void*>> fire;
        EnterCriticalSection(&g_lock);
        bool ctrl = KeyDown(VK_CONTROL), shift = KeyDown(VK_SHIFT), alt = KeyDown(VK_MENU);
        for (auto& h : g_hotkeys) {
            bool modsOk = ((h.mods & GOCODE_MOD_CTRL) != 0) == ctrl && ((h.mods & GOCODE_MOD_SHIFT) != 0) == shift &&
                          ((h.mods & GOCODE_MOD_ALT) != 0) == alt;
            bool pressed = modsOk && KeyDown(static_cast<int>(h.vk));
            if (pressed && !h.down && focused) fire.push_back({h.cb, h.user});
            h.down = pressed;
        }
        for (auto& t : g_ticks) {
            if (static_cast<int32_t>(now - t.next) >= 0) {
                fire.push_back({t.cb, t.user});
                t.next = now + t.intervalMs;
            }
        }
        LeaveCriticalSection(&g_lock);
        for (auto& f : fire) {
            try {
                f.first(f.second);
            } catch (...) {
                LogLine("loader", "a mod callback threw an exception");
            }
        }

        if (now - lastSlow >= 500) {
            lastSlow = now;
            if (g_fpsTouched) ApplyFps();  // GD (or a pause/resume) may have reset the interval
            UpdateTitle();
        }
    }
    return 0;
}

// =============================================================================================
// Native mod API
// =============================================================================================
static void ApiLog(const char* mod, const char* msg) { LogLine(mod ? mod : "native", msg ? msg : ""); }

static int ApiPatch(uintptr_t addr, const uint8_t* bytes, size_t len) {
    return (bytes && PatchBytes(addr, bytes, len)) ? 1 : 0;
}

static uintptr_t ApiScan(const char* pattern) {
    gocode::Pattern p;
    if (!pattern || !gocode::parsePattern(pattern, p)) return 0;
    return ScanImage(p);
}

static int ApiSetSpeed(double s) {
    if (!(s == s)) return 0;  // NaN
    s = gocode::ClampDouble(s, g_cfg.minSpeed, g_cfg.maxSpeed);
    if (s != 1.0) {
        // Fail closed: never change the game speed unless the Safe Mode guard can stop the results being uploaded.
        if (!g_guardActive || !g_timeHooked) {
            LogLine("speed", "refused: needs the Safe Mode network guard and the clock hooks (see earlier log lines)");
            return 0;
        }
        TaintSession("speedhack");
    }
    RescaleClocks(s);
    if (g_fpsTouched || s != 1.0) ApplyFps();
    return 1;
}

static double ApiGetSpeed() { return CurrentSpeed(); }

static int ApiSetFps(double fps) {
    if (!(fps == fps) || fps < 0.0) return 0;
    if (fps > 0.0) fps = gocode::ClampDouble(fps, 1.0, g_cfg.maxFps);
    if (fps > 0.0 && g_cfg.taintOnFps) TaintSession("fps bypass");
    EnterCriticalSection(&g_lock);
    g_fpsTarget = fps;
    LeaveCriticalSection(&g_lock);
    if (!ResolveFps()) {
        LogLine("fps", "CCApplication::setAnimationInterval not found in libcocos2d.dll - FPS control unavailable");
        return 0;
    }
    ApplyFps();
    return 1;
}

static double ApiGetFps() {
    EnterCriticalSection(&g_lock);
    double f = g_fpsTarget;
    LeaveCriticalSection(&g_lock);
    return f;
}

static int ApiIsTainted() { return g_tainted ? 1 : 0; }
static void ApiTaint(const char* reason) { TaintSession(reason); }
static int ApiGuardActive() { return g_guardActive ? 1 : 0; }

static int ApiRegisterHotkey(uint32_t vk, uint32_t mods, GoCodeCallback cb, void* user) {
    if (!cb || vk == 0 || vk > 255) return 0;
    EnterCriticalSection(&g_lock);
    g_hotkeys.push_back({vk, mods, cb, user, false});
    LeaveCriticalSection(&g_lock);
    return 1;
}

static int ApiRegisterTick(uint32_t intervalMs, GoCodeCallback cb, void* user) {
    if (!cb || intervalMs < 10) return 0;
    EnterCriticalSection(&g_lock);
    g_ticks.push_back({intervalMs, cb, user, GetTickCount() + intervalMs});
    LeaveCriticalSection(&g_lock);
    return 1;
}

static void ApiSetStatus(const char* mod, const char* text) {
    std::string m = mod ? mod : "", t = text ? text : "";
    EnterCriticalSection(&g_lock);
    bool found = false;
    for (auto& s : g_status)
        if (s.first == m) {
            s.second = t;
            found = true;
        }
    if (!found) g_status.push_back({m, t});
    LeaveCriticalSection(&g_lock);
}

static const char* ApiGocodeDir() { return g_root.c_str(); }

static GoCodeAPI g_api;

// =============================================================================================
// Mod loading
// =============================================================================================
static std::string BaseName(const std::string& path) {
    size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? path : path.substr(s + 1);
}

// ---- GoCode scripts ----
// Everything a script may do goes through this class; it maps onto the same services native mods use.
class LoaderHost : public gocode::Host {
public:
    void Log(const std::string& mod, const std::string& msg) override { LogLine(mod, msg); }
    double GetSpeed() override { return ApiGetSpeed(); }
    bool SetSpeed(double s) override { return ApiSetSpeed(s) == 1; }
    double GetFps() override { return ApiGetFps(); }
    bool SetFps(double f) override { return ApiSetFps(f) == 1; }
    bool Tainted() override { return g_tainted != 0; }
    void Taint(const std::string& reason) override { TaintSession(reason.c_str()); }
    bool GuardActive() override { return g_guardActive; }
    void SetStatus(const std::string& mod, const std::string& text) override { ApiSetStatus(mod.c_str(), text.c_str()); }
    bool Patch(uint64_t addr, const std::vector<uint8_t>& bytes) override {
        return addr <= 0xFFFFFFFFull && !bytes.empty() && PatchBytes(static_cast<uintptr_t>(addr), bytes.data(), bytes.size());
    }
    uint64_t Scan(const std::string& pattern) override { return ApiScan(pattern.c_str()); }
    bool Read(uint64_t addr, size_t count, std::vector<uint8_t>& out) override {
        if (addr > 0xFFFFFFFFull || count == 0) return false;
        out.assign(count, 0);
        SIZE_T got = 0;  // ReadProcessMemory on ourselves fails cleanly instead of crashing on a bad address
        BOOL ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)), out.data(), count, &got);
        return ok && got == count;
    }
    uint64_t GdBase() override { return g_base; }
    uint32_t GdStamp() override { return g_stamp; }
    bool Config(const std::string& slug, const std::string& key, std::string& out) override {
        // <GD>\gocode\config\<slug>.cfg : "key=value" lines, '#' or ';' start a comment. Re-read on every call.
        std::ifstream in(g_root + "\\config\\" + slug + ".cfg");
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            auto trim = [](std::string v) {
                size_t a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
                return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
            };
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (_stricmp(trim(line.substr(0, eq)).c_str(), key.c_str()) == 0) {
                out = trim(line.substr(eq + 1));
                return true;
            }
        }
        return false;
    }
    int64_t NowMs() override { return static_cast<int64_t>(g_realGTC64 ? g_realGTC64() : GetTickCount()); }
};

static LoaderHost g_host;
static std::vector<std::unique_ptr<gocode::Runtime>> g_scripts;  // live for the whole process

struct ScriptEvent {
    gocode::Runtime* rt;
    size_t index;
};

// Called by the control thread for every hotkey / tick registered by a script. Scripts therefore always run on one
// thread, one handler at a time.
static void __cdecl ScriptCallback(void* user) {
    auto* e = static_cast<ScriptEvent*>(user);
    e->rt->RunEvent(e->index);
}

static void RunScriptMod(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LogLine("loader", "cannot open " + path);
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    std::string slug = BaseName(path);
    if (slug.size() > 7 && _stricmp(slug.c_str() + slug.size() - 7, ".gocode") == 0) slug.resize(slug.size() - 7);

    auto rt = std::make_unique<gocode::Runtime>(&g_host, slug, slug);
    std::vector<std::string> errors;
    if (!rt->Parse(ss.str(), errors)) {
        for (auto& e : errors) LogLine(slug, "syntax error, " + e);
        LogLine(slug, "mod skipped");
        return;
    }
    std::string label = rt->name().empty() ? slug : rt->name();
    rt->SetLabel(label);
    if (rt->hasStamp() && rt->stamp() != g_stamp) {
        char buf[160];
        snprintf(buf, sizeof(buf), "built for a different Geometry Dash version (needs stamp 0x%08X, game has 0x%08X) - skipped",
                 (unsigned)rt->stamp(), (unsigned)g_stamp);
        LogLine(label, buf);
        return;
    }
    if (!rt->Init()) {
        LogLine(label, "mod skipped (error while starting)");
        return;
    }

    gocode::Runtime* raw = rt.get();
    g_scripts.push_back(std::move(rt));

    int keys = 0, ticks = 0;
    std::vector<size_t> starts;
    const auto& events = raw->events();
    for (size_t i = 0; i < events.size(); ++i) {
        auto* ev = new ScriptEvent{raw, i};  // intentionally never freed: the callbacks live as long as the game
        if (events[i].kind == gocode::EventInfo::Key) {
            if (ApiRegisterHotkey(events[i].vk, events[i].mods, ScriptCallback, ev)) ++keys;
        } else if (events[i].kind == gocode::EventInfo::Tick) {
            if (ApiRegisterTick(events[i].intervalMs, ScriptCallback, ev)) ++ticks;
        } else {
            starts.push_back(i);
            delete ev;
        }
    }
    for (size_t i : starts) raw->RunEvent(i);
    LogLine(label, "loaded (" + std::to_string(keys) + " hotkey(s), " + std::to_string(ticks) + " timer(s))");
}

static void RunNativeMod(const std::string& path) {
    std::string label = BaseName(path);
    HMODULE h = LoadLibraryA(path.c_str());
    if (!h) {
        LogLine(label, "LoadLibrary failed, error " + std::to_string(GetLastError()) + " (is it a 32-bit DLL?)");
        return;
    }
    auto init = reinterpret_cast<GoCodeInitFn>(GetProcAddress(h, "GoCode_Init"));
    if (!init) {
        LogLine(label, "no GoCode_Init export - not a GoCode mod, unloading");
        FreeLibrary(h);
        return;
    }
    try {
        init(&g_api);
        LogLine(label, "native mod initialised");
    } catch (...) {
        LogLine(label, "GoCode_Init threw an exception");
    }
}

static std::vector<std::string> ListFiles(const std::string& dir, const char* pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + "\\" + fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

static DWORD WINAPI StartupThread(LPVOID) {
    CreateDirectoryA(g_root.c_str(), nullptr);
    CreateDirectoryA(g_modsDir.c_str(), nullptr);
    LoadConfig();
    if (g_cfg.startupDelayMs > 0) Sleep(static_cast<DWORD>(g_cfg.startupDelayMs));

    if (FILE* f = fopen(g_logPath.c_str(), "w")) fclose(f);  // fresh log each launch

    InitImageInfo();
    char hdr[160];
    snprintf(hdr, sizeof(hdr), "GoCode loader " GOCODE_LOADER_VERSION " - GD base 0x%08X, version stamp 0x%08X",
             (unsigned)g_base, (unsigned)g_stamp);
    LogLine("loader", hdr);

    // Core services. Order matters: the Safe Mode guard goes in before anything can change the game speed.
    g_guardActive = InstallGuard();
    LogLine("safe-mode", g_guardActive ? "network guard active" : "network guard UNAVAILABLE - game speed changes will be refused");
    g_timeHooked = InstallTimeHooks();
    LogLine("speed", g_timeHooked ? "clock hooks active (speed 1.0x)" : "clock hooks UNAVAILABLE");
    LogLine("fps", ResolveFps() ? "libcocos2d frame-rate control found" : "libcocos2d frame-rate control not found (yet)");

    memset(&g_api, 0, sizeof(g_api));
    g_api.version = GOCODE_API_VERSION;
    g_api.gdBase = g_base;
    g_api.gdStamp = g_stamp;
    g_api.log = ApiLog;
    g_api.patch = ApiPatch;
    g_api.scan = ApiScan;
    g_api.set_speed = ApiSetSpeed;
    g_api.get_speed = ApiGetSpeed;
    g_api.set_fps = ApiSetFps;
    g_api.get_fps = ApiGetFps;
    g_api.is_tainted = ApiIsTainted;
    g_api.taint = ApiTaint;
    g_api.guard_active = ApiGuardActive;
    g_api.register_hotkey = ApiRegisterHotkey;
    g_api.register_tick = ApiRegisterTick;
    g_api.set_status = ApiSetStatus;
    g_api.gocode_dir = ApiGocodeDir;

    auto scripts = ListFiles(g_modsDir, "*.gocode");
    auto dlls = ListFiles(g_modsDir, "*.dll");
    for (auto& p : scripts) RunScriptMod(p);
    if (g_cfg.nativeMods) {
        for (auto& p : dlls) RunNativeMod(p);
    } else if (!dlls.empty()) {
        LogLine("loader", "native mods disabled in gocode.ini - skipped " + std::to_string(dlls.size()) + " dll(s)");
    }
    LogLine("loader", "done: " + std::to_string(scripts.size()) + " script(s), " +
                          std::to_string(g_cfg.nativeMods ? dlls.size() : 0) + " native mod(s)");

    HANDLE t = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_self = inst;
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_timeLock);

        char exe[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string full(exe);
        size_t slash = full.find_last_of("\\/");
        std::string name = slash == std::string::npos ? full : full.substr(slash + 1);

        // Only run inside Geometry Dash itself; anywhere else we are just an XInput forwarder.
        if (slash != std::string::npos && _stricmp(name.c_str(), "GeometryDash.exe") == 0) {
            g_gdDir = full.substr(0, slash);
            g_root = g_gdDir + "\\gocode";
            g_modsDir = g_root + "\\mods";
            g_logPath = g_root + "\\gocode.log";
            g_iniPath = g_root + "\\gocode.ini";
            HANDLE t = CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
