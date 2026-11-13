// CpuCores=a-b,c: lets the game run on the given CPU cores.
//
// On the Steam Frame (Snapdragon 8 Gen 3: cores 0-1 small, 2-6 medium, 7 fast) SteamOS starts
// games restricted to cores 0-4, which leaves the emulated game on the two slowest cores and
// without the fastest one. Antichamber is limited by its render thread, so letting it use cores
// 2-7 matters. Under Proton, SetProcessAffinityMask becomes a Linux sched_setaffinity; whether it
// takes effect is visible in the log (the Linux "Cpus_allowed_list" lines, read through Wine's Z:).
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "config.h"
#include "log.h"

static DWORD_PTR ParseCores(const char* s) {
    DWORD_PTR mask = 0;
    while (*s) {
        while (*s == ' ' || *s == ',') s++;
        if (*s < '0' || *s > '9') break;
        int a = 0, b;
        while (*s >= '0' && *s <= '9') a = a * 10 + (*s++ - '0');
        b = a;
        if (*s == '-') {
            s++;
            b = 0;
            while (*s >= '0' && *s <= '9') b = b * 10 + (*s++ - '0');
        }
        for (int i = a; i <= b && i < (int)(sizeof(DWORD_PTR) * 8); i++) mask |= (DWORD_PTR)1 << i;
    }
    return mask;
}

// Logs the Linux view of the CPUs this process/thread may use (only exists under Wine/Proton).
void LogLinuxCpus(const char* who, bool thread) {
    FILE* f = fopen(thread ? "Z:\proc\thread-self\status" : "Z:\proc\self\status", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "Cpus_allowed_list:", 18)) {
            line[strcspn(line, "\r\n")] = 0;
            Log("cpu: %s linux %s", who, line);
        }
    }
    fclose(f);
}

void ApplyCpuCores() {
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    Log("cpu: process affinity %lX of system %lX", (unsigned long)proc, (unsigned long)sys);
    LogLinuxCpus("process (before)", false);
    if (!g_cfg.cpuCores[0]) return;
    DWORD_PTR want = ParseCores(g_cfg.cpuCores) & sys;
    if (!want) {
        Log("cpu: CpuCores=%s matches no core, left alone", g_cfg.cpuCores);
        return;
    }
    BOOL ok = SetProcessAffinityMask(GetCurrentProcess(), want);
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    Log("cpu: CpuCores=%s -> mask %lX: %s, now %lX", g_cfg.cpuCores, (unsigned long)want, ok ? "ok" : "failed", (unsigned long)proc);
    LogLinuxCpus("process (after)", false);
}

// RenderThreadCore=N: the engine's render thread (the one that limits the frame rate on the Steam
// Frame) runs only on core N, and the game thread stays off it. On the Frame core 7 is the fast
// one. Called once from each thread.
void PinRenderThread() {
    int core = g_cfg.renderThreadCore;
    if (core < 0 || core > 31) return;
    DWORD_PTR m = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << core);
    Log("cpu: render thread pinned to core %d: %s (previous mask %lX)", core, m ? "ok" : "failed", (unsigned long)m);
}
void KeepGameThreadOffRenderCore() {
    int core = g_cfg.renderThreadCore;
    if (core < 0 || core > 31) return;
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    DWORD_PTR m = proc & ~((DWORD_PTR)1 << core);
    if (!m) return;
    DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), m);
    Log("cpu: game thread mask %lX (off core %d): %s", (unsigned long)m, core, prev ? "ok" : "failed");
}
