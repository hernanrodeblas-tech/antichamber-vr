// First-chance exception logger: records where the game faults so crashes can
// be diagnosed without a debugger.
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include "log.h"

static void Describe(uintptr_t addr, char* out, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    out[0] = 0;
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !mbi.AllocationBase) return;
    char path[MAX_PATH];
    if (mbi.Type == MEM_IMAGE && GetModuleFileNameA((HMODULE)mbi.AllocationBase, path, MAX_PATH)) {
        const char* base = strrchr(path, '\\');
        snprintf(out, n, "%s+0x%X", base ? base + 1 : path, (unsigned)(addr - (uintptr_t)mbi.AllocationBase));
    }
}

static volatile LONG g_count = 0;

static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_count) > 6) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    char d[300];
    Describe(c->Eip, d, sizeof(d));
    Log("EXCEPTION %08lX at %08lX [%s] info0=%08lX info1=%08lX", code, c->Eip, d,
        (unsigned long)ep->ExceptionRecord->ExceptionInformation[0],
        (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
    Log("  eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX", c->Eax, c->Ebx, c->Ecx,
        c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    if (code == EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    if (g_count == 1) {
        HMODULE mods[512];
        DWORD needed = 0;
        if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
            int n = needed / sizeof(HMODULE);
            for (int i = 0; i < n && i < 512; i++) {
                char path[MAX_PATH];
                if (GetModuleFileNameA(mods[i], path, MAX_PATH)) Log("  module %p %s", mods[i], path);
            }
        }
    }
    // Stack scan: print every word that points into an image.
    uintptr_t* sp = (uintptr_t*)c->Esp;
    int printed = 0;
    for (int i = 0; i < 600 && printed < 48; i++) {
        if (!Readable(sp + i, sizeof(uintptr_t))) break;
        Describe(sp[i], d, sizeof(d));
        if (d[0]) {
            Log("  [esp+%03X] %08X %s", i * 4, (unsigned)sp[i], d);
            printed++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLogger() { AddVectoredExceptionHandler(1, Veh); }
