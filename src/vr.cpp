#include "vr.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <cstdio>

// The C header typedefs bool when __WIN32 is defined, which is not valid C++.
#define OPENVR_API_NODLL
#pragma push_macro("__WIN32")
#undef __WIN32
extern "C" {
#include "openvr_capi.h"
}
#pragma pop_macro("__WIN32")

namespace vr {

Setup S;

static HMODULE g_dll;
static bool g_active = false;
static VR_IVRSystem_FnTable* g_sys;
static VR_IVRCompositor_FnTable* g_comp;
static VR_IVRRenderModels_FnTable* g_rm;

typedef intptr_t (*InitInternal_t)(EVRInitError*, EVRApplicationType);
typedef void (*ShutdownInternal_t)();
typedef bool (*IsHmdPresent_t)();
typedef intptr_t (*GetGenericInterface_t)(const char*, EVRInitError*);
typedef const char* (*ErrDesc_t)(EVRInitError);
static InitInternal_t pInit;
static ShutdownInternal_t pShutdown;
static IsHmdPresent_t pIsHmdPresent;
static GetGenericInterface_t pGetIface;
static ErrDesc_t pErrDesc;

struct DeviceCache {
    uint32_t index = 0xFFFFFFFF;
    int stickAxis = 0, triggerAxis = 1, gripAxis = -1;
    bool haveTip = false;
    Mat34 tip = Mat34::Identity();
    char type[64] = "";
};
static DeviceCache g_dev[2];

static bool LoadDll() {
    if (g_dll) return true;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    wcscat(path, L"openvr_api.dll");
    g_dll = LoadLibraryW(path);
    if (!g_dll) {
        Log("vr: openvr_api.dll not found next to the game executable");
        return false;
    }
    pInit = (InitInternal_t)GetProcAddress(g_dll, "VR_InitInternal");
    pShutdown = (ShutdownInternal_t)GetProcAddress(g_dll, "VR_ShutdownInternal");
    pIsHmdPresent = (IsHmdPresent_t)GetProcAddress(g_dll, "VR_IsHmdPresent");
    pGetIface = (GetGenericInterface_t)GetProcAddress(g_dll, "VR_GetGenericInterface");
    pErrDesc = (ErrDesc_t)GetProcAddress(g_dll, "VR_GetVRInitErrorAsEnglishDescription");
    return pInit && pShutdown && pIsHmdPresent && pGetIface;
}

bool HmdPresent() {
    if (g_cfg.fakeHmd) return true;
    if (!LoadDll()) return false;
    return pIsHmdPresent();
}

static Mat34 FromHmd(const HmdMatrix34_t& m) {
    Mat34 r;
    memcpy(r.m, m.m, sizeof(r.m));
    return r;
}

void ComputeFrustum() {
    float tanH = 0, tanV = 0;
    for (int e = 0; e < 2; e++) {
        EyeInfo& ei = S.eye[e];
        if (ei.tanLeft > tanH) tanH = ei.tanLeft;
        if (ei.tanRight > tanH) tanH = ei.tanRight;
        if (ei.tanUp > tanV) tanV = ei.tanUp;
        if (ei.tanDown > tanV) tanV = ei.tanDown;
    }
    S.tanH = tanH;
    S.tanV = tanV;
    S.fovDeg = 2.0f * atanf(tanH) * 180.0f / 3.14159265f;
    float sx = 1, sy = 1;
    for (int e = 0; e < 2; e++) {
        EyeInfo& ei = S.eye[e];
        ei.uMin = (tanH - ei.tanLeft) / (2 * tanH);
        ei.uMax = (tanH + ei.tanRight) / (2 * tanH);
        ei.vMin = (tanV - ei.tanUp) / (2 * tanV);
        ei.vMax = (tanV + ei.tanDown) / (2 * tanV);
        float fx = 1.0f / (ei.uMax - ei.uMin), fy = 1.0f / (ei.vMax - ei.vMin);
        if (fx > sx) sx = fx;
        if (fy > sy) sy = fy;
    }
    // The cropped region should keep the recommended pixel density.
    float w = S.recW * sx * g_cfg.resolutionScale;
    float h = S.recH * sy * g_cfg.resolutionScale;
    // Keep the exact aspect of the symmetric frustum.
    h = w * tanV / tanH;
    if (g_cfg.maxRenderWidth > 0 && w > g_cfg.maxRenderWidth) {
        h *= g_cfg.maxRenderWidth / w;
        w = (float)g_cfg.maxRenderWidth;
    }
    for (int e = 0; e < 2; e++) {
        EyeInfo& ei = S.eye[e];
        ei.projX = 1.0f / tanH;
        ei.projY = 1.0f / tanV;
        ei.offX = ei.offY = 0;
    }
    S.lineTanH = tanH;
    S.lineTanV = tanV;
    S.asymmetric = g_cfg.asymmetricFrustum && !S.fake;
    if (S.asymmetric) {
        // Each eye renders exactly its own (off-centre) frustum on the whole image: no pixels
        // outside the lens are drawn, and the engine culls everything outside that frustum. The
        // recommended size already covers one eye's frustum at the right pixel density.
        w = S.recW * g_cfg.resolutionScale;
        h = S.recH * g_cfg.resolutionScale;
        if (g_cfg.maxRenderWidth > 0 && w > g_cfg.maxRenderWidth) {
            h *= g_cfg.maxRenderWidth / w;
            w = (float)g_cfg.maxRenderWidth;
        }
        float lh = 0, lv = 0;
        for (int e = 0; e < 2; e++) {
            EyeInfo& ei = S.eye[e];
            float wx = ei.tanLeft + ei.tanRight, wy = ei.tanUp + ei.tanDown;
            ei.projX = 2.0f / wx;
            ei.offX = (ei.tanLeft - ei.tanRight) / wx;
            ei.projY = 2.0f / wy;
            ei.offY = (ei.tanDown - ei.tanUp) / wy;
            ei.uMin = ei.vMin = 0;
            ei.uMax = ei.vMax = 1;
            lh += wx * 0.25f;
            lv += wy * 0.25f;
        }
        S.lineTanH = lh;
        S.lineTanV = lv;
    }
    S.renderW = ((uint32_t)(w + 0.5f) + 3) & ~3u;
    S.renderH = ((uint32_t)(h + 0.5f) + 3) & ~3u;
    Log("vr: recommended %ux%u, symmetric tanH=%.3f tanV=%.3f fov=%.1f, render %ux%u%s", S.recW, S.recH, tanH, tanV, S.fovDeg,
        S.renderW, S.renderH, S.asymmetric ? " (asymmetric per-eye frustum)" : "");
    for (int e = 0; e < 2; e++)
        Log("vr: eye %d tan L%.3f R%.3f U%.3f D%.3f bounds u[%.3f,%.3f] v[%.3f,%.3f] offset(%.4f,%.4f,%.4f)", e, S.eye[e].tanLeft,
            S.eye[e].tanRight, S.eye[e].tanUp, S.eye[e].tanDown, S.eye[e].uMin, S.eye[e].uMax, S.eye[e].vMin, S.eye[e].vMax,
            S.eye[e].eyeToHead.m[0][3], S.eye[e].eyeToHead.m[1][3], S.eye[e].eyeToHead.m[2][3]);
}

static void InitFake() {
    S.fake = true;
    S.recW = 1600;
    S.recH = 1760;
    S.displayHz = 90;
    for (int e = 0; e < 2; e++) {
        EyeInfo& ei = S.eye[e];
        ei.tanLeft = e == 0 ? 1.25f : 1.0f;
        ei.tanRight = e == 0 ? 1.0f : 1.25f;
        ei.tanUp = 1.1f;
        ei.tanDown = 1.25f;
        ei.eyeToHead = Translate(e == 0 ? -0.032f : 0.032f, 0, 0);
    }
    ComputeFrustum();
    g_active = true;
    Log("vr: FAKE headset mode");
}

static void* Iface(const char* version) {
    char name[128];
    snprintf(name, sizeof(name), "FnTable:%s", version);
    EVRInitError err = EVRInitError_VRInitError_None;
    void* p = (void*)pGetIface(name, &err);
    if (!p || err != EVRInitError_VRInitError_None)
        Log("vr: interface %s unavailable (%d %s)", version, err, pErrDesc ? pErrDesc(err) : "");
    return p;
}

bool Init() {
    if (g_active) return true;
    if (g_cfg.fakeHmd) {
        InitFake();
        return true;
    }
    if (!LoadDll()) return false;
    EVRInitError err = EVRInitError_VRInitError_None;
    pInit(&err, EVRApplicationType_VRApplication_Scene);
    if (err != EVRInitError_VRInitError_None) {
        Log("vr: VR_Init failed: %d %s", err, pErrDesc ? pErrDesc(err) : "");
        return false;
    }
    g_sys = (VR_IVRSystem_FnTable*)Iface(IVRSystem_Version);
    g_comp = (VR_IVRCompositor_FnTable*)Iface(IVRCompositor_Version);
    g_rm = (VR_IVRRenderModels_FnTable*)Iface(IVRRenderModels_Version);
    if (!g_sys || !g_comp) {
        pShutdown();
        return false;
    }
    g_comp->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseStanding);
    g_sys->GetRecommendedRenderTargetSize(&S.recW, &S.recH);
    ETrackedPropertyError perr;
    float hz = g_sys->GetFloatTrackedDeviceProperty(0, ETrackedDeviceProperty_Prop_DisplayFrequency_Float, &perr);
    if (hz > 20 && hz < 400) S.displayHz = hz;
    float v2p = g_sys->GetFloatTrackedDeviceProperty(0, ETrackedDeviceProperty_Prop_SecondsFromVsyncToPhotons_Float, &perr);
    if (v2p >= 0 && v2p < 0.1f) S.vsyncToPhotons = v2p;
    int32_t adapter = 0;
    g_sys->GetDXGIOutputInfo(&adapter);
    S.adapterIndex = adapter < 0 ? 0 : adapter;
    char model[128] = "", tracking[128] = "";
    g_sys->GetStringTrackedDeviceProperty(0, ETrackedDeviceProperty_Prop_ModelNumber_String, model, sizeof(model), &perr);
    g_sys->GetStringTrackedDeviceProperty(0, ETrackedDeviceProperty_Prop_TrackingSystemName_String, tracking, sizeof(tracking), &perr);
    Log("vr: headset '%s' (%s) %.1f Hz, adapter %d", model, tracking, S.displayHz, S.adapterIndex);

    float ipd = g_sys->GetFloatTrackedDeviceProperty(0, ETrackedDeviceProperty_Prop_UserIpdMeters_Float, &perr);
    if (!(ipd > 0.04f && ipd < 0.09f)) ipd = 0.064f;
    for (int e = 0; e < 2; e++) {
        float l = 0, r = 0, t = 0, b = 0;
        g_sys->GetProjectionRaw((EVREye)e, &l, &r, &t, &b);
        EyeInfo& ei = S.eye[e];
        ei.tanLeft = -l;
        ei.tanRight = r;
        // OpenVR's "top" is the negative (downward-looking in clip space) value: the positive one is up.
        float hi = t > b ? t : b, lo = t > b ? b : t;
        ei.tanUp = hi;
        ei.tanDown = -lo;
        HmdMatrix34_t m = g_sys->GetEyeToHeadTransform((EVREye)e);
        ei.eyeToHead = FromHmd(m);
        // Sanity check the struct-return ABI; fall back to a plain IPD offset.
        Vec3 c0 = ei.eyeToHead.Col(0), p = ei.eyeToHead.Pos();
        bool sane = fabsf(Length(c0) - 1.0f) < 0.05f && fabsf(p.x) < 0.1f && fabsf(p.y) < 0.1f && fabsf(p.z) < 0.2f &&
                    fabsf(p.x) > 0.01f;
        if (!sane) {
            Log("vr: eye-to-head transform for eye %d looks invalid, using IPD %.4f", e, ipd);
            ei.eyeToHead = Translate(e == 0 ? -ipd / 2 : ipd / 2, 0, 0);
        }
        if (!(ei.tanLeft > 0.2f && ei.tanRight > 0.2f && ei.tanUp > 0.2f && ei.tanDown > 0.2f)) {
            Log("vr: projection for eye %d looks invalid (%f %f %f %f), using defaults", e, l, r, t, b);
            ei.tanLeft = ei.tanRight = ei.tanUp = ei.tanDown = 1.0f;
        }
    }
    ComputeFrustum();
    g_active = true;
    return true;
}

bool Active() { return g_active; }

void Shutdown() {
    if (g_active && !S.fake && pShutdown) pShutdown();
    g_active = false;
}

static void RefreshDevice(int hand, uint32_t index) {
    DeviceCache& d = g_dev[hand];
    if (d.index == index) return;
    d = DeviceCache();
    d.index = index;
    if (index == 0xFFFFFFFF) return;
    ETrackedPropertyError perr;
    bool haveStick = false, haveTrigger = false;
    int trackpad = -1;
    for (int a = 0; a < 5; a++) {
        int32_t type = g_sys->GetInt32TrackedDeviceProperty(index, (ETrackedDeviceProperty)(ETrackedDeviceProperty_Prop_Axis0Type_Int32 + a), &perr);
        if (type == EVRControllerAxisType_k_eControllerAxis_Joystick && !haveStick) {
            d.stickAxis = a;
            haveStick = true;
        } else if (type == EVRControllerAxisType_k_eControllerAxis_TrackPad && trackpad < 0) {
            trackpad = a;
        } else if (type == EVRControllerAxisType_k_eControllerAxis_Trigger) {
            if (!haveTrigger) {
                d.triggerAxis = a;
                haveTrigger = true;
            } else if (d.gripAxis < 0) {
                d.gripAxis = a;
            }
        }
    }
    if (!haveStick && trackpad >= 0) d.stickAxis = trackpad;
    g_sys->GetStringTrackedDeviceProperty(index, ETrackedDeviceProperty_Prop_ControllerType_String, d.type, sizeof(d.type), &perr);
    char model[128] = "";
    g_sys->GetStringTrackedDeviceProperty(index, ETrackedDeviceProperty_Prop_RenderModelName_String, model, sizeof(model), &perr);
    if (g_rm && model[0]) {
        VRControllerState_t cs;
        memset(&cs, 0, sizeof(cs));
        RenderModel_ControllerMode_State_t mode;
        memset(&mode, 0, sizeof(mode));
        RenderModel_ComponentState_t comp;
        memset(&comp, 0, sizeof(comp));
        char tipName[] = "tip";
        if (g_rm->GetComponentState(model, tipName, &cs, &mode, &comp)) {
            Mat34 t = FromHmd(comp.mTrackingToComponentLocal);
            if (fabsf(Length(t.Col(0)) - 1.0f) < 0.05f && Length(t.Pos()) < 0.5f) {
                d.tip = t;
                d.haveTip = true;
            }
        }
    }
    Log("vr: %s controller: index %u type '%s' model '%s' stickAxis=%d triggerAxis=%d gripAxis=%d tip=%d", hand ? "right" : "left",
        index, d.type, model, d.stickAxis, d.triggerAxis, d.gripAxis, d.haveTip);
}

void GetFrame(Frame& out) {
    out = Frame();
    if (!g_active) return;
    if (S.fake) {
        out.hmdValid = true;
        out.hmd = Translate(0, 1.65f, 0);
        // Development aid: DebugFakeYaw / DebugFakePitch (degrees) turn the fake head.
        if (g_cfg.debugFakeYaw != 0 || g_cfg.debugFakePitch != 0) {
            float yaw = g_cfg.debugFakeYaw, pitch = g_cfg.debugFakePitch;
            float a = -yaw * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
            Mat34 ry = Mat34::Identity();
            ry.m[0][0] = c; ry.m[0][2] = s;
            ry.m[2][0] = -s; ry.m[2][2] = c;
            out.hmd = Mul(Mul(out.hmd, ry), RotX(pitch));
        }
        for (int h = 0; h < 2; h++) {
            out.hand[h].valid = true;
            out.hand[h].raw = Translate(h ? 0.2f : -0.2f, 1.25f, -0.45f);
            out.hand[h].aim = out.hand[h].raw;
        }
        return;
    }
    float sinceVsync = 0;
    uint64_t frameCounter = 0;
    g_sys->GetTimeSinceLastVsync(&sinceVsync, &frameCounter);
    float frameDur = 1.0f / S.displayHz;
    float predict = frameDur - sinceVsync + S.vsyncToPhotons + frameDur;
    if (predict < 0) predict = 0;
    if (predict > 0.1f) predict = 0.1f;
    static TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
    g_sys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding, predict, poses, k_unMaxTrackedDeviceCount);
    if (poses[0].bPoseIsValid) {
        out.hmdValid = true;
        out.hmd = FromHmd(poses[0].mDeviceToAbsoluteTracking);
    }
    for (int h = 0; h < 2; h++) {
        uint32_t idx = g_sys->GetTrackedDeviceIndexForControllerRole(
            h == 0 ? ETrackedControllerRole_TrackedControllerRole_LeftHand : ETrackedControllerRole_TrackedControllerRole_RightHand);
        if (idx >= k_unMaxTrackedDeviceCount) idx = 0xFFFFFFFF;
        RefreshDevice(h, idx);
        if (idx == 0xFFFFFFFF) continue;
        Hand& hd = out.hand[h];
        DeviceCache& d = g_dev[h];
        if (poses[idx].bPoseIsValid) {
            hd.valid = true;
            hd.raw = FromHmd(poses[idx].mDeviceToAbsoluteTracking);
            hd.aim = d.haveTip ? Mul(hd.raw, d.tip) : hd.raw;
            if (g_cfg.aimPitchOffset != 0) hd.aim = Mul(hd.aim, RotX(-g_cfg.aimPitchOffset));
        }
        VRControllerState_t cs;
        memset(&cs, 0, sizeof(cs));
        if (g_sys->GetControllerState(idx, &cs, sizeof(cs))) {
            hd.stickX = cs.rAxis[d.stickAxis].x;
            hd.stickY = cs.rAxis[d.stickAxis].y;
            hd.trigger = cs.rAxis[d.triggerAxis].x;
            if (d.gripAxis >= 0) hd.grip = cs.rAxis[d.gripAxis].x;
            uint64_t b = cs.ulButtonPressed;
            hd.triggerBtn = (b & (1ull << EVRButtonId_k_EButton_SteamVR_Trigger)) != 0 || hd.trigger > 0.6f;
            hd.gripBtn = (b & (1ull << EVRButtonId_k_EButton_Grip)) != 0 || hd.grip > 0.6f;
            hd.aBtn = (b & (1ull << EVRButtonId_k_EButton_A)) != 0;
            hd.bBtn = (b & (1ull << EVRButtonId_k_EButton_ApplicationMenu)) != 0;
            hd.stickBtn = (b & (1ull << (EVRButtonId_k_EButton_Axis0 + d.stickAxis))) != 0;
        }
    }
}

void WaitGetPoses() {
    if (!g_active || S.fake) {
        if (S.fake) Sleep(8);
        return;
    }
    static TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
    EVRCompositorError err = g_comp->WaitGetPoses(poses, k_unMaxTrackedDeviceCount, nullptr, 0);
    static int logged = 0;
    if (err != EVRCompositorError_VRCompositorError_None && logged++ < 10) Log("vr: WaitGetPoses error %d", err);
}

// Submit diagnostics (DebugEvents): result of the last submit and how many were sent.
static volatile LONG g_submits = 0, g_lastSubmitErr = -1;

bool Submit(void* left, void* right, const Mat34& hmdPoseUsed) {
    if (!g_active || S.fake) return true;
    bool ok = true;
    int firstErr = 0;
    void* tex[2] = {left, right};
    for (int e = 0; e < 2; e++) {
        VRTextureWithPose_t t;
        t.handle = tex[e];
        t.eType = ETextureType_TextureType_DirectX;
        t.eColorSpace = EColorSpace_ColorSpace_Gamma;
        memcpy(t.mDeviceToAbsoluteTracking.m, hmdPoseUsed.m, sizeof(hmdPoseUsed.m));
        VRTextureBounds_t b = {S.eye[e].uMin, S.eye[e].vMin, S.eye[e].uMax, S.eye[e].vMax};
        EVRCompositorError err = g_comp->Submit((EVREye)e, (Texture_t*)&t, &b, EVRSubmitFlags_Submit_TextureWithPose);
        static int logged = 0;
        if (err != EVRCompositorError_VRCompositorError_None) {
            ok = false;
            if (!firstErr) firstErr = err;
            if (logged++ < 20) Log("vr: Submit eye %d error %d", e, err);
        }
    }
    InterlockedIncrement(&g_submits);
    LONG prev = InterlockedExchange(&g_lastSubmitErr, firstErr);
    if (g_cfg.debugEvents && prev != firstErr) Log("vrstate: submit result %d (was %ld)", firstErr, prev);
    return ok;
}

// DebugEvents: while the SteamVR dashboard is open (and for 30 s after it closes), a status line
// every 2 s: whether SteamVR still takes our frames, which process it shows as the scene app,
// and whether it asks the game to pause. For issue #1 (dashboard panel staying in front).
static bool g_dashboardOpen = false;
static DWORD g_dashboardClosedAt = 0;
static void LogVrState() {
    static DWORD last = 0;
    static LONG lastSubmits = 0;
    DWORD now = GetTickCount();
    bool recent = g_dashboardOpen || (g_dashboardClosedAt && now - g_dashboardClosedAt < 30000);
    if (!recent || now - last < 2000) return;
    LONG submits = g_submits;
    Log("vrstate: dashboard %s | submits +%ld, last result %ld | scene focus pid %u (ours %lu), last renderer %u | canRender %d shouldPause %d",
        g_dashboardOpen ? "open" : "closed", submits - lastSubmits, (long)g_lastSubmitErr, g_comp->GetCurrentSceneFocusProcess(),
        GetCurrentProcessId(), g_comp->GetLastFrameRenderer(), g_comp->CanRenderScene(), g_sys->ShouldApplicationPause());
    last = now;
    lastSubmits = submits;
}

void PollEvents(bool& quitRequested) {
    quitRequested = false;
    if (!g_active || S.fake) return;
    if (g_cfg.debugEvents) LogVrState();
    VREvent_t ev;
    while (g_sys->PollNextEvent(&ev, sizeof(ev))) {
        if (ev.eventType == EVREventType_VREvent_DashboardActivated) g_dashboardOpen = true;
        if (ev.eventType == EVREventType_VREvent_DashboardDeactivated) {
            g_dashboardOpen = false;
            g_dashboardClosedAt = GetTickCount();
        }
        if (g_cfg.debugEvents) {
            // Focus / dashboard / standby events, to diagnose what happens when the player leaves
            // the game for the SteamVR dashboard and comes back.
            static int logged = 0;
            const char* name = nullptr;
            switch (ev.eventType) {
            case EVREventType_VREvent_InputFocusCaptured: name = "InputFocusCaptured"; break;
            case EVREventType_VREvent_InputFocusReleased: name = "InputFocusReleased"; break;
            case EVREventType_VREvent_SceneApplicationChanged: name = "SceneApplicationChanged"; break;
            case EVREventType_VREvent_InputFocusChanged: name = "InputFocusChanged"; break;
            case EVREventType_VREvent_SceneApplicationStateChanged: name = "SceneApplicationStateChanged"; break;
            case EVREventType_VREvent_DashboardActivated: name = "DashboardActivated"; break;
            case EVREventType_VREvent_DashboardDeactivated: name = "DashboardDeactivated"; break;
            case EVREventType_VREvent_ResetDashboard: name = "ResetDashboard"; break;
            case EVREventType_VREvent_OverlayShown: name = "OverlayShown"; break;
            case EVREventType_VREvent_OverlayHidden: name = "OverlayHidden"; break;
            case EVREventType_VREvent_EnterStandbyMode: name = "EnterStandbyMode"; break;
            case EVREventType_VREvent_LeaveStandbyMode: name = "LeaveStandbyMode"; break;
            case EVREventType_VREvent_TrackedDeviceUserInteractionStarted: name = "UserInteractionStarted"; break;
            case EVREventType_VREvent_TrackedDeviceUserInteractionEnded: name = "UserInteractionEnded"; break;
            case EVREventType_VREvent_Quit: name = "Quit"; break;
            case EVREventType_VREvent_ProcessQuit: name = "ProcessQuit"; break;
            default: break;
            }
            if (name && logged++ < 300) Log("vrevent: %s (%u) device %u", name, ev.eventType, ev.trackedDeviceIndex);
        }
        if (ev.eventType == EVREventType_VREvent_Quit || ev.eventType == EVREventType_VREvent_ProcessQuit) {
            g_sys->AcknowledgeQuit_Exiting();
            quitRequested = true;
        } else if (ev.eventType == EVREventType_VREvent_TrackedDeviceRoleChanged ||
                   ev.eventType == EVREventType_VREvent_TrackedDeviceActivated) {
            g_dev[0].index = 0xFFFFFFFE;
            g_dev[1].index = 0xFFFFFFFE;
        }
    }
}

void Haptic(int hand, int microseconds) {
    if (!g_active || S.fake) return;
    uint32_t idx = g_dev[hand].index;
    if (idx >= k_unMaxTrackedDeviceCount) return;
    g_sys->TriggerHapticPulse(idx, 0, (unsigned short)microseconds);
}

} // namespace vr
