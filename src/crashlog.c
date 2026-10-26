// Logs CPU exceptions inside the game (address, registers, likely return addresses)
// to ff2wide.log, so a crash report can be matched against the decompiled code.
#include <windows.h>
#include "log.h"
#include "crashlog.h"

#define GAME_CODE_START 0x401000
#define GAME_CODE_END   0x545189
#define MAX_REPORTS     8

static LONG reports;

static LONG CALLBACK on_exception(PEXCEPTION_POINTERS info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    // C++ throws, debugger breakpoints and other non-fatal codes are part of normal running.
    if (code < 0xC0000000 || code == 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&reports) > MAX_REPORTS) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT *c = info->ContextRecord;
    log_printf("EXCEPTION %08lx at %08lx", code, c->Eip);
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
        log_printf("  %s address %08lx", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                   (DWORD)info->ExceptionRecord->ExceptionInformation[1]);
    log_printf("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx", c->Eax, c->Ebx, c->Ecx, c->Edx);
    log_printf("  esi=%08lx edi=%08lx ebp=%08lx esp=%08lx", c->Esi, c->Edi, c->Ebp, c->Esp);

    // Return addresses on the stack that point into game code give a rough call stack.
    const DWORD *sp = (const DWORD *)c->Esp;
    int found = 0;
    for (int i = 0; i < 512 && found < 24; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        DWORD v = sp[i];
        if (v >= GAME_CODE_START && v < GAME_CODE_END) {
            log_printf("  stack[%03x] %08lx", i * 4, v);
            found++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// The game's runtime library checks container accesses (vector bounds, iterator ranges)
// and on a failure calls _invalid_parameter (0x4a9ec2) through 0x4a9ee6, which ends the
// game on the spot without an exception: nothing above sees it. So that call is logged,
// with where it came from, before it goes on as before.
#define FN_INVALID_PARAMETER 0x4a9ec2

void __cdecl crashlog_runtime_check_failed(void)
{
    DWORD from = (DWORD)__builtin_return_address(0);
    log_printf("RUNTIME CHECK FAILED (the game will close), called from %08lx", from);
    const DWORD *sp = (const DWORD *)__builtin_frame_address(0);
    int found = 0;
    for (int i = 0; i < 1024 && found < 24; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        DWORD v = sp[i];
        if (v >= GAME_CODE_START && v < GAME_CODE_END) {
            log_printf("  stack[%03x] %08lx", i * 4, v);
            found++;
        }
    }
    typedef void (__cdecl *InvalidParameter_t)(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, UINT_PTR);
    ((InvalidParameter_t)FN_INVALID_PARAMETER)(NULL, NULL, NULL, 0, 0);
}

void crashlog_install(void)
{
    AddVectoredExceptionHandler(1, on_exception);
}
