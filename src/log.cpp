#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

static CRITICAL_SECTION g_logCs;
static FILE* g_logFile = nullptr;
static bool g_logReady = false;

void LogInit() {
    if (g_logReady) return;
    InitializeCriticalSection(&g_logCs);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    wcscat(path, L"antichamber_vr.log");
    g_logFile = _wfopen(path, L"w");
    g_logReady = true;
}

void Log(const char* fmt, ...) {
    if (!g_logReady || !g_logFile) return;
    EnterCriticalSection(&g_logCs);
    fprintf(g_logFile, "[%8lu][t%5lu] ", GetTickCount(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    fflush(g_logFile);
    LeaveCriticalSection(&g_logCs);
}

void LogHex(const char* title, const void* p, size_t n) {
    if (!Readable(p, n)) {
        Log("%s @%p: <unreadable>", title, p);
        return;
    }
    Log("%s @%p:", title, p);
    const uint8_t* b = (const uint8_t*)p;
    char line[256];
    for (size_t i = 0; i < n; i += 16) {
        int len = snprintf(line, sizeof(line), "   +%03X:", (unsigned)i);
        for (size_t j = i; j < i + 16 && j < n; j += 4) {
            uint32_t v;
            memcpy(&v, b + j, 4);
            len += snprintf(line + len, sizeof(line) - len, " %08X", v);
        }
        Log("%s", line);
    }
}

bool Readable(const void* p, size_t n) {
    static thread_local uintptr_t cacheLo = 0, cacheHi = 0;
    uintptr_t a = (uintptr_t)p;
    if (a < 0x10000 || a + n < a) return false;
    if (a >= cacheLo && a + n <= cacheHi) return true;
    uintptr_t end = a + n;
    uintptr_t cur = a;
    uintptr_t lo = 0, hi = 0;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)cur, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        uintptr_t rlo = (uintptr_t)mbi.BaseAddress;
        uintptr_t rhi = rlo + mbi.RegionSize;
        if (!lo) lo = rlo;
        hi = rhi;
        cur = rhi;
    }
    cacheLo = lo;
    cacheHi = hi;
    return true;
}
