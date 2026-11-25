# GoCode - a Geometry Dash mod loader with its own mod language

Mods are written in **GoCode**, a small scripting language that the loader runs itself. A mod is a plain text file
(`something.gocode`) - no compiler, no SDK, nothing to install. Players and modders only need the installer.

* `GoCodeInstaller.exe` - one self-contained exe (no runtime needed). Asks you to locate your Steam Geometry Dash folder,
  downloads the loader and installs it.
* `XInput9_1_0.dll` - the loader: one self-contained 32-bit DLL (no dependencies). Runs GoCode mods inside the game and
  provides **game speed**, **frame-rate control**, **Safe Mode**, hotkeys and byte patching.
* `mods\*.gocode` - the built-in **FPS bypass**, **speedhack** and **Safe Mode** status are ordinary GoCode mods. Read them,
  copy them, change them.

## Getting the exe (for whoever publishes it)

Push this folder to GitHub. `.github/workflows/build.yml` builds everything on GitHub's Windows machines. Push a tag
(`git tag v0.3.0 && git push --tags`) and it publishes a Release with `GoCodeInstaller.exe` and `gocode-payload.zip`.
The installer downloads that zip from your latest release and verifies its checksum. (Building locally instead needs Go and a
32-bit MinGW-w64: run `build.bat`.)

## Using it

Run `GoCodeInstaller.exe`, pick your Geometry Dash folder (the window opens on the auto-detected one), choose *Install*.
Close the game first. Uninstall = run it again, option 2. Check `<Geometry Dash>\gocode\gocode.log` to see what loaded.

| Key (game focused) | Action |
|---|---|
| F5 / F6 | speed down / up (step 0.1) |
| F7 | speed back to 1.0x |
| F8 | next FPS preset (60, 120, 240, 360, 480, 1000, then off) |
| Shift+F8 | FPS bypass off |

The window title shows the state: `Geometry Dash | speed 1.50x | 240 FPS | SAFE MODE: uploads blocked (speedhack)`.

**Any value:** put it in a settings file while the game runs (applies within a second):
`gocode\config\fps-bypass.cfg` -> `fps=1234` (0 = off), `gocode\config\speedhack.cfg` -> `speed=1.5`.
Turn **VSync off** in Geometry Dash's video settings or your monitor's refresh rate caps the FPS.

## Safe Mode (built in, not optional)

Any game speed other than 1.0x - or any mod calling `taint("reason")` - puts the session into Safe Mode: requests that submit
scores or level data (`uploadGJLevel`, `updateGJUserScore`, `getGJLevelScores`, `uploadGJComment` by default; list in
`gocode.ini`) are blocked until Geometry Dash is restarted, so a sped-up run can never verify and publish a level or post a record.
It is enforced by the loader, not by a script, so no mod can turn it off. If the loader cannot install the guard it
**refuses to change the speed at all**. (`taint_on_fps=1` in `gocode.ini` treats the FPS bypass the same way.)

## The GoCode language

```go
mod "My Mod"                  # name shown in the log
author "me"
version "1.0.0"
# require_stamp 0x12345678    # only run on this exact game build (the loader logs your build's stamp)

let jump_boost = cfg("boost", 1.5)     # cfg(): reads gocode\config\<this file's name>.cfg, default 1.5

fn show(s) {                           # functions (global, may recurse)
    if s == 1 { status("") } else { status(format("speed %.2fx", s)) }
}

on start { log("hello") }              # runs once, after the game has started
on key ctrl+shift+F9 { set_speed(speed() + 0.1)  show(speed()) }
on tick 500ms { }                      # every 500ms (10ms minimum; also 2s, or a plain number of ms)
```

* **Values:** numbers (`1`, `0.5`, `0xFF`, `1s` = 1000), strings (`"a\n"`), `true`/`false`, `nil`, lists (`[1, 2]`, `a[0]`).
  Only `false` and `nil` are false (`0` is true). `&&`/`||` return an operand, like `x = cfg("a") || 5`.
* **Statements:** `let x = 1`, `x += 1`, `if / else if / else`, `while`, `for v in list`, `break`, `continue`, `return`.
  A newline ends a statement; `;` also works; `#` or `//` start a comment.
* **Built-ins:** `log(...)`, `format("%d %.2f %s %X", ...)`, `str num len type`, `abs floor ceil round(x, digits) sqrt pow min max clamp`,
  `push range bytes("90 EB") join contains lower upper`, `u16_bytes u32_bytes f32_bytes`.
* **Game:** `speed() set_speed(x) fps() set_fps(x) tainted() taint(reason) guard_active() status(text) gd_base() gd_stamp() now_ms() cfg(key, default)`.
* **Memory** (`GeometryDash.exe` only for writes): `scan("8B 45 ?? 85 C0")` -> address or 0, `patch(addr, [0x90, 0x90])`,
  `nop(addr, n)`, `read_bytes read_u8 read_u16 read_u32 read_f32` (nil if unreadable). See `examples\memory-patch.gocode`.

Scripts are sandboxed: they cannot touch files, the network, or other programs - only what the built-ins above allow.
Each handler has a step limit (endless loops are stopped and logged) and a handler that errors five times is switched off.
Syntax errors are reported with the line number in `gocode.log` and the mod is skipped; the game is never affected.

Optional: advanced authors can also write native 32-bit DLL mods in C/C++ against `sdk\gocode.h` (`examples\native-c`).

## How it works

* **Speed**: the game's clock imports (`QueryPerformanceCounter`, `GetTickCount[64]`, `timeGetTime`) in `GeometryDash.exe`,
  `libcocos2d.dll` and `libExtensions.dll` are redirected to scaled clocks. No game-version offsets involved.
* **FPS**: calls `CCApplication::setAnimationInterval` in `libcocos2d.dll` (found by scanning its exports) and re-applies it
  twice a second in case the game resets it. The speed multiplier is compensated so FPS stays what you asked for.
* **Safe Mode**: `curl_easy_setopt` imports are redirected; once tainted, blocked URLs are swapped for a dead local
  address so the request fails as if offline.

## Honest status

* **Tested here:** the GoCode language (lexer, parser, interpreter, ~250 checks, run under AddressSanitizer/UBSan) including
  the three shipped mods driven like the loader drives them, the clock-scaling and URL-blocking logic, and byte patterns.
  The loader DLL was syntax-checked against stub Windows headers.
* **Never run on Windows:** the loader's hooks, the installer, the build script and the workflow were written without a Windows,
  Go or MinGW toolchain. Expect a build fix or two on the first CI run, and read `gocode.log` on first launch.
* **Game updates:** the hooks assume Geometry Dash imports its clock and curl functions by name and that `libcocos2d.dll`
  exports `CCApplication::setAnimationInterval`. If that is not true for the current game build, the log says which feature
  could not be set up and why, and the speed stays locked at 1.0x (it fails closed). 32-bit only.
* No function detours or game bindings yet (most of what Geode's API adds). Memory offsets in scripts are per game version.
