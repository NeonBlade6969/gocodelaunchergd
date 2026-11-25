/* Native GoCode mod in C.  Build (32-bit MinGW):
 *   i686-w64-mingw32-gcc -shared -O2 -I../../sdk -o hello-c.dll hello.c
 * Copy hello-c.dll to <Geometry Dash>\gocode\mods\
 */
#include <stdio.h>
#include "gocode.h"

__declspec(dllexport) void __cdecl GoCode_Init(const GoCodeAPI* api) {
    char msg[128];
    snprintf(msg, sizeof(msg), "Hello from C! GD base = 0x%08X, version stamp = 0x%08X",
             (unsigned)api->gdBase, (unsigned)api->gdStamp);
    api->log("hello-c", msg);
}
