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

// Eye targets (render thread only, except the ready flag). One set holds a stereo frame: both
// eye images on the D3D9 side and the matching D3D11 textures handed to SteamVR. With
// PipelineFrames=1 there are three sets in a ring: a finished frame is submitted one frame later,
// so the CPU never sits idle waiting for the GPU to finish the frame it just queued.
struct EyeSet {
    IDirect3DTexture9* tex[2];
    IDirect3DSurface9* surf[2];
    ID3D11Texture2D* tex11[2];
    IDirect3DSurface9* sys[2]; // bridge 2: system-memory copies of the eye images
    IDirect3DQuery9* query;    // signals when the GPU has finished writing this set
    Mat34 pose;                // head pose the set was rendered with
    bool pending;              // finished on the CPU side, not submitted yet
};
static EyeSet g_sets[3];
static int g_numSets = 1, g_cur = 0;
// How the D3D9 eye images reach the D3D11 textures handed to SteamVR:
//   0 = D3D9Ex shared texture opened in D3D11 (Windows)
//   1 = D3D11 shared texture opened in D3D9Ex (some D3D9-on-Vulkan layers only share this way)
//   2 = copy through system memory (slow; last resort)
static int g_bridge = -1; // set from D3DBridge on first use
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

template <typename T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

static void ReleaseTargets() {
    g_stereoReady = false;
    for (EyeSet& s : g_sets) {
        for (int e = 0; e < 2; e++) {
            SafeRelease(s.tex11[e]);
            SafeRelease(s.sys[e]);
            SafeRelease(s.surf[e]);
            SafeRelease(s.tex[e]);
        }
        SafeRelease(s.query);
        s.pending = false;
    }
    g_cur = 0;
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

// Bridge 0: the eye textures are created by D3D9Ex as shared and opened by D3D11.
static bool CreateTargets9to11(EyeSet& S, UINT w, UINT h) {
    for (int e = 0; e < 2; e++) {
        HANDLE shared = nullptr;
        HRESULT hr = oCreateTexture(g_dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &S.tex[e],
                                    g_ex ? &shared : nullptr);
        if (FAILED(hr)) {
            Log("d3d: eye texture %d (%ux%u) creation failed %08lX", e, w, h, hr);
            return false;
        }
        S.tex[e]->GetSurfaceLevel(0, &S.surf[e]);
        if (vr::S.fake) continue;
        hr = g_d3d11->OpenSharedResource(shared, __uuidof(ID3D11Texture2D), (void**)&S.tex11[e]);
        if (FAILED(hr)) {
            Log("d3d: OpenSharedResource for eye %d failed %08lX (handle %p)", e, hr, shared);
            return false;
        }
    }
    return true;
}

// Bridge 1: the eye textures are created by D3D11 as shared and opened by D3D9Ex.
static bool CreateTargets11to9(EyeSet& S, UINT w, UINT h) {
    for (int e = 0; e < 2; e++) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        HRESULT hr = g_d3d11->CreateTexture2D(&td, nullptr, &S.tex11[e]);
        if (FAILED(hr)) {
            Log("d3d: [11->9] D3D11 eye texture %d failed %08lX", e, hr);
            return false;
        }
        IDXGIResource* res = nullptr;
        HANDLE shared = nullptr;
        if (SUCCEEDED(S.tex11[e]->QueryInterface(__uuidof(IDXGIResource), (void**)&res)) && res) {
            hr = res->GetSharedHandle(&shared);
            res->Release();
        }
        if (!shared) {
            Log("d3d: [11->9] no shared handle for eye %d (%08lX)", e, hr);
            return false;
        }
        hr = oCreateTexture(g_dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &S.tex[e], &shared);
        if (FAILED(hr)) {
            Log("d3d: [11->9] D3D9 open of eye %d failed %08lX (handle %p)", e, hr, shared);
            return false;
        }
        S.tex[e]->GetSurfaceLevel(0, &S.surf[e]);
    }
    return true;
}

// Bridge 2: plain D3D9 render targets, copied each frame through system memory into D3D11 textures.
static bool CreateTargetsCpu(EyeSet& S, UINT w, UINT h) {
    for (int e = 0; e < 2; e++) {
        HRESULT hr = oCreateTexture(g_dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &S.tex[e], nullptr);
        if (FAILED(hr)) {
            Log("d3d: [cpu] eye texture %d failed %08lX", e, hr);
            return false;
        }
        S.tex[e]->GetSurfaceLevel(0, &S.surf[e]);
        hr = g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &S.sys[e], nullptr);
        if (FAILED(hr)) {
            Log("d3d: [cpu] system-memory surface %d failed %08lX", e, hr);
            return false;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = g_d3d11->CreateTexture2D(&td, nullptr, &S.tex11[e]);
        if (FAILED(hr)) {
            Log("d3d: [cpu] D3D11 eye texture %d failed %08lX", e, hr);
            return false;
        }
    }
    return true;
}

// Bridge 2: copy one eye from its D3D9 render target into its D3D11 texture.
static void CopyEyeCpu(EyeSet& S, int e) {
    if (FAILED(g_dev->GetRenderTargetData(S.surf[e], S.sys[e]))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(S.sys[e]->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    g_ctx11->UpdateSubresource(S.tex11[e], 0, nullptr, lr.pBits, lr.Pitch, 0);
    S.sys[e]->UnlockRect();
}

static bool CreateTargets() {
    if (g_targetsFailed) return false;
    UINT w = vr::S.renderW, h = vr::S.renderH;
    if (!EnsureD3D11()) { g_targetsFailed = true; return false; }
    static const char* names[] = {"shared D3D9->D3D11", "shared D3D11->D3D9", "system-memory copy"};
    if (g_bridge < 0) g_bridge = vr::S.fake ? 0 : g_cfg.d3dBridge < 0 ? 0 : g_cfg.d3dBridge > 2 ? 2 : g_cfg.d3dBridge;
    // Remember the bridge that worked (device resets recreate the targets).
    for (int b = g_bridge; b < 3; b++) {
        if (vr::S.fake && b > 0) break;
        // The system-memory copy waits for the GPU anyway, so pipelining would gain nothing there.
        int sets = (g_cfg.pipelineFrames && b != 2 && !vr::S.fake) ? 3 : 1;
        bool ok = true;
        for (int i = 0; i < sets && ok; i++) {
            EyeSet& S = g_sets[i];
            ok = b == 0 ? CreateTargets9to11(S, w, h) : b == 1 ? CreateTargets11to9(S, w, h) : CreateTargetsCpu(S, w, h);
            if (ok) g_dev->CreateQuery(D3DQUERYTYPE_EVENT, &S.query);
        }
        if (ok) {
            g_bridge = b;
            g_numSets = sets;
            g_cur = 0;
            Log("d3d: eye targets %ux%u created (%s%s)", w, h, names[b], sets > 1 ? ", pipelined" : "");
            g_stereoReady = true;
            return true;
        }
        ReleaseTargets();
        if (b < 2) Log("d3d: %s unavailable, trying %s", names[b], names[b + 1]);
    }
    g_targetsFailed = true;
    return false;
}

// Marks the end of a set's GPU work and makes sure it is sent to the GPU (without waiting).
static void IssueEnd(EyeSet& S) {
    if (!S.query) return;
    S.query->Issue(D3DISSUE_END);
    S.query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
}

// Waits until the GPU has finished writing a set (issued with IssueEnd).
static void WaitSet(EyeSet& S) {
    if (!S.query) return;
    DWORD start = GetTickCount();
    while (S.query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        if (GetTickCount() - start > 200) break;
        SwitchToThread();
    }
}

static volatile LONG g_shotRequests = 0;
static int g_shotIndex = 0;
void RequestShot() { InterlockedIncrement(&g_shotRequests); }

static void DumpEye(EyeSet& S, int e, const char* prefix = "vr_eye") {
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
    HRESULT hr = save(path, 1 /*D3DXIFF_JPG*/, S.surf[e], nullptr, nullptr);
    Log("d3d: dumped eye %d -> %s (%08lX)", e, path, hr);
}

// --- Hooks -----------------------------------------------------------------

static volatile DWORD g_renderThread = 0;
DWORD RenderThreadId() { return g_renderThread; }

static HRESULT WINAPI hkPresent(IDirect3DDevice9* dev, const RECT* s, const RECT* d, HWND w, const RGNDATA* r) {
    g_renderThread = GetCurrentThreadId();
    g_presents++;
    if (!g_vrMode) return oPresent(dev, s, d, w, r);
    if (!g_stereoReady && !g_targetsFailed && g_presents > 30) CreateTargets();
    EyeTag tag;
    bool tagged = PopEye(tag);
    if (!g_stereoReady || !tagged) return oPresent(dev, s, d, w, r);

    EyeSet& C = g_sets[g_cur];
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        HRESULT hr = dev->StretchRect(bb, nullptr, C.surf[tag.eye], nullptr, D3DTEXF_LINEAR);
        static int logged = 0;
        if (FAILED(hr) && logged++ < 5) Log("d3d: StretchRect eye %d failed %08lX", tag.eye, hr);
        bb->Release();
    }
    if (g_bridge == 2) CopyEyeCpu(C, tag.eye); // waits for the GPU, so no flush needed for this eye
    static LARGE_INTEGER tEye0; // when the left eye was finished (splits game+render below)
    if (tag.eye == 0) {
        QueryPerformanceCounter(&tEye0);
        return D3D_OK; // left eye captured; the right eye follows immediately
    }

    // Frame timing, to tell what limits the frame rate: the GPU (flush), the compositor (wait)
    // or the game itself (everything else, split into game tick + left eye, and right eye).
    static LARGE_INTEGER qpf, tPrev;
    static double sumFrame, sumFlush, sumWait, sumMax, sumEye1;
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t0, t1, t2, t3;
    QueryPerformanceCounter(&t0);
    C.pose = tag.pose;
    EyeSet* toSubmit = nullptr;
    if (g_bridge == 2) {
        toSubmit = &C; // already copied (synchronously) into the D3D11 textures
    } else if (g_numSets == 1) {
        IssueEnd(C); // wait for this frame's GPU work, then hand it over
        WaitSet(C);
        toSubmit = &C;
    } else {
        // Pipelined: queue this frame and submit the previous one, which the GPU has almost
        // certainly finished while the game was building this frame.
        IssueEnd(C);
        C.pending = true;
        EyeSet& P = g_sets[(g_cur + g_numSets - 1) % g_numSets];
        if (P.pending) {
            WaitSet(P);
            toSubmit = &P;
        }
        g_cur = (g_cur + 1) % g_numSets;
    }
    QueryPerformanceCounter(&t1);
    unsigned frame = ++g_stereoFrames;
    if (g_cfg.debugDumpFrame > 0 && frame == (unsigned)g_cfg.debugDumpFrame) {
        DumpEye(C, 0);
        DumpEye(C, 1);
    }
    if (g_shotRequests > 0) {
        // Diagnostic snapshot requested by the player (F9 or both sticks).
        InterlockedExchange(&g_shotRequests, 0);
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "vr_shot%02d_eye", ++g_shotIndex);
        DumpEye(C, 0, prefix);
        DumpEye(C, 1, prefix);
    }
    if (toSubmit) {
        vr::Submit(toSubmit->tex11[0], toSubmit->tex11[1], toSubmit->pose);
        toSubmit->pending = false;
    }
    HRESULT hr = oPresent(dev, s, d, w, r); // desktop mirror (right eye)
    QueryPerformanceCounter(&t2);
    vr::WaitGetPoses();
    QueryPerformanceCounter(&t3);
    {
        auto ms = [](LONGLONG a, LONGLONG b) { return (b - a) * 1000.0 / qpf.QuadPart; };
        if (tPrev.QuadPart) {
            double f = ms(tPrev.QuadPart, t3.QuadPart);
            sumFrame += f;
            if (f > sumMax) sumMax = f;
        }
        sumFlush += ms(t0.QuadPart, t1.QuadPart);
        sumWait += ms(t2.QuadPart, t3.QuadPart);
        if (tEye0.QuadPart && tEye0.QuadPart < t0.QuadPart) sumEye1 += ms(tEye0.QuadPart, t0.QuadPart);
        tPrev = t3;
        const unsigned n = 360;
        if (frame % n == 0) {
            double avg = sumFrame / n, game = avg - sumFlush / n - sumWait / n;
            Log("perf: %.1f fps | frame %.1f ms (worst %.1f) = game+render %.1f (tick+left eye %.1f, right eye %.1f) + gpu flush %.1f + "
                "compositor wait %.1f | %ux%u per eye, %.0f Hz",
                avg > 0 ? 1000.0 / avg : 0.0, avg, sumMax, game, game - sumEye1 / n, sumEye1 / n, sumFlush / n, sumWait / n,
                vr::S.renderW, vr::S.renderH, vr::S.displayHz);
            sumFrame = sumFlush = sumWait = sumMax = sumEye1 = 0;
        }
    }
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
