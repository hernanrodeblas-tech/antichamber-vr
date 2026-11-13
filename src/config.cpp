#include "config.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

Config g_cfg;

static bool ParseBool(const char* v) {
    return !_stricmp(v, "1") || !_stricmp(v, "true") || !_stricmp(v, "yes") || !_stricmp(v, "on");
}

void LoadConfig() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    wcscat(path, L"antichamber_vr.ini");
    FILE* f = _wfopen(path, L"r");
    if (!f) {
        Log("config: antichamber_vr.ini not found, using defaults");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == ';' || *p == '#' || *p == '[' || !*p) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* key = p;
        char* val = eq + 1;
        char* e = key + strlen(key);
        while (e > key && isspace((unsigned char)e[-1])) *--e = 0;
        while (isspace((unsigned char)*val)) val++;
        e = val + strlen(val);
        while (e > val && isspace((unsigned char)e[-1])) *--e = 0;
        char* cm = strchr(val, ';');
        if (cm) {
            *cm = 0;
            e = val + strlen(val);
            while (e > val && isspace((unsigned char)e[-1])) *--e = 0;
        }
#define KB(name, field) else if (!_stricmp(key, name)) g_cfg.field = ParseBool(val)
#define KF(name, field) else if (!_stricmp(key, name)) g_cfg.field = (float)atof(val)
#define KI(name, field) else if (!_stricmp(key, name)) g_cfg.field = atoi(val)
        if (0) {}
        KB("Enabled", enabled);
        KB("FakeHMD", fakeHmd);
        KF("WorldScale", worldScale);
        KF("HeightOffset", heightOffset);
        else if (!_stricmp(key, "PlayMode")) g_cfg.seated = !_stricmp(val, "Seated") || !_stricmp(val, "Sentado");
        KF("SeatedEyeHeight", seatedEyeHeight);
        KF("ResolutionScale", resolutionScale);
        KI("MaxRenderWidth", maxRenderWidth);
        KB("SnapTurn", snapTurn);
        KF("SnapTurnDegrees", snapTurnDegrees);
        KF("SmoothTurnSpeed", smoothTurnSpeed);
        KB("MoveRelativeToHand", moveRelativeToHand);
        KB("LeftHanded", leftHanded);
        KF("StickDeadzone", stickDeadzone);
        KF("AimPitchOffset", aimPitchOffset);
        KF("GunOffsetX", gunOffsetX);
        KF("GunOffsetY", gunOffsetY);
        KF("GunOffsetZ", gunOffsetZ);
        KF("GunScale", gunScale);
        KB("Roomscale", roomscale);
        KF("NearClip", nearClip);
        KB("Crosshair", crosshair);
        KB("StereoCaptures", stereoCaptures);
        KB("EyeOcclusionFix", eyeOcclusionFix);
        KB("OcclusionQueries", occlusionQueries);
        KB("AsymmetricFrustum", asymmetricFrustum);
        KB("EyeFrameAdvance", eyeFrameAdvance);
        KB("EyeTileFix", eyeTileFix);
        KB("DynamicShadows", dynamicShadows);
        KB("MotionBlur", motionBlur);
        else if (!_stricmp(key, "EngineSettings")) { strncpy(g_cfg.engineSettings, val, sizeof(g_cfg.engineSettings) - 1); }
        else if (!_stricmp(key, "CpuCores")) { strncpy(g_cfg.cpuCores, val, sizeof(g_cfg.cpuCores) - 1); }
        KI("RenderThreadCore", renderThreadCore);
        KB("LineworkFix", lineworkFix);
        KB("ResizeScreenTargets", resizeScreenTargets);
        KF("CaptureScale", captureScale);
        KF("LineworkScale", lineworkScale);
        KB("FixPhysX", fixPhysX);
        KI("D3DBridge", d3dBridge);
        KB("PipelineFrames", pipelineFrames);
        KI("DebugDumpFrame", debugDumpFrame);
        KB("DebugLog", debugLog);
        KB("DebugTile", debugTile);
        KB("DebugFireTrace", debugFireTrace);
        KB("DebugProfile", debugProfile);
        KB("DebugProjection", debugProjection);
        KB("DebugEvents", debugEvents);
        KI("DebugJumpNode", debugJumpNode);
        KF("DebugFakeYaw", debugFakeYaw);
        KF("DebugFakePitch", debugFakePitch);
        KI("DebugWalkMs", debugWalkMs);
        KB("DebugNoLinework", debugNoLinework);
        KF("DebugCapOffset", debugCapOffset);
        KB("DebugJumpDelayed", debugJumpDelayed);
        else if (!_stricmp(key, "DebugTeleport")) sscanf(val, "%f %f %f %f", &g_cfg.debugTeleport[0], &g_cfg.debugTeleport[1], &g_cfg.debugTeleport[2], &g_cfg.debugTeleport[3]);
        else if (!_stricmp(key, "Language")) {} // launcher setting
        else Log("config: unknown key '%s'", key);
    }
    fclose(f);
    Log("config: enabled=%d fake=%d worldScale=%.1f resScale=%.2f snap=%d(%.0f) leftHanded=%d roomscale=%d", g_cfg.enabled,
        g_cfg.fakeHmd, g_cfg.worldScale, g_cfg.resolutionScale, g_cfg.snapTurn, g_cfg.snapTurnDegrees, g_cfg.leftHanded,
        g_cfg.roomscale);
}
