// Antichamber ships an Epic-built PhysX 2.8.4.4. When NVIDIA's PhysX System
// Software is installed, the loader prefers the system-wide PhysXCore.dll
// (2.8.4.5+), whose interface does not match and crashes the game while it
// creates the physics scene. Redirect that load to the game's own copy.
// The redirect is process-local: nothing on the system is modified.
#include <windows.h>
#include <cwchar>
#include <cstring>
#include <MinHook.h>
#include "log.h"

static wchar_t g_localCore[MAX_PATH];
static bool g_haveLocal = false;

typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI* LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI* LoadLibraryA_t)(LPCSTR);
static LoadLibraryExW_t oLoadLibraryExW;
static LoadLibraryExA_t oLoadLibraryExA;
static LoadLibraryW_t oLoadLibraryW;
static LoadLibraryA_t oLoadLibraryA;

static bool ShouldRedirectW(const wchar_t* name) {
    if (!g_haveLocal || !name) return false;
    const wchar_t* base = wcsrchr(name, L'\\');
    const wchar_t* base2 = wcsrchr(name, L'/');
    if (base2 && (!base || base2 > base)) base = base2;
    if (!base) return false; // bare name: normal search order already finds the local copy
    if (_wcsicmp(base + 1, L"PhysXCore.dll") != 0) return false;
    return _wcsicmp(name, g_localCore) != 0;
}

static bool ShouldRedirectA(const char* name) {
    if (!name) return false;
    wchar_t w[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH)) return false;
    return ShouldRedirectW(w);
}

static HMODULE WINAPI hkLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    if (ShouldRedirectW(name)) {
        Log("PhysX fix: redirecting '%ls' -> local PhysXCore.dll", name);
        return oLoadLibraryExW(g_localCore, nullptr, 0);
    }
    return oLoadLibraryExW(name, file, flags);
}
static HMODULE WINAPI hkLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
    if (ShouldRedirectA(name)) {
        Log("PhysX fix: redirecting '%s' -> local PhysXCore.dll", name);
        return oLoadLibraryExW(g_localCore, nullptr, 0);
    }
    return oLoadLibraryExA(name, file, flags);
}
static HMODULE WINAPI hkLoadLibraryW(LPCWSTR name) {
    if (ShouldRedirectW(name)) {
        Log("PhysX fix: redirecting '%ls' -> local PhysXCore.dll", name);
        return oLoadLibraryExW(g_localCore, nullptr, 0);
    }
    return oLoadLibraryW(name);
}
static HMODULE WINAPI hkLoadLibraryA(LPCSTR name) {
    if (ShouldRedirectA(name)) {
        Log("PhysX fix: redirecting '%s' -> local PhysXCore.dll", name);
        return oLoadLibraryExW(g_localCore, nullptr, 0);
    }
    return oLoadLibraryA(name);
}

void InstallPhysXFix() {
    GetModuleFileNameW(nullptr, g_localCore, MAX_PATH);
    wchar_t* slash = wcsrchr(g_localCore, L'\\');
    if (!slash) return;
    wcscpy(slash + 1, L"PhysXCore.dll");
    g_haveLocal = GetFileAttributesW(g_localCore) != INVALID_FILE_ATTRIBUTES;
    if (!g_haveLocal) return;
    MH_Initialize();
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    MH_CreateHook((void*)GetProcAddress(k32, "LoadLibraryExW"), (void*)hkLoadLibraryExW, (void**)&oLoadLibraryExW);
    MH_CreateHook((void*)GetProcAddress(k32, "LoadLibraryExA"), (void*)hkLoadLibraryExA, (void**)&oLoadLibraryExA);
    MH_CreateHook((void*)GetProcAddress(k32, "LoadLibraryW"), (void*)hkLoadLibraryW, (void**)&oLoadLibraryW);
    MH_CreateHook((void*)GetProcAddress(k32, "LoadLibraryA"), (void*)hkLoadLibraryA, (void**)&oLoadLibraryA);
    MH_EnableHook(MH_ALL_HOOKS);
}
