// d3d9.dll proxy for Antichamber (UDK / UE3, 32-bit, Direct3D 9).
// Loads the real system d3d9.dll, forwards the exports the game uses and
// installs the VR hooks once the device exists.
#include <windows.h>
#include <d3d9.h>
#include "log.h"
#include "d3dhook.h"
#include "config.h"

void InstallCrashLogger();
void InstallPhysXFix();

static HMODULE g_real = nullptr;

static FARPROC Real(const char* name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        wcscat(path, L"\\d3d9.dll");
        g_real = LoadLibraryW(path);
        Log("Antichamber VR: real d3d9 loaded at %p", g_real);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

extern "C" {

IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    typedef IDirect3D9*(WINAPI * Fn)(UINT);
    Fn fn = (Fn)Real("Direct3DCreate9");
    if (!fn) return nullptr;
    (void)fn;
    IDirect3D9* d3d = d3dhook::CreateD3D(sdk, g_real);
    Log("Direct3DCreate9(%u) -> %p", sdk, d3d);
    return d3d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    typedef HRESULT(WINAPI * Fn)(UINT, IDirect3D9Ex**);
    Fn fn = (Fn)Real("Direct3DCreate9Ex");
    return fn ? fn(sdk, out) : E_FAIL;
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name) {
    typedef int(WINAPI * Fn)(D3DCOLOR, LPCWSTR);
    static Fn fn = (Fn)Real("D3DPERF_BeginEvent");
    return fn ? fn(col, name) : 0;
}

int WINAPI D3DPERF_EndEvent(void) {
    typedef int(WINAPI * Fn)(void);
    static Fn fn = (Fn)Real("D3DPERF_EndEvent");
    return fn ? fn() : 0;
}

void WINAPI D3DPERF_SetOptions(DWORD opt) {
    typedef void(WINAPI * Fn)(DWORD);
    static Fn fn = (Fn)Real("D3DPERF_SetOptions");
    if (fn) fn(opt);
}

DWORD WINAPI D3DPERF_GetStatus(void) {
    typedef DWORD(WINAPI * Fn)(void);
    static Fn fn = (Fn)Real("D3DPERF_GetStatus");
    return fn ? fn() : 0;
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name) {
    typedef void(WINAPI * Fn)(D3DCOLOR, LPCWSTR);
    static Fn fn = (Fn)Real("D3DPERF_SetMarker");
    if (fn) fn(col, name);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name) {
    typedef void(WINAPI * Fn)(D3DCOLOR, LPCWSTR);
    static Fn fn = (Fn)Real("D3DPERF_SetRegion");
    if (fn) fn(col, name);
}

BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    typedef BOOL(WINAPI * Fn)(void);
    static Fn fn = (Fn)Real("D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LogInit();
        LoadConfig();
        InstallCrashLogger();
        if (g_cfg.fixPhysX) InstallPhysXFix();
    }
    return TRUE;
}
