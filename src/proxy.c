// version.dll proxy: forwards the three functions the game imports to the real
// implementation, and gives us a foothold inside popcapgame1.exe.
#include <windows.h>
#include "log.h"

// On Windows 8+ kernelbase.dll (always loaded) implements these, so prefer it over
// loading the system version.dll: inside the DRM-launched game that load fails
// with ERROR_MOD_NOT_FOUND.
static void *real(const char *name)
{
    static HMODULE kernelbase, version;
    void *fn = NULL;
    if (!kernelbase) kernelbase = GetModuleHandleA("kernelbase.dll");
    if (kernelbase) fn = (void *)GetProcAddress(kernelbase, name);
    if (fn) return fn;
    if (!version) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        lstrcatA(path, "\\version.dll");
        version = LoadLibraryA(path);
        if (!version) log_printf("could not load %s (error %lu)", path, GetLastError());
    }
    if (version) fn = (void *)GetProcAddress(version, name);
    if (!fn) log_printf("no implementation found for %s", name);
    return fn;
}

typedef DWORD (WINAPI *GetFileVersionInfoSizeA_t)(LPCSTR, LPDWORD);
typedef BOOL (WINAPI *GetFileVersionInfoA_t)(LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL (WINAPI *VerQueryValueA_t)(LPCVOID, LPCSTR, LPVOID *, PUINT);

__declspec(dllexport) DWORD WINAPI proxy_GetFileVersionInfoSizeA(LPCSTR f, LPDWORD h)
{
    GetFileVersionInfoSizeA_t fn = (GetFileVersionInfoSizeA_t)real("GetFileVersionInfoSizeA");
    if (!fn) {
        SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
        return 0;
    }
    return fn(f, h);
}

__declspec(dllexport) BOOL WINAPI proxy_GetFileVersionInfoA(LPCSTR f, DWORD h, DWORD len, LPVOID data)
{
    GetFileVersionInfoA_t fn = (GetFileVersionInfoA_t)real("GetFileVersionInfoA");
    return fn ? fn(f, h, len, data) : FALSE;
}

__declspec(dllexport) BOOL WINAPI proxy_VerQueryValueA(LPCVOID block, LPCSTR sub, LPVOID *buf, PUINT len)
{
    VerQueryValueA_t fn = (VerQueryValueA_t)real("VerQueryValueA");
    return fn ? fn(block, sub, buf, len) : FALSE;
}
