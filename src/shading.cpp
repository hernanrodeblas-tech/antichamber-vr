// Antichamber shades surfaces with a hatch pattern drawn in screen space. In a headset
// that pattern sits in the same place in both eyes and turns with the head. This module
// finds the materials that make up the game's line/shading look and can replace their
// texture parameters with a plain white texture (toggled live, F10 / left stick click).
#include <cstring>
#include <string>
#include <vector>
#include "log.h"
#include "ue3.h"

namespace shading {

using ue::At;
using ue::Obj;

static void (*g_callFn)(Obj, Obj, void*) = nullptr;
static Obj g_white = nullptr, fnSetTex = nullptr, fnSetScalar = nullptr;
static int oTexParams = -1, oScalarParams = -1, oParent = -1;
static int tpSize = 0, tpName = -1, tpValue = -1, spSize = 0, spName = -1, spValue = -1;

struct Swap {
    Obj mic;
    int nameIndex, nameNumber;
    Obj original;
};
static std::vector<Swap> g_swaps;
static bool g_active = false;

static bool Interesting(const std::string& s) {
    std::string l = s;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    return l.find("linework") != std::string::npos || l.find("shad") != std::string::npos || l.find("hatch") != std::string::npos ||
           l.find("sketch") != std::string::npos || l.find("screenmaterial") != std::string::npos;
}

static bool ChainInteresting(Obj mat) {
    for (int i = 0; mat && i < 8; i++) {
        if (Interesting(ue::FullName(mat))) return true;
        if (oParent < 0 || ue::ObjName(ue::ClassOf(mat)) == "Material") break;
        mat = At<Obj>(mat, oParent);
    }
    return false;
}

static int StructSize(const char* full) {
    Obj s = ue::Find(full);
    return s ? At<int>(s, 0x50) : 0;
}

void Init(void (*callFn)(Obj, Obj, void*)) {
    g_callFn = callFn;
    Obj cMic = ue::Find("Class Engine.MaterialInstanceConstant");
    Obj cMi = ue::Find("Class Engine.MaterialInstance");
    if (!cMic || !cMi) {
        Log("shading: material classes not found");
        return;
    }
    oTexParams = ue::PropOffset(cMic, "TextureParameterValues");
    oScalarParams = ue::PropOffset(cMic, "ScalarParameterValues");
    oParent = ue::PropOffset(cMi, "Parent");
    Obj tps = ue::Find("ScriptStruct Engine.MaterialInstanceConstant.TextureParameterValue");
    Obj sps = ue::Find("ScriptStruct Engine.MaterialInstanceConstant.ScalarParameterValue");
    tpSize = StructSize("ScriptStruct Engine.MaterialInstanceConstant.TextureParameterValue");
    spSize = StructSize("ScriptStruct Engine.MaterialInstanceConstant.ScalarParameterValue");
    if (tps) { tpName = ue::PropOffset(tps, "ParameterName"); tpValue = ue::PropOffset(tps, "ParameterValue"); }
    if (sps) { spName = ue::PropOffset(sps, "ParameterName"); spValue = ue::PropOffset(sps, "ParameterValue"); }
    fnSetTex = ue::Find("Function Engine.MaterialInstanceConstant.SetTextureParameterValue");
    g_white = ue::Find("Texture2D EngineResources.WhiteSquareTexture");
    Log("shading: tex param size %d (name %d value %d), scalar size %d, SetTextureParameterValue %p, white %p", tpSize, tpName, tpValue, spSize,
        fnSetTex, g_white);

    // Log every loaded material whose name or parent chain looks like part of the line/shading look.
    int n = ue::NumObjects(), logged = 0;
    for (int i = 0; i < n && logged < 120; i++) {
        Obj o = ue::ObjAt(i);
        if (!o) continue;
        std::string cn = ue::ObjName(ue::ClassOf(o));
        bool isMat = cn == "Material" || cn == "MaterialInstanceConstant";
        bool isTex = cn == "Texture2D";
        if (!isMat && !isTex) continue;
        std::string full = ue::FullName(o);
        if (full.find("Default__") != std::string::npos) continue;
        if (isTex) {
            if (Interesting(full)) { Log("shading: texture %s", full.c_str()); logged++; }
            continue;
        }
        if (!ChainInteresting(o)) continue;
        std::string line = full;
        if (cn == "MaterialInstanceConstant") {
            Obj parent = At<Obj>(o, oParent);
            line += " parent=" + ue::FullName(parent);
            uint8_t* td = At<uint8_t*>(o, oTexParams);
            int tn = At<int>(o, oTexParams + 4);
            for (int k = 0; td && tpSize > 0 && tpName >= 0 && k < tn && k < 12; k++)
                line += " tex[" + ue::NameStr(*(int*)(td + k * tpSize + tpName)) + "=" + ue::FullName(*(Obj*)(td + k * tpSize + tpValue)) + "]";
            uint8_t* sd = At<uint8_t*>(o, oScalarParams);
            int sn = At<int>(o, oScalarParams + 4);
            for (int k = 0; sd && spSize > 0 && spName >= 0 && k < sn && k < 12; k++)
                line += " scalar[" + ue::NameStr(*(int*)(sd + k * spSize + spName)) + "=" + std::to_string(*(float*)(sd + k * spSize + spValue)) + "]";
        }
        Log("shading: material %s", line.c_str());
        logged++;
    }
}

static void SetTex(Obj mic, int nameIndex, int nameNumber, Obj tex) {
    uint8_t parms[64];
    memset(parms, 0, sizeof(parms));
    *(int*)parms = nameIndex; // FName ParameterName
    *(int*)(parms + 4) = nameNumber;
    *(Obj*)(parms + 8) = tex;
    g_callFn(mic, fnSetTex, parms);
}

void Toggle() {
    if (!fnSetTex || !g_white || tpSize <= 0 || tpName < 0 || !g_callFn) {
        Log("shading: toggle unavailable");
        return;
    }
    if (g_active) {
        for (auto& s : g_swaps) SetTex(s.mic, s.nameIndex, s.nameNumber, s.original);
        Log("shading: restored %d texture parameters", (int)g_swaps.size());
        g_swaps.clear();
        g_active = false;
        return;
    }
    int n = ue::NumObjects();
    for (int i = 0; i < n; i++) {
        Obj o = ue::ObjAt(i);
        if (!o || ue::ObjName(ue::ClassOf(o)) != "MaterialInstanceConstant") continue;
        if (ue::ObjName(o).compare(0, 9, "Default__") == 0 || !ChainInteresting(o)) continue;
        uint8_t* td = At<uint8_t*>(o, oTexParams);
        int tn = At<int>(o, oTexParams + 4);
        for (int k = 0; td && k < tn && k < 32; k++) {
            uint8_t* e = td + k * tpSize;
            Obj tex = *(Obj*)(e + tpValue);
            if (!tex || tex == g_white) continue;
            g_swaps.push_back({o, *(int*)(e + tpName), *(int*)(e + tpName + 4), tex});
            Log("shading: %s tex %s (%s) -> white", ue::FullName(o).c_str(), ue::NameStr(*(int*)(e + tpName)).c_str(), ue::FullName(tex).c_str());
        }
    }
    for (auto& s : g_swaps) SetTex(s.mic, s.nameIndex, s.nameNumber, g_white);
    Log("shading: replaced %d texture parameters with white", (int)g_swaps.size());
    g_active = true;
}

// --- Linework sampling distance ---------------------------------------------------------
// The game's lines (outlines and shading) come from a post-process edge detector that
// compares the depth of each pixel with neighbours a fixed distance away in screen UV
// (ThinHorizontalThickness / ThinVerticalThickness). Tuned for a flat 16:9 view at 90
// degrees; in a headset the same UV distance spans a much wider angle, so sloped surfaces
// cross the depth threshold and turn into bands of iso-depth lines. Scale the distances so
// they cover the same angle as on a flat screen.
static Obj fnGetScalar = nullptr;
struct LineBase { Obj mic; float h, v; };
static std::vector<LineBase> g_lineBases;

static bool GetScalar(Obj mat, const char* name, float& out) {
    if (!fnGetScalar) return false;
    Obj pn = ue::FindField(fnGetScalar, "ParameterName", false), pv = ue::FindField(fnGetScalar, "OutValue", false),
        pr = ue::FindField(fnGetScalar, "ReturnValue", false);
    if (!pn || !pv || !pr) return false;
    int nameIdx = -1;
    for (int i = 0; i < 60000 && nameIdx < 0; i++)
        if (ue::NameStr(i) == name) nameIdx = i;
    if (nameIdx < 0) return false;
    uint8_t parms[64];
    memset(parms, 0, sizeof(parms));
    *(int*)(parms + At<int>(pn, ue::L.pOffset)) = nameIdx;
    g_callFn(mat, fnGetScalar, parms);
    if (!*(uint32_t*)(parms + At<int>(pr, ue::L.pOffset))) return false;
    out = *(float*)(parms + At<int>(pv, ue::L.pOffset));
    return true;
}

static void SetScalar(Obj mic, const char* name, float value) {
    Obj pn = ue::FindField(fnSetScalar, "ParameterName", false), pv = ue::FindField(fnSetScalar, "Value", false);
    if (!pn || !pv) return;
    static std::vector<std::pair<std::string, int>> cache;
    int nameIdx = -1;
    for (auto& c : cache)
        if (c.first == name) nameIdx = c.second;
    for (int i = 0; i < 60000 && nameIdx < 0; i++)
        if (ue::NameStr(i) == name) { nameIdx = i; cache.push_back({name, i}); }
    if (nameIdx < 0) return;
    uint8_t parms[64];
    memset(parms, 0, sizeof(parms));
    *(int*)(parms + At<int>(pn, ue::L.pOffset)) = nameIdx;
    *(float*)(parms + At<int>(pv, ue::L.pOffset)) = value;
    g_callFn(mic, fnSetScalar, parms);
}

// sx / sy: factors that map the flat-screen UV distance to the same angle in the headset view.
void ApplyLineSampling(Obj activeMic, float sx, float sy) {
    if (!g_callFn) return;
    if (!fnSetScalar) fnSetScalar = ue::Find("Function Engine.MaterialInstanceConstant.SetScalarParameterValue");
    if (!fnGetScalar) fnGetScalar = ue::Find("Function Engine.MaterialInterface.GetScalarParameterValue");
    if (!fnSetScalar || !fnGetScalar || !activeMic) return;
    std::vector<Obj> targets;
    targets.push_back(activeMic);
    int n = ue::NumObjects();
    static bool scanned = false;
    static std::vector<Obj> extra;
    if (!scanned) {
        scanned = true;
        for (int i = 0; i < n; i++) {
            Obj o = ue::ObjAt(i);
            if (!o || ue::ObjName(ue::ClassOf(o)) != "MaterialInstanceConstant" || ue::ObjName(o).compare(0, 9, "Default__") == 0) continue;
            std::string full = ue::FullName(o);
            if (full.find("ScreenMaterial") != std::string::npos || full.find("Linework") != std::string::npos) extra.push_back(o);
        }
    }
    for (Obj o : extra) targets.push_back(o);
    for (Obj mic : targets) {
        bool known = false;
        for (auto& b : g_lineBases)
            if (b.mic == mic) known = true;
        if (known) continue;
        float h = 0, v = 0;
        bool hasH = GetScalar(mic, "ThinHorizontalThickness", h), hasV = GetScalar(mic, "ThinVerticalThickness", v);
        if (!hasH || !hasV || h <= 0 || v <= 0) {
            g_lineBases.push_back({mic, 0, 0}); // not a linework material; remember so it is not queried again
            continue;
        }
        g_lineBases.push_back({mic, h, v});
        SetScalar(mic, "ThinHorizontalThickness", h * sx);
        SetScalar(mic, "ThinVerticalThickness", v * sy);
        Log("linework: %s thickness H %.5f -> %.5f, V %.5f -> %.5f", ue::FullName(mic).c_str(), h, h * sx, v, v * sy);
    }
}

} // namespace shading
