// FF2 Remaster: runtime patches for Feeding Frenzy 2 Deluxe, loaded via the version.dll proxy.
#include <windows.h>
#include "crashlog.h"
#include "log.h"
#include "patches.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    // The DRM launcher (FeedingFrenzyTwo.exe) starts the real game as a child process;
    // only that one contains the game code we patch.
    if (!patches_is_game()) return TRUE;
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    log_open();
    log_printf("FF2 Remaster loaded into %s", path);
    crashlog_install();
    patches_apply();
    return TRUE;
}
