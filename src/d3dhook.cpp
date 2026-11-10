// Direct3D 9 side of the mod.
//
// In VR mode the game is given a Direct3D 9Ex device so the per-eye images can
// live in shared textures that Direct3D 11 (and therefore the SteamVR
// compositor) can read without any CPU copies. D3D9Ex has no managed pool, so
// managed resources are created as dynamic default-pool resources instead.
#include "d3dhook.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "vr.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <MinHook.h>

namespace d3dhook {

template <typename Fn> static Fn PatchVtbl(void* obj, int index, Fn hook) {
    void** vtbl = *(void***)obj;
    Fn orig = (Fn)vtbl[index];
    if (orig == hook) return nullptr;
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtbl[index] = (void*)hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return orig;
}

typedef HRESULT(WINAPI* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(WINAPI* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(WINAPI* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(WINAPI* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(WINAPI* CreateVolumeTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT(WINAPI* CreateCubeTexture_t)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT(WINAPI* CreateVertexBuffer_t)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT(WINAPI* CreateIndexBuffer_t)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);

static CreateDevice_t oCreateDevice;
static Present_t oPresent;
static Reset_t oReset;
static CreateTexture_t oCreateTexture;
static CreateVolumeTexture_t oCreateVolumeTexture;
static CreateCubeTexture_t oCreateCubeTexture;
static CreateVertexBuffer_t oCreateVertexBuffer;
static CreateIndexBuffer_t oCreateIndexBuffer;

static bool g_vrMode = false;
static bool g_ex = false;
static IDirect3DDevice9* g_dev = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;

// Eye targets (render thread only, except the ready flag).
static IDirect3DTexture9* g_eyeTex[2];
static IDirect3DSurface9* g_eyeSurf[2];
static ID3D11Texture2D* g_eyeTex11[2];
static IDirect3DQuery9* g_query;
static ID3D11Device* g_d3d11;
static ID3D11DeviceContext* g_ctx11;
static volatile bool g_stereoReady = false;
static bool g_targetsFailed = false;
static unsigned g_presents = 0;
static volatile unsigned g_stereoFrames = 0;

struct EyeTag {
    int eye;
    Mat34 pose;
    unsigned ordinal;
};
// Every presenting FViewport::Draw and every Present is counted, so a Present can be matched
// to the exact draw (and eye) it belongs to even if untagged frames are still in flight.
static volatile LONG g_drawOrdinal = 0;
static LONG g_presentOrdinal = 0;
static std::deque<EyeTag> g_tags;
static CRITICAL_SECTION g_tagCs;
static bool g_tagCsInit = false;

bool VrMode() { return g_vrMode; }
bool StereoReady() { return g_stereoReady; }
unsigned StereoFrames() { return g_stereoFrames; }
static volatile unsigned g_bbW = 0, g_bbH = 0;
void BackBufferSize(unsigned& w, unsigned& h) {
    w = g_bbW;
    h = g_bbH;
}

unsigned NoteDraw() { return (unsigned)InterlockedIncrement(&g_drawOrdinal); }

void PushEye(int eye, const Mat34& hmdPose, unsigned drawOrdinal) {
    EnterCriticalSection(&g_tagCs);
    g_tags.push_back({eye, hmdPose, drawOrdinal});
    LeaveCriticalSection(&g_tagCs);
}

static bool PopEye(EyeTag& out) {
    bool ok = false;
    // A Present that no draw accounts for (e.g. issued by the engine outside FViewport::Draw).
    if ((unsigned)g_presentOrdinal >= (unsigned)g_drawOrdinal) {
        g_presentOrdinal = g_drawOrdinal;
        return false;
    }
    unsigned ord = (unsigned)++g_presentOrdinal;
    EnterCriticalSection(&g_tagCs);
    while (!g_tags.empty() && g_tags.front().ordinal < ord) g_tags.pop_front();
    if (!g_tags.empty() && g_tags.front().ordinal == ord) {
        out = g_tags.front();
        g_tags.pop_front();
        ok = true;
    }
    LeaveCriticalSection(&g_tagCs);
    return ok;
}

static void LogPP(const char* what, D3DPRESENT_PARAMETERS* pp) {
    Log("%s: %ux%u fmt=%d windowed=%d swap=%d interval=%X hz=%u", what, pp->BackBufferWidth, pp->BackBufferHeight,
        pp->BackBufferFormat, pp->Windowed, pp->SwapEffect, pp->PresentationInterval, pp->FullScreen_RefreshRateInHz);
}

// --- Desktop size spoof ------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
// The engine clamps windowed resolutions to the desktop size. While the mod
// switches the game to the headset resolution, report a huge desktop so the
// back buffer can match what the headset needs.
static volatile DWORD g_spoofUntil = 0;
static int g_spoofHits[8];
static bool Spoofing() { return g_spoofUntil && GetTickCount() < g_spoofUntil; }
static bool SpoofHit(int which) {
    if (!Spoofing()) return false;
    g_spoofHits[which]++;
    return true;
}

typedef int(WINAPI* GetSystemMetrics_t)(int);
typedef BOOL(WINAPI* SystemParametersInfoW_t)(UINT, UINT, PVOID, UINT);
typedef BOOL(WINAPI* GetMonitorInfoW_t)(HMONITOR, LPMONITORINFO);
static GetSystemMetrics_t oGetSystemMetrics;
static SystemParametersInfoW_t oSystemParametersInfoW;
static GetMonitorInfoW_t oGetMonitorInfoW;
static const int kBigDesktop = 16384;

static int WINAPI hkGetSystemMetrics(int index) {
    if (SpoofHit(0)) {
        switch (index) {
        case SM_CXSCREEN: case SM_CYSCREEN: case SM_CXFULLSCREEN: case SM_CYFULLSCREEN: case SM_CXMAXTRACK:
        case SM_CYMAXTRACK: case SM_CXMAXIMIZED: case SM_CYMAXIMIZED: case SM_CXVIRTUALSCREEN: case SM_CYVIRTUALSCREEN:
            return kBigDesktop;
        }
    }
    return oGetSystemMetrics(index);
}
static BOOL WINAPI hkSystemParametersInfoW(UINT action, UINT param, PVOID data, UINT winIni) {
    BOOL r = oSystemParametersInfoW(action, param, data, winIni);
    if (r && action == SPI_GETWORKAREA && data && SpoofHit(1)) {
        RECT* rc = (RECT*)data;
        rc->left = rc->top = 0;
        rc->right = rc->bottom = kBigDesktop;
    }
    return r;
}
static BOOL WINAPI hkGetMonitorInfoW(HMONITOR mon, LPMONITORINFO info) {
    BOOL r = oGetMonitorInfoW(mon, info);
    if (r && info && SpoofHit(2)) {
        info->rcMonitor.left = info->rcMonitor.top = info->rcWork.left = info->rcWork.top = 0;
        info->rcMonitor.right = info->rcMonitor.bottom = info->rcWork.right = info->rcWork.bottom = kBigDesktop;
    }
    return r;
}

typedef BOOL(WINAPI* GetRect_t)(HWND, LPRECT);
typedef int(WINAPI* GetDeviceCaps_t)(HDC, int);
static GetRect_t oGetWindowRect, oGetClientRect;
static GetDeviceCaps_t oGetDeviceCaps;

static BOOL WINAPI hkGetWindowRect(HWND hwnd, LPRECT rc) {
    BOOL r = oGetWindowRect(hwnd, rc);
    if (r && rc && hwnd == GetDesktopWindow() && SpoofHit(3)) {
        rc->left = rc->top = 0;
        rc->right = rc->bottom = kBigDesktop;
    }
    return r;
}
static BOOL WINAPI hkGetClientRect(HWND hwnd, LPRECT rc) {
    BOOL r = oGetClientRect(hwnd, rc);
    if (r && rc && hwnd == GetDesktopWindow() && SpoofHit(4)) {
        rc->left = rc->top = 0;
        rc->right = rc->bottom = kBigDesktop;
    }
    return r;
}
static int WINAPI hkGetDeviceCaps(HDC dc, int index) {
    if (SpoofHit(5) && (index == HORZRES || index == VERTRES || index == DESKTOPHORZRES || index == DESKTOPVERTRES)) return kBigDesktop;
    return oGetDeviceCaps(dc, index);
}

typedef HRESULT(WINAPI* GetAdapterDisplayMode_t)(IDirect3D9*, UINT, D3DDISPLAYMODE*);
typedef HRESULT(WINAPI* GetDisplayMode_t)(IDirect3DDevice9*, UINT, D3DDISPLAYMODE*);
static GetAdapterDisplayMode_t oGetAdapterDisplayMode;
static GetDisplayMode_t oGetDisplayMode;

static HRESULT WINAPI hkGetAdapterDisplayMode(IDirect3D9* d3d, UINT adapter, D3DDISPLAYMODE* mode) {
    HRESULT hr = oGetAdapterDisplayMode(d3d, adapter, mode);
    if (SUCCEEDED(hr) && mode && SpoofHit(6)) mode->Width = mode->Height = kBigDesktop;
    return hr;
}
static HRESULT WINAPI hkGetDisplayMode(IDirect3DDevice9* dev, UINT swapChain, D3DDISPLAYMODE* mode) {
    HRESULT hr = oGetDisplayMode(dev, swapChain, mode);
    if (SUCCEEDED(hr) && mode && SpoofHit(7)) mode->Width = mode->Height = kBigDesktop;
    return hr;
}

void BeginDesktopSpoof() {
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        MH_Initialize();
        MH_CreateHook((void*)GetProcAddress(u32, "GetSystemMetrics"), (void*)hkGetSystemMetrics, (void**)&oGetSystemMetrics);
        MH_CreateHook((void*)GetProcAddress(u32, "SystemParametersInfoW"), (void*)hkSystemParametersInfoW, (void**)&oSystemParametersInfoW);
        MH_CreateHook((void*)GetProcAddress(u32, "GetMonitorInfoW"), (void*)hkGetMonitorInfoW, (void**)&oGetMonitorInfoW);
        MH_CreateHook((void*)GetProcAddress(u32, "GetWindowRect"), (void*)hkGetWindowRect, (void**)&oGetWindowRect);
        MH_CreateHook((void*)GetProcAddress(u32, "GetClientRect"), (void*)hkGetClientRect, (void**)&oGetClientRect);
        MH_CreateHook((void*)GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "GetDeviceCaps"), (void*)hkGetDeviceCaps, (void**)&oGetDeviceCaps);
        MH_EnableHook(MH_ALL_HOOKS);
    }
    g_spoofUntil = GetTickCount() + 5000;
    // Something (the Steam overlay or the engine itself) replaces the window procedure after
    // device creation, so make sure ours is on top before the window gets resized.
    if (g_hwnd && (WNDPROC)GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) != WndProc)
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
}

// --- Eye targets -----------------------------------------------------------

static void ReleaseTargets() {
    g_stereoReady = false;
    for (int e = 0; e < 2; e++) {
        if (g_eyeTex11[e]) { g_eyeTex11[e]->Release(); g_eyeTex11[e] = nullptr; }
        if (g_eyeSurf[e]) { g_eyeSurf[e]->Release(); g_eyeSurf[e] = nullptr; }
        if (g_eyeTex[e]) { g_eyeTex[e]->Release(); g_eyeTex[e] = nullptr; }
    }
    if (g_query) { g_query->Release(); g_query = nullptr; }
}

static bool EnsureD3D11() {
    if (g_d3d11 || vr::S.fake) return true;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        Log("d3d: CreateDXGIFactory1 failed");
        return false;
    }
    IDXGIAdapter* adapter = nullptr;
    factory->EnumAdapters(vr::S.adapterIndex, &adapter);
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3,
                                   D3D11_SDK_VERSION, &g_d3d11, &got, &g_ctx11);
    if (adapter) adapter->Release();
    factory->Release();
    if (FAILED(hr)) {
        Log("d3d: D3D11CreateDevice failed %08lX", hr);
        return false;
    }
    Log("d3d: D3D11 bridge device created (feature level %X)", got);
    return true;
}

static bool CreateTargets() {
    if (g_targetsFailed) return false;
    UINT w = vr::S.renderW, h = vr::S.renderH;
    if (!EnsureD3D11()) { g_targetsFailed = true; return false; }
    for (int e = 0; e < 2; e++) {
        HANDLE shared = nullptr;
        HRESULT hr = oCreateTexture(g_dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_eyeTex[e],
                                    g_ex ? &shared : nullptr);
        if (FAILED(hr)) {
            Log("d3d: eye texture %d (%ux%u) creation failed %08lX", e, w, h, hr);
            ReleaseTargets();
            g_targetsFailed = true;
            return false;
        }
        g_eyeTex[e]->GetSurfaceLevel(0, &g_eyeSurf[e]);
        if (!vr::S.fake) {
            hr = g_d3d11->OpenSharedResource(shared, __uuidof(ID3D11Texture2D), (void**)&g_eyeTex11[e]);
            if (FAILED(hr)) {
                Log("d3d: OpenSharedResource for eye %d failed %08lX (handle %p)", e, hr, shared);
                ReleaseTargets();
                g_targetsFailed = true;
                return false;
            }
        }
    }
    g_dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_query);
    Log("d3d: eye targets %ux%u created (shared=%d)", w, h, g_ex);
    g_stereoReady = true;
    return true;
}

static void FlushGpu() {
    if (!g_query) return;
    g_query->Issue(D3DISSUE_END);
    DWORD start = GetTickCount();
    while (g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        if (GetTickCount() - start > 200) break;
        SwitchToThread();
    }
}

static volatile LONG g_shotRequests = 0;
static int g_shotIndex = 0;
void RequestShot() { InterlockedIncrement(&g_shotRequests); }

static void DumpEye(int e, const char* prefix = "vr_eye") {
    HMODULE d3dx = GetModuleHandleA("d3dx9_43.dll");
    if (!d3dx) return;
    typedef HRESULT(WINAPI * Save_t)(LPCSTR, DWORD, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);
    Save_t save = (Save_t)GetProcAddress(d3dx, "D3DXSaveSurfaceToFileA");
    if (!save) return;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) slash[1] = 0;
    char name[64];
    snprintf(name, sizeof(name), "%s%d.jpg", prefix, e);
    strcat(path, name);
    HRESULT hr = save(path, 1 /*D3DXIFF_JPG*/, g_eyeSurf[e], nullptr, nullptr);
    Log("d3d: dumped eye %d -> %s (%08lX)", e, path, hr);
}

// --- Hooks -----------------------------------------------------------------

static HRESULT WINAPI hkPresent(IDirect3DDevice9* dev, const RECT* s, const RECT* d, HWND w, const RGNDATA* r) {
    g_presents++;
    if (!g_vrMode) return oPresent(dev, s, d, w, r);
    if (!g_stereoReady && !g_targetsFailed && g_presents > 30) CreateTargets();
    EyeTag tag;
    bool tagged = PopEye(tag);
    if (!g_stereoReady || !tagged) return oPresent(dev, s, d, w, r);

    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        HRESULT hr = dev->StretchRect(bb, nullptr, g_eyeSurf[tag.eye], nullptr, D3DTEXF_LINEAR);
        static int logged = 0;
        if (FAILED(hr) && logged++ < 5) Log("d3d: StretchRect eye %d failed %08lX", tag.eye, hr);
        bb->Release();
    }
    if (tag.eye == 0) return D3D_OK; // left eye captured; the right eye follows immediately

    FlushGpu();
    unsigned frame = ++g_stereoFrames;
    if (g_cfg.debugDumpFrame > 0 && frame == (unsigned)g_cfg.debugDumpFrame) {
        DumpEye(0);
        DumpEye(1);
    }
    if (g_shotRequests > 0) {
        // Diagnostic snapshot requested by the player (F9 or both sticks).
        InterlockedExchange(&g_shotRequests, 0);
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "vr_shot%02d_eye", ++g_shotIndex);
        DumpEye(0, prefix);
        DumpEye(1, prefix);
    }
    vr::Submit(g_eyeTex11[0], g_eyeTex11[1], tag.pose);
    HRESULT hr = oPresent(dev, s, d, w, r); // desktop mirror (right eye)
    vr::WaitGetPoses();
    if (frame == 1 || frame == 300 || (frame % 9000) == 0) {
        static DWORD lastTick = 0;
        static unsigned lastFrame = 0;
        DWORD now = GetTickCount();
        if (lastTick && now > lastTick)
            Log("d3d: stereo frame %u (%.1f fps)", frame, (frame - lastFrame) * 1000.0f / (now - lastTick));
        else
            Log("d3d: stereo frame %u", frame);
        lastTick = now;
        lastFrame = frame;
    }
    return hr;
}

static void ForceVrPresentParams(D3DPRESENT_PARAMETERS* pp) {
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
}

static HRESULT WINAPI hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (g_vrMode) {
        ReleaseTargets();
        EnterCriticalSection(&g_tagCs);
        g_tags.clear();
        LeaveCriticalSection(&g_tagCs);
        g_presentOrdinal = g_drawOrdinal; // the engine flushes rendering before a Reset
        ForceVrPresentParams(pp);
        g_presents = 0;
    }
    LogPP("Reset", pp);
    HRESULT hr = oReset(dev, pp);
    if (SUCCEEDED(hr)) {
        g_bbW = pp->BackBufferWidth;
        g_bbH = pp->BackBufferHeight;
    }
    if (FAILED(hr)) Log("Reset failed %08lX", hr);
    if (g_vrMode && SUCCEEDED(hr)) {
        Log("spoof hits: metrics %d workarea %d monitor %d deskWndRect %d deskClientRect %d devcaps %d adapterMode %d deviceMode %d", g_spoofHits[0],
            g_spoofHits[1], g_spoofHits[2], g_spoofHits[3], g_spoofHits[4], g_spoofHits[5], g_spoofHits[6], g_spoofHits[7]);
        g_spoofUntil = 0;
        // Keep the (possibly larger than the desktop) window anchored at the top-left corner.
        if (g_hwnd) SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    return hr;
}

static D3DPOOL FixPool(D3DPOOL pool, DWORD* usage, bool texture) {
    if (!g_ex || pool != D3DPOOL_MANAGED) return pool;
    if (texture && !(*usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP))) *usage |= D3DUSAGE_DYNAMIC;
    return D3DPOOL_DEFAULT;
}

static HRESULT WINAPI hkCreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                      IDirect3DTexture9** out, HANDLE* shared) {
    pool = FixPool(pool, &usage, true);
    return oCreateTexture(dev, w, h, levels, usage, fmt, pool, out, shared);
}
static HRESULT WINAPI hkCreateVolumeTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt,
                                            D3DPOOL pool, IDirect3DVolumeTexture9** out, HANDLE* shared) {
    pool = FixPool(pool, &usage, true);
    return oCreateVolumeTexture(dev, w, h, d, levels, usage, fmt, pool, out, shared);
}
static HRESULT WINAPI hkCreateCubeTexture(IDirect3DDevice9* dev, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                          IDirect3DCubeTexture9** out, HANDLE* shared) {
    pool = FixPool(pool, &usage, true);
    return oCreateCubeTexture(dev, edge, levels, usage, fmt, pool, out, shared);
}
static HRESULT WINAPI hkCreateVertexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                           IDirect3DVertexBuffer9** out, HANDLE* shared) {
    pool = FixPool(pool, &usage, false);
    return oCreateVertexBuffer(dev, len, usage, fvf, pool, out, shared);
}
static HRESULT WINAPI hkCreateIndexBuffer(IDirect3DDevice9* dev, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                          IDirect3DIndexBuffer9** out, HANDLE* shared) {
    pool = FixPool(pool, &usage, false);
    return oCreateIndexBuffer(dev, len, usage, fmt, pool, out, shared);
}

// Lets the game window exceed the desktop size so the back buffer can match the headset resolution.
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETMINMAXINFO) {
        LRESULT r = CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMaxTrackSize.x = 16384;
        mm->ptMaxTrackSize.y = 16384;
        mm->ptMaxSize.x = 16384;
        mm->ptMaxSize.y = 16384;
        return r;
    }
    if (msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wpos = (WINDOWPOS*)lp;
        static int logged = 0;
        if (!(wpos->flags & SWP_NOSIZE) && logged++ < 3) Log("wnd: WM_WINDOWPOSCHANGING %dx%d at %d,%d flags %X", wpos->cx, wpos->cy, wpos->x, wpos->y, wpos->flags);
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

static HRESULT WINAPI hkCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                     D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (g_vrMode) ForceVrPresentParams(pp);
    LogPP("CreateDevice", pp);
    HRESULT hr;
    if (g_ex) {
        hr = ((IDirect3D9Ex*)d3d)->CreateDeviceEx(adapter, type, focus, flags, pp, nullptr, (IDirect3DDevice9Ex**)out);
    } else {
        hr = oCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    }
    Log("CreateDevice%s -> %08lX dev=%p behavior=%lX", g_ex ? "Ex" : "", hr, out ? *out : nullptr, flags);
    if (SUCCEEDED(hr) && out && *out) {
        IDirect3DDevice9* dev = *out;
        g_dev = dev;
        if (auto o = PatchVtbl<Reset_t>(dev, 16, hkReset)) oReset = o;
        if (g_vrMode)
            if (auto o = PatchVtbl<GetDisplayMode_t>(dev, 8, hkGetDisplayMode)) oGetDisplayMode = o;
        if (auto o = PatchVtbl<Present_t>(dev, 17, hkPresent)) oPresent = o;
        if (auto o = PatchVtbl<CreateTexture_t>(dev, 23, hkCreateTexture)) oCreateTexture = o;
        if (auto o = PatchVtbl<CreateVolumeTexture_t>(dev, 24, hkCreateVolumeTexture)) oCreateVolumeTexture = o;
        if (auto o = PatchVtbl<CreateCubeTexture_t>(dev, 25, hkCreateCubeTexture)) oCreateCubeTexture = o;
        if (auto o = PatchVtbl<CreateVertexBuffer_t>(dev, 26, hkCreateVertexBuffer)) oCreateVertexBuffer = o;
        if (auto o = PatchVtbl<CreateIndexBuffer_t>(dev, 27, hkCreateIndexBuffer)) oCreateIndexBuffer = o;
        if (g_vrMode) {
            HWND hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
            if (hwnd && hwnd != g_hwnd) {
                g_hwnd = hwnd;
                g_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
            }
            game::InstallDrawHook();
        }
    }
    return hr;
}

IDirect3D9* CreateD3D(UINT sdk, HMODULE real) {
    static bool decided = false;
    if (!g_tagCsInit) {
        InitializeCriticalSection(&g_tagCs);
        g_tagCsInit = true;
    }
    if (!decided) {
        decided = true;
        if (g_cfg.enabled && vr::HmdPresent()) {
            if (vr::Init()) {
                g_vrMode = true;
            } else {
                Log("VR initialisation failed: the game will run in flat mode");
            }
        } else {
            Log("VR disabled or no headset detected: the game will run in flat mode");
        }
    }
    IDirect3D9* d3d = nullptr;
    if (g_vrMode) {
        typedef HRESULT(WINAPI * CreateEx_t)(UINT, IDirect3D9Ex**);
        CreateEx_t createEx = (CreateEx_t)GetProcAddress(real, "Direct3DCreate9Ex");
        IDirect3D9Ex* ex = nullptr;
        if (createEx && SUCCEEDED(createEx(sdk, &ex)) && ex) {
            d3d = ex;
            g_ex = true;
        } else {
            Log("Direct3DCreate9Ex unavailable");
        }
    }
    if (!d3d) {
        typedef IDirect3D9*(WINAPI * Create_t)(UINT);
        Create_t create = (Create_t)GetProcAddress(real, "Direct3DCreate9");
        if (!create) return nullptr;
        d3d = create(sdk);
        if (g_vrMode && !g_ex) {
            Log("No D3D9Ex: VR disabled");
            g_vrMode = false;
        }
    }
    if (d3d) {
        if (auto o = PatchVtbl<CreateDevice_t>(d3d, 16, hkCreateDevice)) oCreateDevice = o;
        if (g_vrMode)
            if (auto o = PatchVtbl<GetAdapterDisplayMode_t>(d3d, 8, hkGetAdapterDisplayMode)) oGetAdapterDisplayMode = o;
    }
    return d3d;
}

} // namespace d3dhook
