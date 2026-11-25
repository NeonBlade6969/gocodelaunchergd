/* GoCode native mod SDK - API version 2.
 *
 * A native mod is a 32-bit DLL placed in  <Geometry Dash>\gocode\mods\  that exports
 *     void __cdecl GoCode_Init(const GoCodeAPI* api);
 * It is called once, on a loader thread, shortly after the game starts. Go mods get this through the
 * Go SDK in sdk\go instead of using this header directly.
 *
 * The struct only ever grows at the end, so a mod built against an older version keeps working.
 */
#ifndef GOCODE_H
#define GOCODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GOCODE_API_VERSION 2

/* Hotkey modifier bits. A hotkey fires only when exactly these modifiers are held. */
#define GOCODE_MOD_CTRL  1
#define GOCODE_MOD_SHIFT 2
#define GOCODE_MOD_ALT   4

typedef void (__cdecl *GoCodeCallback)(void* user);

typedef struct GoCodeAPI {
    /* ---- v1 ---- */
    uint32_t  version;   /* GOCODE_API_VERSION */
    uintptr_t gdBase;    /* base address of GeometryDash.exe in memory */
    uint32_t  gdStamp;   /* PE TimeDateStamp of GeometryDash.exe (identifies the game version) */
    void      (*log)(const char* modName, const char* message);                 /* -> gocode\gocode.log */
    int       (*patch)(uintptr_t address, const uint8_t* bytes, size_t length); /* 1 = ok */
    uintptr_t (*scan)(const char* pattern);                                     /* "8B 45 ?? 85 C0"; 0 = not found */

    /* ---- v2 ---- */
    /* Game speed multiplier (scales the game's clocks). Returns 1 if applied, 0 if refused.
     * Refused when the Safe Mode network guard could not be installed. Any speed != 1.0 puts the session
     * into Safe Mode (see is_tainted). */
    int       (*set_speed)(double speed);
    double    (*get_speed)(void);
    /* Target frame rate. 0 turns the bypass off (back to base_fps from gocode.ini). Returns 1 if applied. */
    int       (*set_fps)(double fps);
    double    (*get_fps)(void);
    /* Safe Mode: once tainted, uploads that could carry scores / level verification are blocked until the game
     * is restarted. Mods cannot clear it. */
    int       (*is_tainted)(void);
    void      (*taint)(const char* reason);   /* call this from any mod that gives an unfair advantage */
    int       (*guard_active)(void);          /* 1 if the network guard is installed */
    /* Hotkeys (polled while the game window is focused) and periodic ticks. Callbacks run on a loader thread. */
    int       (*register_hotkey)(uint32_t vk, uint32_t modifiers, GoCodeCallback cb, void* user);
    int       (*register_tick)(uint32_t intervalMs, GoCodeCallback cb, void* user);
    /* Text shown in the game window's title bar after "Geometry Dash". Empty string removes it. */
    void      (*set_status)(const char* modName, const char* text);
    const char* (*gocode_dir)(void);          /* <Geometry Dash>\gocode  (valid for the life of the process) */
} GoCodeAPI;

typedef void (__cdecl *GoCodeInitFn)(const GoCodeAPI* api);

#ifdef __cplusplus
}
#endif

#endif /* GOCODE_H */
