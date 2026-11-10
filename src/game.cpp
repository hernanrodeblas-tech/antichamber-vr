// Game-side VR logic for Antichamber (UDK build 7977, 32-bit).
//
//  * FViewport::Draw is called twice per frame, once per eye.
//  * UObject::ProcessInternal (the UnrealScript interpreter entry) is hooked to
//    override the player's view point, FOV, input axes and aim.
//  * Head position drives the pawn (roomscale); controllers drive movement,
//    turning, jumping and the three fire modes; the gun follows the hand.
#include "game.h"
#include "config.h"
#include "d3dhook.h"
#include "log.h"
#include "ue3.h"
#include "vr.h"
#include <windows.h>
#include <MinHook.h>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dbgdump { void Run(ue::Obj pc, ue::Obj localPlayer, int boolMaskOff); void DumpObject(ue::Obj o, const char* tag, int boolMaskOff); void XrefTile(); }
namespace shading { void Init(void (*callFn)(ue::Obj, ue::Obj, void*)); void Toggle(); void ApplyLineSampling(ue::Obj activeMic, float sx, float sy); }

namespace game {

static bool GetBool(ue::Obj obj, ue::Obj cls, const char* name);

using ue::At;
using ue::Obj;

// Addresses in UDK.exe (image base 0x400000, no ASLR), verified by signature.
static const uintptr_t kViewportDraw = 0x0086A7E0; // FViewport::Draw(UBOOL bShouldPresent)
static const uint8_t kViewportDrawSig[] = {0x6A, 0xFF, 0x68, 0x92, 0x7A, 0xCB, 0x01, 0x64, 0xA1};
static const int kProcessEventVtblIndex = 0x108 / 4;
static const uintptr_t kDesktopSizeX = 0x025D24E0, kDesktopSizeY = 0x025D24E4; // cached desktop size (ints)
static const uintptr_t kNearClipPlane = 0x0243FE90; // GNearClippingPlane (float, 10.0 by default)

// FFrame layout
static const int kFnINative = 0x94; // UFunction::iNative
static const int kFrameNode = 0x10, kFrameLocals = 0x1C, kFrameOutParms = 0x24;

typedef void(__fastcall* ViewportDraw_t)(void* self, void* edx, int bShouldPresent);
typedef void(__fastcall* ProcessInternal_t)(void* self, void* edx, uint8_t* stack, void* result);
static ViewportDraw_t oViewportDraw;
static ProcessInternal_t oProcessInternal;

struct FVector { float x, y, z; };
struct FRotator { int pitch, yaw, roll; };
struct FString { const wchar_t* data; int num; int max; };

// --- Reflection handles -----------------------------------------------------
static Obj fnGPVP, fnPCFov, fnCamFov, fnPawnViewLoc, fnPawnViewRot, fnPlayerInput, fnPlayerTick, fnUpdateRotation;
static Obj fnMove, fnFaceRotation, fnConsoleCommand, fnJump, fnStartFire, fnStopFire, fnStartAltFire, fnStopAltFire,
    fnStartTriFire, fnStopTriFire, fnShowMenu, fnCancelExit;
static Obj propOutLoc, propOutRot;
static Obj fnDrawHUD, fnTrace, fnDraw2DLine, g_hudClass, g_pcClass;
static int oHudCanvas = -1, oCanvasClipX = -1, oCanvasClipY = -1;
static int oTrHitLoc, oTrHitNormal, oTrEnd, oTrStart, oTrActors, oTrRet;
static int oLnX1, oLnY1, oLnX2, oLnY2, oLnColor;
static bool g_lineInts = true;
static bool g_crosshairOk = false;
static Obj g_localPlayer, g_engine, g_defaultPawn;
static std::unordered_set<void*> g_aimFns;
static std::unordered_set<void*> g_tileFns; // placement-preview script functions (diagnostics)

static int oLocation, oRotation, oPcPawn, oPcInput, oPcFOV, oPcDesiredFOV, oPcDefaultFOV, oPcViewTarget, oLpActor;
static int oPawnWeapon, oPawnBaseEye, oPawnEye, oPawnCyl, oPawnGroundSpeed, oCylHeight, oCylRadius;
static int oInBaseY, oInStrafe, oWeapViewOffset;
static int oBoolMask = -1;

// --- State (game thread) ------------------------------------------------------
static bool g_ready = false, g_initFailed = false;
static int g_draws = 0;
static int g_eye = -1;      // eye being drawn, -1 outside FViewport::Draw
static int g_aimDepth = 0;  // >0 while weapon / interaction script code runs
static vr::Frame g_frame;   // latest poses
static bool g_haveFrame = false;
static float g_playYaw = 0; // degrees, playspace rotation relative to the world
static float g_anchorX = 0, g_anchorZ = 0; // tracking-space point mapped onto the pawn
static bool g_anchorInit = false;
static float g_seatedOffset = 0;  // extra height in seated mode (seatedEyeHeight - head height at calibration)
static bool g_seatedCalibrated = false;
static float g_moveX = 0, g_moveY = 0;
static bool g_snapLatch = false;
static bool g_setupDone = false;
static int g_lastYaw = 0;          // controller yaw the mod last wrote
static bool g_haveLastYaw = false;
static bool g_rotSwapped = false;  // controller rotation temporarily replaced (weapon placement)
static Obj fnSetPosition, fnSetLocation;
static Obj fnWorldCam, fnChamberCam;
// Engine global toggled by the TOGGLEOCCLUSION console command: when set, views ignore
// occlusion queries. Verified against the instruction that writes it in that command.
static const uintptr_t kIgnoreOcclusionWrite = 0x008EDD68; // mov [0x025ADF7C], eax
static const uint8_t kIgnoreOcclusionSig[] = {0xA3, 0x7C, 0xDF, 0x5A, 0x02};
static int* g_ignoreOcclusion = nullptr;
// GFrameCounter (64-bit), incremented once per engine loop iteration by FEngineLoop::Tick.
static const uintptr_t kFrameCounterInc = 0x0162593A; // add dword ptr [0x024E1968], 1
static const uint8_t kFrameCounterSig[] = {0x83, 0x05, 0x68, 0x19, 0x4E, 0x02, 0x01};
static uint64_t* g_frameCounter = nullptr;
static int g_savedOcclusion = 0;
static Obj g_capFn = nullptr;   // capture-camera update the game ran this frame (redone per eye)
static Obj g_capSelf = nullptr;
static DWORD g_lastResFix = 0;
static DWORD g_jumpTick = 0;
static Obj fnSetFOV, fnSetScale, fnUpdWeapPos, fnActiveOverlays, fnInteractTest;
// Block placement preview: ShowProjectedTile draws its outline with DrawDebugBox. Those debug
// lines live for a single render, so only the left eye got them. The lines the call adds to the
// engine's line batcher are copied and drawn again in the right eye's HUD with Canvas.Draw3DLine.
static Obj fnShowTile = nullptr, fnDraw3DLine = nullptr;
static std::vector<Obj> g_lineBatchers;
struct TileLine { FVector a, b; uint32_t color; };
static std::vector<TileLine> g_tileLines;
static int o3dStart = -1, o3dEnd = -1, o3dColor = -1;
static int oMeshFOV = -1, oMeshScale = -1, oWeapRotator = -1;
static FVector g_aimHit = {0, 0, 0};
static bool g_aimHitValid = false;
static bool g_prevFire = false, g_prevAlt = false, g_prevTri = false, g_prevJump = false, g_prevMenu = false;

// ---------------------------------------------------------------------------

// Calls a script or native function through UObject::ProcessEvent. ProcessEvent silently
// rejects natives that have a native index (UFunction::iNative != 0, e.g. Trace and Move),
// so the index is cleared for the duration of the call. Script bytecode reaches those
// natives through the GNatives table and is not affected.
static void CallFn(Obj obj, Obj fn, void* parms) {
    typedef void(__thiscall * ProcessEvent_t)(void*, void*, void*, void*);
    ProcessEvent_t pe = (ProcessEvent_t)(*(void***)obj)[kProcessEventVtblIndex];
    uint16_t& iNative = At<uint16_t>(fn, kFnINative);
    uint16_t saved = iNative;
    iNative = 0;
    pe(obj, fn, parms, nullptr);
    iNative = saved;
}

static int ParmsSize(Obj fn) { return At<uint16_t>(fn, 0xA6); }

static void CallNoArgs(Obj obj, Obj fn) {
    if (!obj || !fn) return;
    uint8_t parms[128];
    memset(parms, 0, sizeof(parms));
    if (ParmsSize(fn) > (int)sizeof(parms)) return;
    CallFn(obj, fn, parms);
}

static void ConsoleCommand(Obj pc, const wchar_t* cmd) {
    if (!pc || !fnConsoleCommand) return;
    uint8_t parms[64];
    memset(parms, 0, sizeof(parms));
    FString* s = (FString*)parms;
    s->data = cmd;
    s->num = s->max = (int)wcslen(cmd) + 1;
    CallFn(pc, fnConsoleCommand, parms);
    Log("console: %ls", cmd);
}

static void SetBool(Obj obj, Obj cls, const char* name, bool value) {
    Obj prop = ue::FindField(cls, name);
    if (!prop || oBoolMask < 0) {
        Log("SetBool: %s not available", name);
        return;
    }
    uint32_t mask = At<uint32_t>(prop, oBoolMask);
    uint32_t& word = At<uint32_t>(obj, At<int>(prop, ue::L.pOffset));
    if (value) word |= mask;
    else word &= ~mask;
}

static Obj Pc() { return g_localPlayer ? At<Obj>(g_localPlayer, oLpActor) : nullptr; }
static Obj PawnOf(Obj pc) { return pc ? At<Obj>(pc, oPcPawn) : nullptr; }

// Tracking-space direction (x right, y up, -z forward) -> world direction
// (X forward, Y right, Z up), including the playspace yaw.
static Vec3 DirToWorld(const Vec3& t) {
    float a = g_playYaw * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    float x = -t.z, y = t.x;
    return Vec3(x * c - y * s, x * s + y * c, t.y);
}

// World XY delta -> tracking-space XZ delta (metres).
static void WorldToTracking(float wx, float wy, float& tx, float& tz) {
    float a = g_playYaw * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    float x = (wx * c + wy * s) / g_cfg.worldScale;
    float y = (-wx * s + wy * c) / g_cfg.worldScale;
    tx = y;
    tz = -x;
}

// Where the tracking-space floor origin under the anchor sits in the world.
static bool BasePoint(Obj pawn, Vec3& base) {
    if (!pawn) return false;
    FVector loc = At<FVector>(pawn, oLocation);
    Obj cyl = At<Obj>(pawn, oPawnCyl);
    float halfHeight = cyl ? At<float>(cyl, oCylHeight) : 44.0f;
    // EyeHeight - BaseEyeHeight carries the engine's stair / landing smoothing.
    float smooth = At<float>(pawn, oPawnEye) - At<float>(pawn, oPawnBaseEye);
    if (smooth > 40 || smooth < -40) smooth = 0;
    base = Vec3(loc.x, loc.y, loc.z - halfHeight + smooth);
    return true;
}

static void PoseToWorld(const Mat34& m, const Vec3& base, FVector& outLoc, FRotator& outRot) {
    Vec3 p = m.Pos();
    Vec3 rel(p.x - g_anchorX, p.y + g_cfg.heightOffset + g_seatedOffset, p.z - g_anchorZ);
    Vec3 w = base + DirToWorld(rel) * g_cfg.worldScale;
    outLoc = {w.x, w.y, w.z};
    Rotator r = RotatorFromAxes(DirToWorld(m.Forward()), DirToWorld(m.Right()), DirToWorld(m.Up()));
    outRot = {r.pitch, r.yaw, r.roll};
}

static int GunHand() { return g_cfg.leftHanded ? 0 : 1; }

// Decides which tracked pose a "where is the player looking" query should get.
// Seamless teleports (and anything else in the game) rotate the player by writing a new
// yaw into the controller. The mod owns the controller rotation, so any yaw it did not
// write itself is folded into the playspace rotation: the world turns with the teleport
// and the cut stays invisible, exactly like on a flat screen.
static void SyncYaw(Obj pc) {
    if (!g_haveLastYaw || g_rotSwapped || !pc) return;
    int cur = At<FRotator>(pc, oRotation).yaw;
    int d = (int)(int16_t)(cur - g_lastYaw);
    if (d == 0) return;
    g_playYaw += d * (360.0f / 65536.0f);
    g_lastYaw = cur;
}

static bool CurrentView(Obj pc, FVector& loc, FRotator& rot, bool allowAim) {
    if (!g_haveFrame || !g_frame.hmdValid) return false;
    SyncYaw(pc);
    Obj pawn = PawnOf(pc);
    Vec3 base;
    if (!BasePoint(pawn, base)) return false;
    Obj viewTarget = At<Obj>(pc, oPcViewTarget);
    if (viewTarget && viewTarget != pawn && viewTarget != pc) return false; // scripted camera
    if (allowAim && g_aimDepth > 0 && g_frame.hand[GunHand()].valid) {
        PoseToWorld(g_frame.hand[GunHand()].aim, base, loc, rot);
        rot.roll = 0;
        return true;
    }
    if (g_eye >= 0) {
        PoseToWorld(Mul(g_frame.hmd, vr::S.eye[g_eye].eyeToHead), base, loc, rot);
        return true;
    }
    PoseToWorld(g_frame.hmd, base, loc, rot);
    return true;
}

// --- Per-tick logic -------------------------------------------------------------

// Switches the game to the per-eye render size. Also used when the player changes the
// resolution from the in-game options wall.
// --- Screen-size render targets ---------------------------------------------------------
// Portals, mirrors and the linework are scene captures rendered into textures that the game
// sizes from the screen resolution when the player changes it in the options. The mod
// switches resolution directly, so those textures kept the start-up (flat) size and look
// pixelated at the headset resolution. Ask the game to resize them, the way its options do.
static DWORD g_targetsDue = 0;

static void LogRenderTargets(const char* when) {
    Obj cls = ue::Find("Class Engine.TextureRenderTarget2D");
    if (!cls) return;
    int oX = ue::PropOffset(cls, "SizeX"), oY = ue::PropOffset(cls, "SizeY");
    std::string line;
    int n = ue::NumObjects(), count = 0;
    for (int i = 0; i < n && count < 24; i++) {
        Obj o = ue::ObjAt(i);
        if (!o || ue::ClassOf(o) != cls || ue::ObjName(o).compare(0, 9, "Default__") == 0) continue;
        line += " " + ue::ObjName(o) + "=" + std::to_string(At<int>(o, oX)) + "x" + std::to_string(At<int>(o, oY));
        count++;
    }
    Log("targets %s:%s", when, line.c_str());
}

static void RefreshScreenTargets(Obj pc) {
    LogRenderTargets("before");
    Obj tsProp = ue::FindField(g_pcClass, "TextureScale");
    if (tsProp && g_cfg.captureScale > 0.05f) {
        // The game's own factor between screen and capture-texture size (0.75 by default).
        Log("targets: TextureScale %.3f -> %.3f", At<float>(pc, At<int>(tsProp, ue::L.pOffset)), g_cfg.captureScale);
        At<float>(pc, At<int>(tsProp, ue::L.pOffset)) = g_cfg.captureScale;
    }
    Obj itsProp = ue::FindField(g_pcClass, "InitialTextureSize");
    if (tsProp && itsProp)
        Log("targets: TextureScale %.3f InitialTextureSize (%d %d)", At<float>(pc, At<int>(tsProp, ue::L.pOffset)),
            At<int>(pc, At<int>(itsProp, ue::L.pOffset)), At<int>(pc, At<int>(itsProp, ue::L.pOffset) + 4));
    for (const char* name : {"Function Hazard.HazardPlayerController.UpdateScreenMaterialResolutions",
                             "Function Hazard.HazardPlayerController.UpdateMaterialResolutionsPC"}) {
        Obj f = ue::Find(name);
        if (!f) { Log("targets: %s not found", name); continue; }
        std::string parms;
        int nParms = 0;
        for (Obj p = At<Obj>(f, ue::L.sChildren); p; p = At<Obj>(p, ue::L.fNext)) {
            parms += " " + ue::ObjName(p) + "@" + std::to_string(At<int>(p, ue::L.pOffset));
            nParms++;
        }
        Log("targets: %s parms size %d:%s", name, ParmsSize(f), parms.c_str());
        if (ParmsSize(f) == 0) CallNoArgs(pc, f);
    }
    LogRenderTargets("after");
}

static void ApplyResolution(Obj pc) {
    g_targetsDue = GetTickCount() + 2000; // once the device has been reset to the new size
    g_lastResFix = GetTickCount();
    wchar_t cmd[64];
    swprintf(cmd, 64, L"setres %ux%uw", vr::S.renderW, vr::S.renderH);
    d3dhook::BeginDesktopSpoof();
    // The engine caches the desktop size at startup and clamps windowed resolutions to it.
    // Cached at startup: work area RECT, virtual screen size and primary screen size.
    static const uintptr_t kCached[] = {0x025D2498, 0x025D249C, 0x025D24A8, 0x025D24AC, kDesktopSizeX, kDesktopSizeY};
    int saved[6];
    bool plausible = true;
    for (int i = 0; i < 6; i++) {
        saved[i] = *(int*)kCached[i];
        if (saved[i] < 480 || saved[i] > 16384) plausible = false;
    }
    Log("engine desktop cache: work %dx%d virtual %dx%d screen %dx%d%s", saved[0], saved[1], saved[2], saved[3], saved[4], saved[5],
        plausible ? "" : " (unexpected, left alone)");
    if (plausible)
        for (int i = 0; i < 6; i++) *(int*)kCached[i] = 16384;
    ConsoleCommand(pc, cmd);
    if (plausible)
        for (int i = 0; i < 6; i++) *(int*)kCached[i] = saved[i];
}

// First-person weapon meshes are drawn with their own 60 degree projection, which only
// makes sense on a flat screen. Render them with the world projection so the gun really
// sits where the hand is, and apply the optional size multiplier.
// Every first-person mesh counts: the gun's own Mesh, the rotating ring's RotationMesh
// and the ammo tiles it carries are separate components, each with its own FOV.
static void FixMeshComponent(Obj mesh);

static void FixWeaponMesh(Obj actor, int depth = 0) {
    if (!actor || oMeshFOV < 0 || !fnSetFOV || !ue::IsObj(actor)) return;
    // Per class: offsets of object/component fields (kind 0) and arrays (kind 1).
    static std::unordered_map<void*, std::vector<std::pair<int, int>>> cache;
    Obj cls = ue::ClassOf(actor);
    auto it = cache.find(cls);
    if (it == cache.end()) {
        std::vector<std::pair<int, int>> fields;
        for (Obj c = cls; c; c = ue::SuperOf(c)) {
            if (ue::ObjName(c) == "Actor") break;
            for (Obj p = At<Obj>(c, ue::L.sChildren); p; p = At<Obj>(p, ue::L.fNext)) {
                std::string pc = ue::ObjName(ue::ClassOf(p));
                if (pc == "ComponentProperty" || pc == "ObjectProperty") fields.push_back({At<int>(p, ue::L.pOffset), 0});
                else if (pc == "ArrayProperty") fields.push_back({At<int>(p, ue::L.pOffset), 1});
            }
        }
        it = cache.emplace(cls, fields).first;
    }
    Obj meshCls = ue::OuterOf(fnSetFOV);
    for (auto& f : it->second) {
        if (f.second == 0) {
            Obj o = At<Obj>(actor, f.first);
            if (o && ue::IsObj(o) && ue::IsA(o, meshCls)) FixMeshComponent(o);
        } else {
            Obj* data = At<Obj*>(actor, f.first);
            int num = At<int>(actor, f.first + 4);
            if (!data || num <= 0 || num > 256 || !Readable(data, num * sizeof(Obj))) continue;
            for (int i = 0; i < num; i++) {
                Obj o = data[i];
                if (!o || !ue::IsObj(o)) continue;
                if (ue::IsA(o, meshCls)) FixMeshComponent(o);
                else if (depth == 0) FixWeaponMesh(o, 1); // ammo tiles may be actors with their own meshes
            }
        }
    }
}

static void FixMeshComponent(Obj mesh) {
    if (At<float>(mesh, oMeshFOV) != 0.0f) {
        uint8_t parms[16];
        memset(parms, 0, sizeof(parms));
        CallFn(mesh, fnSetFOV, parms);
        At<float>(mesh, oMeshFOV) = 0.0f;
    }
    if (oMeshScale >= 0 && fnSetScale && g_cfg.gunScale > 0.01f && g_cfg.gunScale != 1.0f) {
        static std::unordered_set<void*> scaled;
        if (!scaled.count(mesh)) {
            scaled.insert(mesh);
            uint8_t parms[16];
            memset(parms, 0, sizeof(parms));
            *(float*)parms = At<float>(mesh, oMeshScale) * g_cfg.gunScale;
            CallFn(mesh, fnSetScale, parms);
        }
    }
}

static float Deadzone(float v) {
    float dz = g_cfg.stickDeadzone;
    if (fabsf(v) < dz) return 0;
    return (v - (v > 0 ? dz : -dz)) / (1.0f - dz);
}

static void OneTimeSetup(Obj pc) {
    g_setupDone = true;
    Obj engineCls = ue::ClassOf(g_engine);
    if (g_engine) {
        SetBool(g_engine, engineCls, "bSmoothFrameRate", false);
        SetBool(g_engine, engineCls, "bPauseOnLossOfFocus", false);
    }
    ApplyResolution(pc);
    if (!g_cfg.dynamicShadows) {
        // Antichamber draws dynamic shadows as a screen-space hatch pattern: stuck to the view and
        // different in each eye, it warps and turns with the head in VR.
        ConsoleCommand(pc, L"scale set DynamicShadows False");
        ConsoleCommand(pc, L"scale set LightEnvironmentShadows False");
    }
    float* nearClip = (float*)kNearClipPlane;
    if (*nearClip > 9.5f && *nearClip < 10.5f && g_cfg.nearClip > 0.1f) {
        *nearClip = g_cfg.nearClip;
        Log("near clip plane 10.0 -> %.1f", g_cfg.nearClip);
    } else {
        Log("near clip plane left unchanged (%.2f)", *nearClip);
    }
    Obj pawn = PawnOf(pc);
    if (pawn) {
        Obj cyl = At<Obj>(pawn, oPawnCyl);
        Log("pawn: BaseEyeHeight=%.1f EyeHeight=%.1f CollisionHeight=%.1f Radius=%.1f GroundSpeed=%.1f", At<float>(pawn, oPawnBaseEye),
            At<float>(pawn, oPawnEye), cyl ? At<float>(cyl, oCylHeight) : -1, cyl ? At<float>(cyl, oCylRadius) : -1,
            At<float>(pawn, oPawnGroundSpeed));
    }
}

static void Edge(bool now, bool& prev, Obj pc, Obj onPress, Obj onRelease) {
    if (now == prev) return;
    prev = now;
    CallNoArgs(pc, now ? onPress : onRelease);
}

static void PreTick(Obj pc, float dt) {
    g_tileLines.clear();
    g_capFn = nullptr;
    bool quit = false;
    vr::PollEvents(quit);
    if (quit) {
        ConsoleCommand(pc, L"exit");
        return;
    }
    vr::Frame f;
    vr::GetFrame(f);
    if (!f.hmdValid && g_haveFrame) f.hmd = g_frame.hmd, f.hmdValid = g_frame.hmdValid;
    g_frame = f;
    g_haveFrame = true;
    // Recenter: right stick clicked on its own, or F8. Re-centres the body under the head and,
    // when seated, recalibrates the height.
    {
        static bool prevRec = false;
        bool rec = (GetAsyncKeyState(VK_F8) & 0x8000) != 0 || (f.hand[1].stickBtn && !f.hand[0].stickBtn);
        if (f.hmdValid && ((rec && !prevRec) || (g_cfg.seated && !g_seatedCalibrated))) {
            Vec3 h = f.hmd.Pos();
            g_anchorX = h.x;
            g_anchorZ = h.z;
            g_anchorInit = true;
            if (g_cfg.seated) {
                g_seatedOffset = g_cfg.seatedEyeHeight - h.y;
                g_seatedCalibrated = true;
            }
            if (rec) {
                vr::Haptic(1, 2500);
                Log("recenter: head at %.2f m, seated offset %.2f m", h.y, g_seatedOffset);
            }
        }
        prevRec = rec;
    }

    // Shading test: F10 on the keyboard.
    {
        static bool prevTog = false;
        bool tog = (GetAsyncKeyState(VK_F10) & 0x8000) != 0; // keyboard only
        if (tog && !prevTog) {
            shading::Toggle();
            vr::Haptic(0, 2000);
        }
        prevTog = tog;
    }
    // Diagnostic snapshot: F9 on the keyboard or both sticks clicked together.
    {
        static bool prevShot = false;
        bool shot = (GetAsyncKeyState(VK_F9) & 0x8000) != 0 || (f.hand[0].stickBtn && f.hand[1].stickBtn);
        if (shot && !prevShot) {
            d3dhook::RequestShot();
            vr::Haptic(0, 3000);
            vr::Haptic(1, 3000);
            Obj pawnS = PawnOf(pc);
            Obj lwS = At<Obj>(pc, ue::PropOffset(ue::ClassOf(pc), "LineworkMaterialInstance"));
            Obj lwParent = lwS ? At<Obj>(lwS, ue::PropOffset(ue::ClassOf(lwS), "Parent")) : nullptr;
            Obj weap = pawnS ? At<Obj>(pawnS, oPawnWeapon) : nullptr;
            Log("SHOT: pawn %s at (%.0f %.0f %.0f) yaw %d, linework %s, weapon %s, inChamber=%d, capturing=%d", ue::ObjName(pawnS).c_str(),
                pawnS ? At<FVector>(pawnS, oLocation).x : 0, pawnS ? At<FVector>(pawnS, oLocation).y : 0, pawnS ? At<FVector>(pawnS, oLocation).z : 0,
                At<FRotator>(pc, oRotation).yaw, ue::FullName(lwParent).c_str(), ue::FullName(weap).c_str(), GetBool(pc, g_pcClass, "bInChamber"),
                GetBool(pc, g_pcClass, "bCapturing"));
            if (weap && g_cfg.debugTile) {
                for (const char* comp : {"ProjectedTile", "ProjectedOutline", "HighlightCube", "TakeTile", "ProjectedMaterial", "GuideTile"}) {
                    Obj prop = ue::FindField(ue::ClassOf(weap), comp);
                    if (!prop) continue;
                    Obj v = At<Obj>(weap, At<int>(prop, ue::L.pOffset));
                    char tag[64];
                    snprintf(tag, sizeof(tag), "weapon.%s", comp);
                    dbgdump::DumpObject(v, tag, oBoolMask);
                }
            }
        }
        prevShot = shot;
    }
    static int dumpTicks = 0;
    if (g_cfg.debugLog && ++dumpTicks == 200) dbgdump::Run(pc, g_localPlayer, oBoolMask);
    if (g_cfg.debugTeleport[0] != 0 && dumpTicks == 300) {
        // Development aid: put the player where a diagnostic snapshot was taken.
        Obj pawnT = PawnOf(pc);
        if (pawnT) {
            uint8_t parms[32];
            memset(parms, 0, sizeof(parms));
            *(FVector*)parms = {g_cfg.debugTeleport[0], g_cfg.debugTeleport[1], g_cfg.debugTeleport[2]};
            CallFn(pawnT, fnSetLocation, parms);
            g_playYaw = g_cfg.debugTeleport[3] * 360.0f / 65536.0f;
            g_anchorInit = false;
            g_jumpTick = GetTickCount();
            Log("dbg: teleported to (%.0f %.0f %.0f) yaw %.1f -> now (%.0f %.0f %.0f)", g_cfg.debugTeleport[0], g_cfg.debugTeleport[1],
                g_cfg.debugTeleport[2], g_playYaw, At<FVector>(pawnT, oLocation).x, At<FVector>(pawnT, oLocation).y, At<FVector>(pawnT, oLocation).z);
        }
    }
    if (g_cfg.debugJumpNode >= 0 && dumpTicks == 300) {
        // Development aid: "click" a map node the way the player does (the map wall reacts
        // to the gun/interaction through TakeDamage).
        std::string want = "HazardMapNode_" + std::to_string(g_cfg.debugJumpNode);
        Obj take = ue::Find(g_cfg.debugJumpDelayed ? "Function Hazard.HazardMapNode.DelayedJump" : "Function Hazard.HazardMapNode.TakeDamage");
        int n = ue::NumObjects();
        for (int i = 0; i < n && take; i++) {
            Obj o = ue::ObjAt(i);
            if (o && ue::ObjName(ue::ClassOf(o)) == "HazardMapNode" && ue::ObjName(o) == want) {
                uint8_t parms[256];
                memset(parms, 0, sizeof(parms));
                std::string line;
                for (Obj p = At<Obj>(take, ue::L.sChildren); p; p = At<Obj>(p, ue::L.fNext)) {
                    std::string pn = ue::ObjName(p);
                    int off = At<int>(p, ue::L.pOffset);
                    line += " " + pn + "@" + std::to_string(off);
                    if (off + 12 > (int)sizeof(parms)) continue;
                    if (pn == "DamageAmount" || pn == "Damage") *(int*)(parms + off) = 1;
                    else if (pn == "EventInstigator") *(Obj*)(parms + off) = pc;
                    else if (pn == "DamageCauser") *(Obj*)(parms + off) = PawnOf(pc);
                    else if (pn == "HitLocation") *(FVector*)(parms + off) = At<FVector>(o, oLocation);
                }
                Log("dbg: TakeDamage on %s (parms %d:%s)", want.c_str(), ParmsSize(take), line.c_str());
                g_jumpTick = GetTickCount();
                if (ParmsSize(take) <= (int)sizeof(parms)) CallFn(o, take, parms);
            }
        }
    }
    if (g_cfg.debugLog && g_jumpTick && (dumpTicks % 90) == 0) {
        for (const char* capName : {"WorldCapture", "WireframeCapture", "HoloCapture"}) {
            Obj wc = At<Obj>(pc, ue::PropOffset(ue::ClassOf(pc), capName));
            Obj comp = wc ? At<Obj>(wc, ue::PropOffset(ue::ClassOf(wc), "SceneCapture")) : nullptr;
            if (!comp) continue;
            Obj cc = ue::ClassOf(comp);
            int oVM = ue::PropOffset(cc, "ViewMatrix");
            float* vm = (float*)((uint8_t*)comp + oVM);
            Obj base = At<Obj>(wc, ue::PropOffset(ue::ClassOf(wc), "Base"));
            Log("dbg: %s actor loc (%.1f %.1f %.1f) rot (%d %d %d) base %s fov %.1f enabled %08X VM r3 (%.1f %.1f %.1f) r0 (%.3f %.3f %.3f)", capName,
                At<FVector>(wc, oLocation).x, At<FVector>(wc, oLocation).y, At<FVector>(wc, oLocation).z, At<FRotator>(wc, oRotation).pitch,
                At<FRotator>(wc, oRotation).yaw, At<FRotator>(wc, oRotation).roll, ue::ObjName(base).c_str(), At<float>(comp, ue::PropOffset(cc, "FieldOfView")),
                At<uint32_t>(comp, ue::PropOffset(cc, "bEnabled")), vm[12], vm[13], vm[14], vm[0], vm[1], vm[2]);
        }
        Obj pawnD = PawnOf(pc);
        if (pawnD) Log("dbg: pawn loc (%.1f %.1f %.1f) pc rot (%d %d %d)", At<FVector>(pawnD, oLocation).x, At<FVector>(pawnD, oLocation).y,
                       At<FVector>(pawnD, oLocation).z, At<FRotator>(pc, oRotation).pitch, At<FRotator>(pc, oRotation).yaw, At<FRotator>(pc, oRotation).roll);
        Obj lw = At<Obj>(pc, ue::PropOffset(ue::ClassOf(pc), "LineworkMaterialInstance"));
        if (lw) {
            Obj lc = ue::ClassOf(lw);
            int oTex = ue::PropOffset(lc, "TextureParameterValues"), oSc = ue::PropOffset(lc, "ScalarParameterValues");
            uint8_t* td = At<uint8_t*>(lw, oTex);
            int tn = At<int>(lw, oTex + 4);
            for (int i = 0; i < tn && i < 6; i++) // FTextureParameterValue {FName ParameterName; UTexture* ParameterValue; ...}
                Log("dbg: linework tex %s = %s", ue::NameStr(*(int*)(td + i * 0x18)).c_str(), ue::FullName(*(Obj*)(td + i * 0x18 + 8)).c_str());
            uint8_t* sd = At<uint8_t*>(lw, oSc);
            int sn = At<int>(lw, oSc + 4);
            for (int i = 0; i < sn && i < 10; i++) Log("dbg: linework scalar %s = %g", ue::NameStr(*(int*)(sd + i * 0x18)).c_str(), *(float*)(sd + i * 0x18 + 8));
            Obj parent = At<Obj>(lw, ue::PropOffset(lc, "Parent"));
            Log("dbg: linework parent %s", ue::FullName(parent).c_str());
        }
    }
    if (g_cfg.debugNoLinework) {
        Obj lw = At<Obj>(pc, ue::PropOffset(ue::ClassOf(pc), "Linework"));
        if (lw) SetBool(lw, ue::ClassOf(lw), "bShowInGame", false);
    }
    if (!g_setupDone) OneTimeSetup(pc);
    else if (GetTickCount() - g_lastResFix > 3000) {
        unsigned bw = 0, bh = 0;
        d3dhook::BackBufferSize(bw, bh);
        if (bw && (bw != vr::S.renderW || bh != vr::S.renderH)) {
            Log("back buffer is %ux%u, restoring the headset resolution", bw, bh);
            ApplyResolution(pc);
        }
    }
    Obj pawn = PawnOf(pc);

    const vr::Hand& gun = f.hand[GunHand()];
    const vr::Hand& off = f.hand[1 - GunHand()];
    const vr::Hand& moveHand = f.hand[g_cfg.leftHanded ? 1 : 0];
    const vr::Hand& turnHand = f.hand[g_cfg.leftHanded ? 0 : 1];

    // Turning
    float tx = turnHand.stickX;
    if (g_cfg.snapTurn) {
        if (!g_snapLatch && fabsf(tx) > 0.7f) {
            g_playYaw += (tx > 0 ? 1 : -1) * g_cfg.snapTurnDegrees;
            g_snapLatch = true;
        } else if (g_snapLatch && fabsf(tx) < 0.3f) {
            g_snapLatch = false;
        }
    } else {
        g_playYaw += Deadzone(tx) * g_cfg.smoothTurnSpeed * dt;
    }
    if (g_playYaw > 360) g_playYaw -= 360;
    if (g_playYaw < -360) g_playYaw += 360;

    // Roomscale: physical head movement walks the pawn (with collision).
    if (f.hmdValid) {
        Vec3 h = f.hmd.Pos();
        if (!g_anchorInit) {
            g_anchorX = h.x;
            g_anchorZ = h.z;
            g_anchorInit = true;
        }
        float dx = h.x - g_anchorX, dz = h.z - g_anchorZ;
        if (g_cfg.roomscale && pawn && fnMove && (dx * dx + dz * dz) > 0.002f * 0.002f) {
            Vec3 wd = DirToWorld(Vec3(dx, 0, dz)) * g_cfg.worldScale;
            FVector before = At<FVector>(pawn, oLocation);
            uint8_t parms[32];
            memset(parms, 0, sizeof(parms));
            *(FVector*)parms = {wd.x, wd.y, 0};
            CallFn(pawn, fnMove, parms);
            FVector after = At<FVector>(pawn, oLocation);
            float mx, mz;
            WorldToTracking(after.x - before.x, after.y - before.y, mx, mz);
            g_anchorX += mx;
            g_anchorZ += mz;
            // If a wall blocked the pawn, let the head lean a little but never drift far from the body.
            float rx = h.x - g_anchorX, rz = h.z - g_anchorZ;
            float rl = sqrtf(rx * rx + rz * rz);
            const float kMaxLean = 0.30f;
            if (rl > kMaxLean) {
                g_anchorX = h.x - rx * (kMaxLean / rl);
                g_anchorZ = h.z - rz * (kMaxLean / rl);
            }
        }
    }

    // Locomotion stick, relative to head (default) or hand yaw.
    float sx = Deadzone(moveHand.stickX), sy = Deadzone(moveHand.stickY);
    float mag = sqrtf(sx * sx + sy * sy);
    if (mag > 1) mag = 1;
    if (g_cfg.moveRelativeToHand && moveHand.valid && f.hmdValid) {
        Vec3 hf = DirToWorld(f.hmd.Forward()), cf = DirToWorld(moveHand.aim.Forward());
        float a = atan2f(cf.y, cf.x) - atan2f(hf.y, hf.x), c = cosf(a), s = sinf(a);
        float nx = sx * c + sy * s, ny = -sx * s + sy * c;
        sx = nx;
        sy = ny;
    }
    g_moveX = sx;
    g_moveY = sy;
    if (g_cfg.debugWalkMs > 0) {
        // Development aid: walk forward for N ms, starting 2 s after the scripted jump.
        DWORD since = g_jumpTick ? GetTickCount() - g_jumpTick : 0;
        if (g_jumpTick && since > 2000 && since < 2000 + (DWORD)g_cfg.debugWalkMs) g_moveY = 1.0f;
    }
    if (pawn && g_defaultPawn) {
        float base = At<float>(g_defaultPawn, oPawnGroundSpeed);
        float scale = mag > 0.01f ? (mag < 0.35f ? 0.35f : mag) : 1.0f;
        At<float>(pawn, oPawnGroundSpeed) = base * scale;
    }

    // Buttons
    Edge(gun.triggerBtn, g_prevFire, pc, fnStartFire, fnStopFire);
    Edge(gun.gripBtn, g_prevAlt, pc, fnStartAltFire, fnStopAltFire);
    Edge(off.triggerBtn || gun.bBtn, g_prevTri, pc, fnStartTriFire, fnStopTriFire);
    Edge(gun.aBtn || off.aBtn, g_prevJump, At<Obj>(pc, oPcInput), fnJump, nullptr);
    Edge(off.bBtn, g_prevMenu, pc, fnShowMenu, fnCancelExit);

    if (g_crosshairOk && g_cfg.crosshair) SetBool(pc, g_pcClass, "bNoCrosshair", true);

    if (g_targetsDue && GetTickCount() > g_targetsDue) {
        g_targetsDue = 0;
        if (g_cfg.resizeScreenTargets) RefreshScreenTargets(pc);
    }

    // Linework edge-detector sampling distance: same angle as on a flat 16:9, 90 degree screen.
    if (g_cfg.lineworkFix) {
        static Obj lastMic = nullptr;
        static int countdown = 0;
        Obj mic = At<Obj>(pc, ue::PropOffset(g_pcClass, "LineworkMaterialInstance"));
        if (mic && (mic != lastMic || --countdown <= 0)) {
            lastMic = mic;
            countdown = 300;
            const float flatTanH = 1.0f, flatTanV = 0.5625f;
            shading::ApplyLineSampling(mic, flatTanH / vr::S.tanH * g_cfg.lineworkScale, flatTanV / vr::S.tanV * g_cfg.lineworkScale);
        }
    }

    // Keep Unreal's FOV in sync with the headset frustum.
    At<float>(pc, oPcFOV) = vr::S.fovDeg;
    At<float>(pc, oPcDesiredFOV) = vr::S.fovDeg;
    At<float>(pc, oPcDefaultFOV) = vr::S.fovDeg;

    // Gun mesh sits in the hand instead of the lower-right corner of the screen.
    if (pawn && oWeapViewOffset >= 0) {
        Obj weapon = At<Obj>(pawn, oPawnWeapon);
        if (weapon && gun.valid) At<FVector>(weapon, oWeapViewOffset) = {g_cfg.gunOffsetX, g_cfg.gunOffsetY, g_cfg.gunOffsetZ};
        if (weapon) {
            FixWeaponMesh(weapon);
            if (oWeapRotator >= 0) FixWeaponMesh(At<Obj>(weapon, oWeapRotator));
        }
    }
}

// After the game's own rotation update: the controller looks where the head looks.
static void PostUpdateRotation(Obj pc, float dt) {
    FVector loc;
    FRotator rot;
    int savedEye = g_eye, savedAim = g_aimDepth;
    g_eye = -1;
    g_aimDepth = 0;
    bool ok = CurrentView(pc, loc, rot, false);
    g_eye = savedEye;
    g_aimDepth = savedAim;
    if (!ok) return;
    rot.roll = 0;
    At<FRotator>(pc, oRotation) = rot;
    g_lastYaw = rot.yaw;
    g_haveLastYaw = true;
    Obj pawn = PawnOf(pc);
    if (pawn && fnFaceRotation) {
        uint8_t parms[32];
        memset(parms, 0, sizeof(parms));
        *(FRotator*)parms = rot;
        *(float*)(parms + 12) = dt;
        CallFn(pawn, fnFaceRotation, parms);
    }
}

// --- Aim pointer --------------------------------------------------------------------
// A dot drawn where the gun hand's ray meets the world, projected for each eye
// so it sits at the right stereo depth.

static bool GetBool(Obj obj, Obj cls, const char* name) {
    Obj prop = ue::FindField(cls, name);
    if (!prop || oBoolMask < 0) return false;
    return (At<uint32_t>(obj, At<int>(prop, ue::L.pOffset)) & At<uint32_t>(prop, oBoolMask)) != 0;
}

static void DrawLine(Obj canvas, float x1, float y1, float x2, float y2, uint32_t bgra) {
    uint8_t parms[64];
    memset(parms, 0, sizeof(parms));
    if (g_lineInts) {
        *(int*)(parms + oLnX1) = (int)x1; *(int*)(parms + oLnY1) = (int)y1;
        *(int*)(parms + oLnX2) = (int)x2; *(int*)(parms + oLnY2) = (int)y2;
    } else {
        *(float*)(parms + oLnX1) = x1; *(float*)(parms + oLnY1) = y1;
        *(float*)(parms + oLnX2) = x2; *(float*)(parms + oLnY2) = y2;
    }
    *(uint32_t*)(parms + oLnColor) = bgra;
    CallFn(canvas, fnDraw2DLine, parms);
}

static void DrawAimDot(Obj hud) {
    Obj pc = Pc();
    Obj pawn = PawnOf(pc);
    Vec3 base;
    const vr::Hand& gun = g_frame.hand[GunHand()];
    if (!pawn || !gun.valid || !g_frame.hmdValid || !BasePoint(pawn, base)) return;
    Obj canvas = At<Obj>(hud, oHudCanvas);
    if (!canvas) return;

    if (g_eye == 0 || !g_aimHitValid) {
        FVector start;
        FRotator rot;
        PoseToWorld(gun.aim, base, start, rot);
        Vec3 dir = DirToWorld(gun.aim.Forward());
        uint8_t parms[512];
        memset(parms, 0, sizeof(parms));
        if (ParmsSize(fnTrace) > (int)sizeof(parms)) return;
        *(FVector*)(parms + oTrStart) = start;
        *(FVector*)(parms + oTrEnd) = {start.x + dir.x * 30000.0f, start.y + dir.y * 30000.0f, start.z + dir.z * 30000.0f};
        *(uint32_t*)(parms + oTrActors) = 1;
        CallFn(pawn, fnTrace, parms);
        if (*(Obj*)(parms + oTrRet)) g_aimHit = *(FVector*)(parms + oTrHitLoc);
        else g_aimHit = {start.x + dir.x * 5000.0f, start.y + dir.y * 5000.0f, start.z + dir.z * 5000.0f};
        g_aimHitValid = true;
    }

    Mat34 eye = Mul(g_frame.hmd, vr::S.eye[g_eye].eyeToHead);
    FVector camLoc;
    FRotator camRot;
    PoseToWorld(eye, base, camLoc, camRot);
    Vec3 f = DirToWorld(eye.Forward()), r = DirToWorld(eye.Right()), u = DirToWorld(eye.Up());
    Vec3 v(g_aimHit.x - camLoc.x, g_aimHit.y - camLoc.y, g_aimHit.z - camLoc.z);
    float z = Dot(v, f);
    if (z < 1.0f) return;
    float clipX = At<float>(canvas, oCanvasClipX), clipY = At<float>(canvas, oCanvasClipY);
    if (clipX < 16 || clipY < 16) return;
    float tanH = vr::S.tanH, tanV = tanH * clipY / clipX;
    float sx = (Dot(v, r) / (z * tanH) * 0.5f + 0.5f) * clipX;
    float sy = (0.5f - Dot(v, u) / (z * tanV) * 0.5f) * clipY;
    static int dbg = 0;
    if (dbg++ < 2)
        Log("aimdot: eye %d canvas %p clip %.0fx%.0f hit (%.1f %.1f %.1f) cam (%.1f %.1f %.1f) z %.1f screen %.1f,%.1f", g_eye, canvas, clipX,
            clipY, g_aimHit.x, g_aimHit.y, g_aimHit.z, camLoc.x, camLoc.y, camLoc.z, z, sx, sy);
    int half = (int)(clipY / 500.0f) + 2;
    if (sx < half || sy < half || sx > clipX - half || sy > clipY - half) return;
    bool green = GetBool(hud, g_hudClass, "bGreenCrosshair");
    uint32_t color = green ? 0xFF30FF30 : 0xFFFFFFFF; // BGRA bytes: B, G, R, A
    uint32_t outline = 0xFF000000;
    for (int dy = -half - 1; dy <= half + 1; dy++) {
        bool edge = dy < -half || dy > half;
        float w = edge ? (float)half : (float)(half + 1);
        DrawLine(canvas, sx - w, sy + dy, sx + w + 1, sy + dy, outline);
        if (!edge) DrawLine(canvas, sx - half + 1, sy + dy, sx + half, sy + dy, color);
    }
}

// Draws the captured placement-preview box in the right eye: each 3D edge is projected with
// this eye's camera and drawn with Canvas.Draw2DLine (this engine build has no Draw3DLine).
static void DrawTileOutline(Obj hud) {
    Obj pc = Pc();
    Obj pawn = PawnOf(pc);
    Vec3 base;
    if (!pawn || !g_frame.hmdValid || !BasePoint(pawn, base) || !fnDraw2DLine) return;
    Obj canvas = At<Obj>(hud, oHudCanvas);
    if (!canvas) return;
    float clipX = At<float>(canvas, oCanvasClipX), clipY = At<float>(canvas, oCanvasClipY);
    if (clipX < 16 || clipY < 16) return;
    Mat34 eye = Mul(g_frame.hmd, vr::S.eye[g_eye].eyeToHead);
    FVector camLoc;
    FRotator camRot;
    PoseToWorld(eye, base, camLoc, camRot);
    Vec3 cf = DirToWorld(eye.Forward()), cr = DirToWorld(eye.Right()), cu = DirToWorld(eye.Up());
    float tanH = vr::S.tanH, tanV = tanH * clipY / clipX;
    const float nearZ = 2.0f;
    for (auto& l : g_tileLines) {
        Vec3 a(l.a.x - camLoc.x, l.a.y - camLoc.y, l.a.z - camLoc.z), b(l.b.x - camLoc.x, l.b.y - camLoc.y, l.b.z - camLoc.z);
        float za = Dot(a, cf), zb = Dot(b, cf);
        if (za < nearZ && zb < nearZ) continue;
        // Clip the segment against the near plane.
        if (za < nearZ) { float t = (nearZ - za) / (zb - za); a = a + (b - a) * t; za = nearZ; }
        if (zb < nearZ) { float t = (nearZ - zb) / (za - zb); b = b + (a - b) * t; zb = nearZ; }
        float ax = (Dot(a, cr) / (za * tanH) * 0.5f + 0.5f) * clipX, ay = (0.5f - Dot(a, cu) / (za * tanV) * 0.5f) * clipY;
        float bx = (Dot(b, cr) / (zb * tanH) * 0.5f + 0.5f) * clipX, by = (0.5f - Dot(b, cu) / (zb * tanV) * 0.5f) * clipY;
        DrawLine(canvas, ax, ay, bx, by, l.color);
    }
}

// --- Script interpreter hook ------------------------------------------------------

static void __fastcall hkProcessInternal(void* self, void* edx, uint8_t* stack, void* result) {
    void* node = *(void**)(stack + kFrameNode);
    if (node == fnGPVP) {
        oProcessInternal(self, edx, stack, result);
        FVector loc;
        FRotator rot;
        if (CurrentView((Obj)self, loc, rot, true)) {
            // FOutParmRec list: {Property, PropAddr, Next}. It is not null-terminated, so stop
            // as soon as both out parameters have been written.
            int found = 0;
            uint8_t* rec = *(uint8_t**)(stack + kFrameOutParms);
            for (int i = 0; i < 3 && rec && found < 2; i++, rec = *(uint8_t**)(rec + 8)) {
                Obj prop = *(Obj*)rec;
                uint8_t* addr = *(uint8_t**)(rec + 4);
                if (prop == propOutLoc) { *(FVector*)addr = loc; found++; }
                else if (prop == propOutRot) { *(FRotator*)addr = rot; found++; }
                else break;
            }
        }
        return;
    }
    if (node == fnPCFov || node == fnCamFov) {
        oProcessInternal(self, edx, stack, result);
        if (result && g_haveFrame) *(float*)result = vr::S.fovDeg;
        return;
    }
    if (node == fnPlayerTick) {
        float dt = *(float*)(*(uint8_t**)(stack + kFrameLocals));
        if (self == Pc()) PreTick((Obj)self, dt);
        oProcessInternal(self, edx, stack, result);
        return;
    }
    if (node == fnPlayerInput) {
        Obj pc = Pc();
        if (pc && At<Obj>(pc, oPcInput) == self) {
            At<float>(self, oInBaseY) += g_moveY;
            At<float>(self, oInStrafe) += g_moveX;
        }
        oProcessInternal(self, edx, stack, result);
        return;
    }
    if (node == fnUpdateRotation) {
        float dt = *(float*)(*(uint8_t**)(stack + kFrameLocals));
        oProcessInternal(self, edx, stack, result);
        if (self == Pc()) {
            SyncYaw((Obj)self);
            PostUpdateRotation((Obj)self, dt);
        }
        return;
    }
    if (node == fnPawnViewLoc || node == fnPawnViewRot) {
        oProcessInternal(self, edx, stack, result);
        if (g_aimDepth > 0 && result) {
            Obj pc = Pc();
            FVector loc;
            FRotator rot;
            if (pc && PawnOf(pc) == self && CurrentView(pc, loc, rot, true)) {
                if (node == fnPawnViewLoc) *(FVector*)result = loc;
                else *(FRotator*)result = rot;
            }
        }
        return;
    }
    if (g_eye == 0 && (node == fnActiveOverlays || node == fnInteractTest)) {
        // The HUD re-runs the gun's aiming logic (CalcWeaponFire, which updates the block
        // placement preview) and the sign hover test after each eye's scene. Run during the
        // left eye, those changes reached the right eye's scene before it was drawn, so the
        // preview only showed in one eye. They now run once, after the right eye, exactly
        // where they run on a flat screen.
        return;
    }
    if (node == fnSetPosition || node == fnUpdWeapPos) {
        // The first-person gun and its rotating ring are placed from the controller rotation
        // and the pawn's eye position (the flat-screen camera). Give the script the hand's
        // rotation, then move what it placed from the camera to the hand.
        Obj pc = Pc();
        Obj pawn = PawnOf(pc);
        Vec3 base;
        const vr::Hand& gun = g_frame.hand[GunHand()];
        if (!pc || !pawn || !g_haveFrame || !gun.valid || At<Obj>(pawn, oPawnWeapon) != self || !BasePoint(pawn, base)) {
            g_aimDepth++;
            oProcessInternal(self, edx, stack, result);
            g_aimDepth--;
            return;
        }
        SyncYaw(pc);
        FVector handLoc;
        FRotator handRot;
        PoseToWorld(gun.aim, base, handLoc, handRot);
        FRotator saved = At<FRotator>(pc, oRotation);
        At<FRotator>(pc, oRotation) = handRot;
        g_rotSwapped = true;
        g_aimDepth++;
        oProcessInternal(self, edx, stack, result);
        g_aimDepth--;
        g_rotSwapped = false;
        At<FRotator>(pc, oRotation) = saved;
        FVector pl = At<FVector>(pawn, oLocation);
        FVector cam = {pl.x, pl.y, pl.z + At<float>(pawn, oPawnEye)};
        Vec3 delta(handLoc.x - cam.x, handLoc.y - cam.y, handLoc.z - cam.z);
        auto moveToHand = [&](Obj actor) {
            // Only shift what the script left at the camera, never something already at the hand.
            FVector l = At<FVector>(actor, oLocation);
            Vec3 toCam(l.x - cam.x, l.y - cam.y, l.z - cam.z), toHand(l.x - handLoc.x, l.y - handLoc.y, l.z - handLoc.z);
            if (Dot(toCam, toCam) > Dot(toHand, toHand)) return;
            uint8_t parms[32];
            memset(parms, 0, sizeof(parms));
            *(FVector*)parms = {l.x + delta.x, l.y + delta.y, l.z + delta.z};
            CallFn(actor, fnSetLocation, parms);
        };
        if (node == fnSetPosition) moveToHand((Obj)self);
        Obj ring = oWeapRotator >= 0 ? At<Obj>((Obj)self, oWeapRotator) : nullptr;
        if (ring && ue::IsObj(ring)) moveToHand(ring);
        return;
    }
    if (g_cfg.debugLog && (node == fnWorldCam || node == fnChamberCam)) {
        static DWORD windowStart = 0;
        static int counts[2][3];
        DWORD now = GetTickCount();
        if (!windowStart) windowStart = now;
        counts[node == fnWorldCam ? 0 : 1][g_eye + 1]++;
        if (now - windowStart > 2000) {
            Log("dbg: camera calls/2s: World(tick %d, eye0 %d, eye1 %d) Chamber(tick %d, eye0 %d, eye1 %d) selfIsPc=%d jumped=%d", counts[0][0],
                counts[0][1], counts[0][2], counts[1][0], counts[1][1], counts[1][2], self == Pc(), g_jumpTick != 0);
            memset(counts, 0, sizeof(counts));
            windowStart = now;
        }
    }
    if (g_cfg.debugLog && (node == fnWorldCam || node == fnChamberCam) && self == Pc() && g_eye < 0) {
        static int diffLogs = 0;
            if (diffLogs < 3) {
                // Development aid: which capture actors/components does this call change?
                diffLogs++;
                struct Snap { Obj a, c; uint8_t ab[0x70], cb[0x200]; };
                static std::vector<Snap> snaps;
                snaps.clear();
                int n = ue::NumObjects();
                for (int i = 0; i < n; i++) {
                    Obj o = ue::ObjAt(i);
                    if (!o || ue::ObjName(ue::ClassOf(o)) != "SceneCapture2DActor") continue;
                    Obj comp = At<Obj>(o, ue::PropOffset(ue::ClassOf(o), "SceneCapture"));
                    if (!comp) continue;
                    Snap sn;
                    sn.a = o;
                    sn.c = comp;
                    memcpy(sn.ab, o, sizeof(sn.ab));
                    memcpy(sn.cb, comp, sizeof(sn.cb));
                    snaps.push_back(sn);
                }
                oProcessInternal(self, edx, stack, result);
                for (auto& sn : snaps) {
                    std::string ad, cd;
                    for (int k = 0x40; k < (int)sizeof(sn.ab); k += 4)
                        if (memcmp(sn.ab + k, (uint8_t*)sn.a + k, 4)) ad += " " + std::to_string(k);
                    for (int k = 0x40; k < (int)sizeof(sn.cb); k += 4)
                        if (memcmp(sn.cb + k, (uint8_t*)sn.c + k, 4)) cd += " " + std::to_string(k);
                    if (!ad.empty() || !cd.empty())
                        Log("dbg: %s changed %s: actor[%s ] comp[%s ]", ue::ObjName((Obj)node).c_str(), ue::ObjName(sn.a).c_str(), ad.c_str(), cd.c_str());
                }
                Log("dbg: %s diff done (%d captures)", ue::ObjName((Obj)node).c_str(), (int)snaps.size());
                return; // the function already ran above
            }
    }
    if ((node == fnWorldCam || node == fnChamberCam) && g_cfg.stereoCaptures && self == Pc()) {
        // Antichamber renders much of what you see (the linework, rooms seen through the
        // map or glass) with scene captures placed by these functions from the flat-screen
        // camera, and composites them in screen space. Give them the exact view of the eye
        // being drawn (position, rotation with roll, headset FOV) so they line up per eye.
        uint8_t* parms = *(uint8_t**)(stack + kFrameLocals);
        static int preLogs = 0;
        if (g_cfg.debugLog && g_jumpTick && preLogs < 4 && GetTickCount() - g_jumpTick > 3000) {
            preLogs++;
            FVector l0;
            FRotator r0;
            bool ok = CurrentView((Obj)self, l0, r0, false);
            Obj vt = At<Obj>((Obj)self, oPcViewTarget);
            Log("dbg: capture fn %s eye=%d viewTarget=%s currentView=%d haveFrame=%d hmd=%d", ue::ObjName((Obj)node).c_str(), g_eye,
                ue::FullName(vt).c_str(), ok, g_haveFrame, g_frame.hmdValid);
        }
        FVector loc;
        FRotator rot;
        if (CurrentView((Obj)self, loc, rot, false)) {
            *(FVector*)parms = loc;
            *(FRotator*)(parms + 12) = rot;
            *(float*)(parms + 24) = vr::S.fovDeg;
            if (g_eye == 1 && g_cfg.debugCapOffset != 0) ((FVector*)parms)->z += g_cfg.debugCapOffset;
            static int seenLogs = 0;
            if (g_cfg.debugLog && g_jumpTick && seenLogs < 6 && GetTickCount() - g_jumpTick > 3000) {
                seenLogs++;
                Log("dbg: capture fn %s eye=%d", ue::ObjName((Obj)node).c_str(), g_eye);
            }
            if (g_eye < 0) {
                g_capFn = (Obj)node;
                g_capSelf = (Obj)self;
            }
        }
        oProcessInternal(self, edx, stack, result);
        return;
    }
    if (node == fnDrawHUD) {
        oProcessInternal(self, edx, stack, result);
        if (g_eye >= 0 && g_crosshairOk && g_cfg.crosshair) DrawAimDot((Obj)self);
        if (g_eye == 1 && g_cfg.eyeTileFix && !g_tileLines.empty()) DrawTileOutline((Obj)self);
        return;
    }
    if (g_cfg.debugLog && node) {
        static int camLogs = 0;
        std::string nm = ue::ObjName((Obj)node);
        if (camLogs < 30 && (nm == "DrawCamera" || nm == "UpdateWorldCamera" || nm == "UpdateChamberCamera") &&
            ue::ObjName(ue::OuterOf((Obj)node)) == "HazardPlayerController") {
            camLogs++;
            uint8_t* p = *(uint8_t**)(stack + kFrameLocals);
            FVector l = *(FVector*)p;
            FRotator r = *(FRotator*)(p + 12);
            std::string chain;
            uint8_t* fr = *(uint8_t**)(stack + 0x20);
            for (int i = 0; i < 6 && fr && Readable(fr, 0x28); i++, fr = *(uint8_t**)(fr + 0x20))
                chain += " <- " + ue::ObjName(*(Obj*)(fr + kFrameNode));
            Log("dbg: cam %s eye=%d loc (%.1f %.1f %.1f) rot (%d %d %d) fov %.1f%s", nm.c_str(), g_eye, l.x, l.y, l.z, r.pitch, r.yaw, r.roll,
                *(float*)(p + 24), chain.c_str());
            if (nm == "UpdateWorldCamera" && camLogs > 20) {
                oProcessInternal(self, edx, stack, result);
                Obj wc = At<Obj>(self, ue::PropOffset(ue::ClassOf((Obj)self), "WorldCapture"));
                Obj comp = wc ? At<Obj>(wc, ue::PropOffset(ue::ClassOf(wc), "SceneCapture")) : nullptr;
                if (comp) {
                    Obj cc = ue::ClassOf(comp);
                    Obj rt = At<Obj>(comp, ue::PropOffset(cc, "TextureTarget"));
                    Log("dbg:   after: capture actor loc (%.1f %.1f %.1f) rot (%d %d %d) comp fov %.1f near %.1f rt %s %dx%d", At<FVector>(wc, oLocation).x,
                        At<FVector>(wc, oLocation).y, At<FVector>(wc, oLocation).z, At<FRotator>(wc, oRotation).pitch, At<FRotator>(wc, oRotation).yaw,
                        At<FRotator>(wc, oRotation).roll, At<float>(comp, ue::PropOffset(cc, "FieldOfView")), At<float>(comp, ue::PropOffset(cc, "NearPlane")),
                        ue::FullName(rt).c_str(), rt ? At<int>(rt, ue::PropOffset(ue::ClassOf(rt), "SizeX")) : -1,
                        rt ? At<int>(rt, ue::PropOffset(ue::ClassOf(rt), "SizeY")) : -1);
                }
                return;
            }
        }
    }
    if (node == fnShowTile && g_eye < 0 && g_cfg.eyeTileFix && !g_lineBatchers.empty()) {
        // Find which TArray of which line batcher grows during the call: those are the new lines.
        struct Cand { Obj comp; int off; int num; };
        std::vector<Cand> cands;
        for (Obj lb : g_lineBatchers)
            for (int off = 0x40; off <= 0x300; off += 4) {
                void* p = At<void*>(lb, off);
                int num = At<int>(lb, off + 4), max = At<int>(lb, off + 8);
                if (num < 0 || max < num || max > 200000 || (max > 0 && !Readable(p, 4))) continue;
                cands.push_back({lb, off, num});
            }
        oProcessInternal(self, edx, stack, result);
        static bool logged = false;
        for (auto& c : cands) {
            int now = At<int>(c.comp, c.off + 4);
            int k = now - c.num;
            if (k <= 0 || k > 64) continue;
            uint8_t* data = At<uint8_t*>(c.comp, c.off);
            // FBatchedLine: {FVector Start; FVector End; FLinearColor Color; ...}; stride found by validation.
            int stride = 0;
            for (int st = 40; st <= 64 && !stride; st += 4) {
                if (!Readable(data, now * st)) continue;
                bool ok = true;
                for (int i = c.num; i < now && ok; i++) {
                    float* f = (float*)(data + i * st);
                    for (int j = 0; j < 6; j++)
                        if (!(f[j] > -1e7f && f[j] < 1e7f)) ok = false;
                    float dx = f[3] - f[0], dy = f[4] - f[1], dz = f[5] - f[2];
                    if (dx * dx + dy * dy + dz * dz < 1e-4f) ok = false;
                    for (int j = 6; j < 10; j++)
                        if (!(f[j] >= -0.01f && f[j] <= 4.0f)) ok = false;
                }
                if (ok) stride = st;
            }
            if (!stride) continue;
            for (int i = c.num; i < now; i++) {
                float* f = (float*)(data + i * stride);
                auto ch = [](float v) { int x = (int)(v * 255.0f + 0.5f); return (uint32_t)(x < 0 ? 0 : x > 255 ? 255 : x); };
                uint32_t col = (ch(f[9]) << 24) | (ch(f[6]) << 16) | (ch(f[7]) << 8) | ch(f[8]); // FColor in memory: B G R A
                g_tileLines.push_back({{f[0], f[1], f[2]}, {f[3], f[4], f[5]}, col});
            }
            if (!logged) {
                logged = true;
                float* f = (float*)(data + c.num * stride);
                Log("tilefix: %d lines from %s +0x%X (stride %d), first (%.1f %.1f %.1f)-(%.1f %.1f %.1f) color %.2f %.2f %.2f %.2f", k,
                    ue::FullName(c.comp).c_str(), c.off, stride, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]);
            }
            break;
        }
        static int noLines = 0;
        if (g_tileLines.empty() && noLines++ < 3) Log("tilefix: ShowProjectedTile added no recognisable lines");
        return;
    }
    if (g_cfg.debugTile && g_tileFns.count(node)) {
        static int logged = 0;
        static bool started = false;
        std::string nm = ue::ObjName((Obj)node);
        if (nm == "ShowProjectedTile") started = true;
        if (started && logged < 400) {
            logged++;
            std::string chain;
            uint8_t* fr = *(uint8_t**)(stack + 0x20);
            for (int i = 0; i < 5 && fr && Readable(fr, 0x28); i++, fr = *(uint8_t**)(fr + 0x20))
                chain += " <- " + ue::ObjName(*(Obj*)(fr + kFrameNode));
            Log("tile: %s eye=%d draw=%u%s", nm.c_str(), g_eye, d3dhook::StereoFrames(), chain.c_str());
        }
    }
    if (g_cfg.debugLog && node && g_eye >= 0) {
        // Development aid: script functions that run while an eye is being drawn. Any state
        // they change during the left eye leaks into the right eye's scene.
        static std::unordered_set<void*> seenDraw;
        if (seenDraw.size() < 400 && seenDraw.insert(node).second)
            Log("dbg: draw-time eye%d %s self=%s", g_eye, ue::FullName((Obj)node).c_str(), ue::ObjName((Obj)self).c_str());
    }
    if (g_aimFns.count(node)) {
        g_aimDepth++;
        oProcessInternal(self, edx, stack, result);
        g_aimDepth--;
        return;
    }
    oProcessInternal(self, edx, stack, result);
}

// --- Initialisation -----------------------------------------------------------------

static Obj MustFind(const char* name, bool& ok) {
    Obj o = ue::Find(name);
    if (!o) {
        Log("init: missing %s", name);
        ok = false;
    }
    return o;
}

static int MustOffset(Obj cls, const char* prop, bool& ok) {
    int off = cls ? ue::PropOffset(cls, prop) : -1;
    if (off < 0) ok = false;
    return off;
}

static bool UnderClass(Obj fn, Obj cls) {
    for (Obj o = ue::OuterOf(fn); o; o = ue::OuterOf(o))
        if (o == cls) return true;
    return false;
}

static bool Init() {
    if (!ue::Init(false)) return false;
    bool ok = true;
    fnGPVP = MustFind("Function Engine.PlayerController.GetPlayerViewPoint", ok);
    fnPCFov = MustFind("Function Engine.PlayerController.GetFOVAngle", ok);
    fnCamFov = MustFind("Function Engine.Camera.GetFOVAngle", ok);
    fnPawnViewLoc = MustFind("Function Hazard.HazardPawn.GetPawnViewLocation", ok);
    fnPawnViewRot = MustFind("Function Hazard.HazardPawn.GetViewRotation", ok);
    fnPlayerInput = MustFind("Function Engine.PlayerInput.PlayerInput", ok);
    fnPlayerTick = MustFind("Function Hazard.HazardPlayerController.PlayerTick", ok);
    fnUpdateRotation = MustFind("Function Hazard.HazardPlayerController.UpdateRotation", ok);
    fnMove = MustFind("Function Engine.Actor.Move", ok);
    fnFaceRotation = MustFind("Function Hazard.HazardPawn.FaceRotation", ok);
    fnConsoleCommand = MustFind("Function Engine.PlayerController.ConsoleCommand", ok);
    fnJump = MustFind("Function Engine.PlayerInput.Jump", ok);
    fnStartFire = MustFind("Function Hazard.HazardPlayerController.StartFire", ok);
    fnStopFire = MustFind("Function Hazard.HazardPlayerController.StopFire", ok);
    fnStartAltFire = MustFind("Function Engine.PlayerController.StartAltFire", ok);
    fnStopAltFire = MustFind("Function Engine.PlayerController.StopAltFire", ok);
    fnStartTriFire = MustFind("Function Hazard.HazardPlayerController.StartTriFire", ok);
    fnStopTriFire = MustFind("Function Hazard.HazardPlayerController.StopTriFire", ok);
    fnShowMenu = MustFind("Function Hazard.HazardPlayerController.ShowMenu", ok);
    fnCancelExit = MustFind("Function Hazard.HazardPlayerController.CancelExit", ok);
    fnSetLocation = MustFind("Function Engine.Actor.SetLocation", ok);
    fnSetPosition = ue::Find("Function Hazard.HazardWeap_TileGun.SetPosition");
    fnUpdWeapPos = ue::Find("Function Hazard.HazardWeap_TileGun.UpdateWeaponPosition");
    fnShowTile = ue::Find("Function Hazard.HazardWeap_TileGun.ShowProjectedTile");
    fnDraw3DLine = ue::Find("Function Engine.Canvas.Draw3DLine");
    if (fnDraw3DLine) {
        Obj ps = ue::FindField(fnDraw3DLine, "Start", false), pe = ue::FindField(fnDraw3DLine, "End", false),
            pc3 = ue::FindField(fnDraw3DLine, "LineColor", false);
        if (ps && pe && pc3) {
            o3dStart = At<int>(ps, ue::L.pOffset);
            o3dEnd = At<int>(pe, ue::L.pOffset);
            o3dColor = At<int>(pc3, ue::L.pOffset);
        }
    }
    {
        int nObj = ue::NumObjects();
        for (int i = 0; i < nObj; i++) {
            Obj o = ue::ObjAt(i);
            if (o && ue::ObjName(ue::ClassOf(o)) == "LineBatchComponent" && ue::ObjName(o).compare(0, 9, "Default__") != 0) g_lineBatchers.push_back(o);
        }
    }
    Log("init: tile outline fix: ShowProjectedTile %p, Draw3DLine %p (start %d end %d color %d), %d line batchers", fnShowTile, fnDraw3DLine,
        o3dStart, o3dEnd, o3dColor, (int)g_lineBatchers.size());
    if (!memcmp((void*)kIgnoreOcclusionWrite, kIgnoreOcclusionSig, sizeof(kIgnoreOcclusionSig))) {
        g_ignoreOcclusion = (int*)0x025ADF7C;
        g_savedOcclusion = *g_ignoreOcclusion;
    }
    Log("init: right-eye occlusion fix %s", g_ignoreOcclusion ? "available" : "unavailable (unexpected build)");
    if (!memcmp((void*)kFrameCounterInc, kFrameCounterSig, sizeof(kFrameCounterSig))) g_frameCounter = (uint64_t*)0x024E1968;
    Log("init: per-eye frame advance %s", g_frameCounter ? "available" : "unavailable (unexpected build)");
    fnWorldCam = ue::Find("Function Hazard.HazardPlayerController.UpdateWorldCamera");
    fnChamberCam = ue::Find("Function Hazard.HazardPlayerController.UpdateChamberCamera");
    fnActiveOverlays = ue::Find("Function Hazard.HazardWeap_TileGun.ActiveRenderOverlays");
    fnInteractTest = ue::Find("Function Hazard.HazardPawn.InteractTest");
    Log("init: ring %s, overlay deferral %s", fnUpdWeapPos ? "ok" : "missing", fnActiveOverlays ? "ok" : "missing");
    fnSetFOV = ue::Find("Function UDKBase.UDKSkeletalMeshComponent.SetFOV");
    fnSetScale = ue::Find("Function Engine.PrimitiveComponent.SetScale");
    {
        Obj cMesh = ue::Find("Class UDKBase.UDKSkeletalMeshComponent");
        Obj cTile = ue::Find("Class Hazard.HazardWeap_TileGun");
        if (cMesh) {
            oMeshFOV = ue::PropOffset(cMesh, "FOV");
            oMeshScale = ue::PropOffset(cMesh, "Scale");
        }
        if (cTile) oWeapRotator = ue::PropOffset(cTile, "WeaponRotator");
        Log("init: weapon mesh FOV fix %s", (fnSetFOV && oMeshFOV >= 0) ? "available" : "unavailable");
    }
    if (!ok) return false;
    propOutLoc = ue::FindField(fnGPVP, "out_Location", false);
    propOutRot = ue::FindField(fnGPVP, "out_Rotation", false);

    Obj cPC = MustFind("Class Hazard.HazardPlayerController", ok);
    Obj cPawn = MustFind("Class Hazard.HazardPawn", ok);
    Obj cInput = MustFind("Class Engine.PlayerInput", ok);
    Obj cCyl = MustFind("Class Engine.CylinderComponent", ok);
    Obj cLP = MustFind("Class Engine.LocalPlayer", ok);
    Obj cWeap = MustFind("Class Hazard.HazardWeap_TileGun", ok);
    if (!ok || !propOutLoc || !propOutRot) return false;
    oLocation = MustOffset(cPC, "Location", ok);
    oRotation = MustOffset(cPC, "Rotation", ok);
    oPcPawn = MustOffset(cPC, "Pawn", ok);
    oPcInput = MustOffset(cPC, "PlayerInput", ok);
    oPcFOV = MustOffset(cPC, "FOVAngle", ok);
    oPcDesiredFOV = MustOffset(cPC, "DesiredFOV", ok);
    oPcDefaultFOV = MustOffset(cPC, "DefaultFOV", ok);
    oPcViewTarget = MustOffset(cPC, "ViewTarget", ok);
    oLpActor = MustOffset(cLP, "Actor", ok);
    oPawnWeapon = MustOffset(cPawn, "Weapon", ok);
    oPawnBaseEye = MustOffset(cPawn, "BaseEyeHeight", ok);
    oPawnEye = MustOffset(cPawn, "EyeHeight", ok);
    oPawnCyl = MustOffset(cPawn, "CylinderComponent", ok);
    oPawnGroundSpeed = MustOffset(cPawn, "GroundSpeed", ok);
    oCylHeight = MustOffset(cCyl, "CollisionHeight", ok);
    oCylRadius = MustOffset(cCyl, "CollisionRadius", ok);
    oInBaseY = MustOffset(cInput, "aBaseY", ok);
    oInStrafe = MustOffset(cInput, "aStrafe", ok);
    oWeapViewOffset = ue::PropOffset(cWeap, "PlayerViewOffset");
    if (!ok) return false;

    // UBoolProperty::BitMask offset: consecutive bools of one class have masks 1, 2, 4.
    Obj b0 = ue::FindField(cPC, "bShowingCredits"), b1 = ue::FindField(cPC, "bBlendingCredits"),
        b2 = ue::FindField(cPC, "bChangedTranslationThreshold");
    if (b0 && b1 && b2) {
        for (int k = ue::L.pOffset + 4; k <= 0xB0; k += 4) {
            if (At<uint32_t>(b0, k) == 1 && At<uint32_t>(b1, k) == 2 && At<uint32_t>(b2, k) == 4) {
                oBoolMask = k;
                break;
            }
        }
    }
    Log("init: BoolProperty mask offset 0x%X", oBoolMask);

    // Optional: aim pointer
    g_pcClass = cPC;
    g_hudClass = ue::Find("Class Hazard.HazardHUD");
    fnDrawHUD = ue::Find("Function Hazard.HazardHUD.DrawHUD");
    fnTrace = ue::Find("Function Engine.Actor.Trace");
    fnDraw2DLine = ue::Find("Function Engine.Canvas.Draw2DLine");
    Obj cCanvas = ue::Find("Class Engine.Canvas");
    if (g_hudClass && fnDrawHUD && fnTrace && fnDraw2DLine && cCanvas) {
        bool cok = true;
        oHudCanvas = MustOffset(g_hudClass, "Canvas", cok);
        oCanvasClipX = MustOffset(cCanvas, "ClipX", cok);
        oCanvasClipY = MustOffset(cCanvas, "ClipY", cok);
        oTrHitLoc = MustOffset(fnTrace, "HitLocation", cok);
        oTrHitNormal = MustOffset(fnTrace, "HitNormal", cok);
        oTrEnd = MustOffset(fnTrace, "TraceEnd", cok);
        oTrStart = MustOffset(fnTrace, "TraceStart", cok);
        oTrActors = MustOffset(fnTrace, "bTraceActors", cok);
        oTrRet = MustOffset(fnTrace, "ReturnValue", cok);
        oLnX1 = MustOffset(fnDraw2DLine, "X1", cok);
        oLnY1 = MustOffset(fnDraw2DLine, "Y1", cok);
        oLnX2 = MustOffset(fnDraw2DLine, "X2", cok);
        oLnY2 = MustOffset(fnDraw2DLine, "Y2", cok);
        oLnColor = MustOffset(fnDraw2DLine, "LineColor", cok);
        if (cok) {
            g_lineInts = ue::ObjName(ue::ClassOf(ue::FindField(fnDraw2DLine, "X1", false))) == "IntProperty";
            g_crosshairOk = true;
        }
    }
    Log("init: aim pointer %s (line coords %s)", g_crosshairOk ? "available" : "unavailable", g_lineInts ? "int" : "float");

    // Script functions whose notion of "view" should be the gun hand.
    Obj cEngWeapon = ue::Find("Class Engine.Weapon"), cUdkWeapon = ue::Find("Class UDKBase.UDKWeapon");
    Obj cEngPawn = ue::Find("Class Engine.Pawn");
    const char* pawnAimFns[] = {"Interact", "InteractTest", "InteractGame", "InteractTestGame", "GetWeaponStartTraceLocation",
                                "GetBaseAimRotation", "GetAdjustedAimFor", "StartFire"};
    int n = ue::NumObjects();
    for (int i = 0; i < n; i++) {
        Obj o = ue::ObjAt(i);
        if (!o || ue::ObjName(ue::ClassOf(o)) != "Function") continue;
        if (o == fnGPVP || o == fnPawnViewLoc || o == fnPawnViewRot) continue;
        bool aim = UnderClass(o, cWeap) || (cEngWeapon && UnderClass(o, cEngWeapon)) || (cUdkWeapon && UnderClass(o, cUdkWeapon));
        if (!aim && (UnderClass(o, cPawn) || (cEngPawn && UnderClass(o, cEngPawn)))) {
            std::string nm = ue::ObjName(o);
            for (const char* a : pawnAimFns)
                if (nm == a) aim = true;
        }
        if (aim) g_aimFns.insert(o);
    }
    Log("init: %d aim-scope functions", (int)g_aimFns.size());

    for (int i = 0; i < n; i++) {
        Obj o = ue::ObjAt(i);
        if (!o) continue;
        std::string cn = ue::ObjName(ue::ClassOf(o));
        std::string nm = ue::ObjName(o);
        if (cn == "LocalPlayer" && nm.compare(0, 9, "Default__") != 0) g_localPlayer = o;
        if (cn == "GameEngine" && nm.compare(0, 9, "Default__") != 0) g_engine = o;
        if (cn == "HazardPawn" && nm == "Default__HazardPawn") g_defaultPawn = o;
    }
    Log("init: LocalPlayer=%p Engine=%p", g_localPlayer, g_engine);
    if (!g_localPlayer) return false;

    MH_Initialize();
    if (MH_CreateHook((void*)ue::L.processInternal, (void*)hkProcessInternal, (void**)&oProcessInternal) != MH_OK ||
        MH_EnableHook((void*)ue::L.processInternal) != MH_OK) {
        Log("init: failed to hook ProcessInternal");
        return false;
    }
    Log("init: game hooks installed, fov %.1f", vr::S.fovDeg);
    shading::Init(CallFn);
    if (g_cfg.debugTile) {
        int nObj = ue::NumObjects();
        for (int i = 0; i < nObj; i++) {
            Obj o = ue::ObjAt(i);
            if (!o || ue::ObjName(ue::ClassOf(o)) != "Function") continue;
            std::string fnm = ue::ObjName(o), owner = ue::ObjName(ue::OuterOf(o));
            if (owner.find("Hazard") == std::string::npos) continue;
            if (fnm.find("Tile") != std::string::npos || fnm.find("Projected") != std::string::npos || fnm.find("Outline") != std::string::npos ||
                fnm.find("Highlight") != std::string::npos || fnm == "ActiveRenderOverlays" || fnm == "DrawWeaponCrosshair")
                g_tileFns.insert(o);
        }
        Log("tile: watching %d functions", (int)g_tileFns.size());
        dbgdump::XrefTile();
    }
    return true;
}

// --- Stereo draw -----------------------------------------------------------------------

static void __fastcall hkViewportDraw(void* self, void* edx, int bShouldPresent) {
    if (!g_ready && !g_initFailed && ++g_draws >= 90) {
        // Retry until the world and the local player exist.
        g_draws = 0;
        static int attempts = 0;
        if (Init()) g_ready = true;
        else if (++attempts > 60) {
            g_initFailed = true;
            Log("init: giving up, the game stays in flat mode");
        }
    }
    if (!g_ready || !bShouldPresent || !d3dhook::StereoReady() || !g_haveFrame || !Pc()) {
        if (bShouldPresent) d3dhook::NoteDraw();
        oViewportDraw(self, edx, bShouldPresent);
        return;
    }
    vr::Frame f;
    vr::GetFrame(f);
    if (f.hmdValid) g_frame = f;
    Mat34 pose = g_frame.hmd;
    for (int eye = 0; eye < 2; eye++) {
        g_eye = eye;
        if (eye == 1 && g_frameCounter && g_cfg.eyeFrameAdvance) {
            // The engine does some work once per frame: scene captures (rooms seen through glass,
            // the linework) and some per-frame geometry such as the block placement outline. The
            // right eye is drawn in the same engine frame as the left one, so it reused or skipped
            // that work. Advancing the frame counter makes the right eye a frame of its own.
            ++*g_frameCounter;
        }
        if (g_ignoreOcclusion && g_cfg.eyeOcclusionFix) {
            // The right eye is drawn right after the left one, within the same game frame,
            // and inherits its occlusion queries. Things the game re-creates every frame (the
            // block placement preview) are new to the left eye, so always drawn, but get culled
            // in the right eye by the left eye's query. Ignore occlusion for the right eye.
            *g_ignoreOcclusion = eye == 1 ? 1 : g_savedOcclusion;
        }
        if (g_capFn && g_capSelf == Pc() && g_cfg.stereoCaptures) {
            uint8_t parms[128];
            memset(parms, 0, sizeof(parms));
            CallFn(g_capSelf, g_capFn, parms); // parameters are filled in by the hook for this eye
        }
        d3dhook::PushEye(eye, pose, d3dhook::NoteDraw());
        oViewportDraw(self, edx, bShouldPresent);
    }
    g_eye = -1;
    if (g_ignoreOcclusion && g_cfg.eyeOcclusionFix) *g_ignoreOcclusion = g_savedOcclusion;
}

void InstallDrawHook() {
    static bool done = false;
    if (done) return;
    done = true;
    if (memcmp((void*)kViewportDraw, kViewportDrawSig, sizeof(kViewportDrawSig)) != 0) {
        Log("FViewport::Draw signature mismatch: unsupported game build, VR rendering disabled");
        return;
    }
    MH_Initialize();
    MH_STATUS s = MH_CreateHook((void*)kViewportDraw, (void*)hkViewportDraw, (void**)&oViewportDraw);
    MH_STATUS e = MH_EnableHook((void*)kViewportDraw);
    Log("FViewport::Draw hook: create=%d enable=%d", s, e);
}

} // namespace game
