// Development aid (DebugLog=1): dumps the objects that shape the final image —
// scene captures, post-process chains and their effects — so per-eye problems can be
// traced to the piece of the renderer that causes them.
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include "log.h"
#include "ue3.h"

namespace dbgdump {

using ue::At;
using ue::Obj;

static int g_boolMaskOff = -1;

static std::string Value(Obj o, Obj prop) {
    std::string pc = ue::ObjName(ue::ClassOf(prop));
    int off = At<int>(prop, ue::L.pOffset);
    char buf[256];
    if (pc == "ObjectProperty" || pc == "ComponentProperty" || pc == "ClassProperty") {
        Obj v = At<Obj>(o, off);
        return v && ue::IsObj(v) ? ue::FullName(v) : "null";
    }
    if (pc == "FloatProperty") { snprintf(buf, sizeof(buf), "%g", At<float>(o, off)); return buf; }
    if (pc == "IntProperty") { snprintf(buf, sizeof(buf), "%d", At<int>(o, off)); return buf; }
    if (pc == "ByteProperty") { snprintf(buf, sizeof(buf), "%u", At<uint8_t>(o, off)); return buf; }
    if (pc == "NameProperty") return ue::NameStr(At<int>(o, off));
    if (pc == "BoolProperty") {
        uint32_t mask = g_boolMaskOff >= 0 ? At<uint32_t>(prop, g_boolMaskOff) : 0;
        return (At<uint32_t>(o, off) & mask) ? "true" : "false";
    }
    if (pc == "StructProperty") {
        snprintf(buf, sizeof(buf), "(%g %g %g %g)", At<float>(o, off), At<float>(o, off + 4), At<float>(o, off + 8), At<float>(o, off + 12));
        return buf;
    }
    if (pc == "ArrayProperty") {
        Obj* data = At<Obj*>(o, off);
        int num = At<int>(o, off + 4);
        snprintf(buf, sizeof(buf), "num=%d", num);
        std::string s = buf;
        for (int i = 0; i < num && i < 6 && data && Readable(data, (i + 1) * 4); i++)
            if (ue::IsObj(data[i])) s += " [" + ue::FullName(data[i]) + "]";
        return s;
    }
    return "?" + pc;
}

static void Dump(Obj o, const char* tag) {
    if (!o || !ue::IsObj(o)) return;
    static std::unordered_set<void*> done;
    if (!done.insert(o).second) return;
    Log("DUMP[%s] %s", tag, ue::FullName(o).c_str());
    for (Obj c = ue::ClassOf(o); c; c = ue::SuperOf(c)) {
        std::string cn = ue::ObjName(c);
        if (cn == "Object" || cn == "Actor") break;
        for (Obj p = At<Obj>(c, ue::L.sChildren); p; p = At<Obj>(p, ue::L.fNext)) {
            std::string pc = ue::ObjName(ue::ClassOf(p));
            if (pc == "Function" || pc == "State" || pc == "Enum" || pc == "Const" || pc == "ScriptStruct") continue;
            if (pc == "DelegateProperty" || pc == "StrProperty" || pc == "MapProperty" || pc == "InterfaceProperty") continue;
            Log("    %s.%s = %s", cn.c_str(), ue::ObjName(p).c_str(), Value(o, p).c_str());
        }
    }
}

// Lists every object a script function's bytecode references (functions it calls,
// properties it touches, classes, materials), in order, deduplicated. UStruct::Script
// is a TArray<BYTE> at +0x54; cooked bytecode stores resolved UObject pointers inline.
static void Xref(const char* fullName) {
    Obj fn = ue::Find(fullName);
    if (!fn) { Log("XREF %s: not found", fullName); return; }
    uint8_t* code = At<uint8_t*>(fn, 0x54);
    int len = At<int>(fn, 0x58);
    if (!code || len <= 0 || len > 200000 || !Readable(code, len)) { Log("XREF %s: no script (len %d)", fullName, len); return; }
    std::string out;
    std::unordered_set<void*> seen;
    for (int i = 0; i + 4 <= len; i++) {
        Obj o = *(Obj*)(code + i);
        if (!ue::IsObj(o) || !seen.insert(o).second) continue;
        std::string cn = ue::ObjName(ue::ClassOf(o));
        std::string nm = ue::ObjName(o);
        Obj outer = ue::OuterOf(o);
        std::string on = outer ? ue::ObjName(outer) : "";
        out += " " + cn.substr(0, 4) + ":" + on + "." + nm;
        i += 3;
    }
    Log("XREF %s (%d bytes):%s", fullName, len, out.c_str());
}

// Dumps all fields of an object (no de-duplication), for on-demand diagnostics.
void DumpObject(Obj o, const char* tag, int boolMaskOff) {
    g_boolMaskOff = boolMaskOff;
    if (!o || !ue::IsObj(o)) { Log("DUMP[%s] null", tag); return; }
    Log("DUMP[%s] %s", tag, ue::FullName(o).c_str());
    for (Obj c = ue::ClassOf(o); c; c = ue::SuperOf(c)) {
        std::string cn = ue::ObjName(c);
        if (cn == "Object" || cn == "Actor" || cn == "ActorComponent" || cn == "Component") break;
        for (Obj p = At<Obj>(c, ue::L.sChildren); p; p = At<Obj>(p, ue::L.fNext)) {
            std::string pc = ue::ObjName(ue::ClassOf(p));
            if (pc == "Function" || pc == "State" || pc == "Enum" || pc == "Const" || pc == "ScriptStruct") continue;
            if (pc == "DelegateProperty" || pc == "StrProperty" || pc == "MapProperty" || pc == "InterfaceProperty") continue;
            std::string pn = ue::ObjName(p);
            // Keep it short: rendering-relevant fields only.
            static const char* keys[] = {"Hidden", "Material", "DepthPriority", "Translation", "Scale", "Rotation", "Attached", "Owner",
                                         "StaticMesh", "Color", "Translucen", "Only", "Cast", "Visible", "Location", "Size", "Tile", "Outline", "LineThickness"};
            bool want = false;
            for (const char* k : keys)
                if (pn.find(k) != std::string::npos) want = true;
            if (want) Log("    %s.%s = %s", cn.c_str(), pn.c_str(), Value(o, p).c_str());
        }
    }
}

void XrefTile() {
    for (const char* f : {"Function Hazard.HazardWeap_TileGun.ProjectTile", "Function Hazard.HazardWeap_TileGun.ShowProjectedTile",
                          "Function Hazard.HazardWeap_TileGun.HideProjectedTile", "Function Hazard.HazardWeap_TileGun.ActiveRenderOverlays",
                          "Function Hazard.HazardWeap_TileGun.DrawWeaponCrosshair", "Function Hazard.HazardWeap_TileGun.CalcWeaponFire",
                          "Function Hazard.HazardWeap_TileGun.Tick", "Function Hazard.HazardWeap_TileGun.PostBeginPlay",
                          "Function Hazard.HazardPlayerController.DrawHUD", "Function Hazard.HazardHUD.DrawHUD", "Function Hazard.HazardPawn.CalcCamera"})
        Xref(f);
}

void Run(Obj pc, Obj localPlayer, int boolMaskOff) {
    g_boolMaskOff = boolMaskOff;
    for (const char* f : {"Function Hazard.HazardWeap_TileGun.ShowProjectedTile", "Function Hazard.HazardWeap_TileGun.HideProjectedTile",
                          "Function Hazard.HazardWeap_TileGun.ProjectTile", "Function Hazard.HazardWeap_TileGun.ActiveRenderOverlays",
                          "Function Hazard.HazardWeap_TileGun.DrawWeaponCrosshair", "Function Hazard.HazardHUD.DrawHUD",
                          "Function Hazard.HazardPlayerController.DrawHUD", "Function Hazard.HazardPlayerController.DrawCamera",
                          "Function Hazard.HazardPlayerController.UpdateWorldCamera", "Function Hazard.HazardPlayerController.SetupScreen",
                          "Function Hazard.HazardPlayerController.UpdateScreenMaterialResolutions", "Function Hazard.HazardPlayerController.PlayerTick",
                          "Function Hazard.HazardWeap_TileGun.Tick", "Function Hazard.HazardWeap_TileGun.UpdateWeaponPosition"})
        Xref(f);
    Log("DUMP begin");
    for (const char* fnName : {"Function Hazard.HazardPlayerController.HoloJump", "Function Hazard.HazardPlayerController.NotifyChamberTransition",
                               "Function Hazard.HazardGame.ActivateMap", "Function Hazard.HazardSeqAct_InitiateJump.Activated"}) {
        Obj f = ue::Find(fnName);
        std::string line;
        if (f)
            for (Obj p = At<Obj>(f, ue::L.sChildren); p; p = At<Obj>(p, ue::L.fNext))
                line += " " + ue::ObjName(ue::ClassOf(p)) + ":" + ue::ObjName(p) + "@" + std::to_string(At<int>(p, ue::L.pOffset));
        Log("FN %s:%s", fnName, line.c_str());
    }
    {
        Obj cls = ue::Find("Class Hazard.HazardMapNode");
        std::string fields;
        for (Obj p = cls ? At<Obj>(cls, ue::L.sChildren) : nullptr; p; p = At<Obj>(p, ue::L.fNext))
            fields += " " + ue::ObjName(ue::ClassOf(p)).substr(0, 4) + ":" + ue::ObjName(p);
        Log("CLASS HazardMapNode:%s", fields.c_str());
        int n = ue::NumObjects(), shown = 0;
        for (int i = 0; i < n && shown < 4; i++) {
            Obj o = ue::ObjAt(i);
            if (o && ue::ClassOf(o) == cls && ue::ObjName(o).compare(0, 9, "Default__") != 0) {
                shown++;
                Dump(o, "MapNode");
            }
        }
    }
    // Controller fields that reference the renderer pieces Antichamber adds.
    for (const char* name : {"WorldCapture", "UnlitCapture", "WireframeCapture", "UnlitPostProcess", "WireframePostProcess", "Linework",
                             "LineworkMaterialInstance", "HoloCapture", "WorldMaterial", "PlayerCamera"}) {
        Obj prop = ue::FindField(ue::ClassOf(pc), name);
        if (!prop) continue;
        Obj v = At<Obj>(pc, At<int>(prop, ue::L.pOffset));
        Log("PC.%s -> %s", name, v && ue::IsObj(v) ? ue::FullName(v).c_str() : "null");
        Dump(v, name);
        // Actors carry their capture in a component; dump those too.
        if (v && ue::IsObj(v))
            for (Obj c = ue::ClassOf(v); c; c = ue::SuperOf(c)) {
                if (ue::ObjName(c) == "Actor") break;
                for (Obj p = At<Obj>(c, ue::L.sChildren); p; p = At<Obj>(p, ue::L.fNext)) {
                    std::string pcn = ue::ObjName(ue::ClassOf(p));
                    if (pcn != "ComponentProperty" && pcn != "ObjectProperty") continue;
                    Obj sub = At<Obj>(v, At<int>(p, ue::L.pOffset));
                    if (sub && ue::IsObj(sub) && ue::ObjName(ue::ClassOf(sub)).find("Capture") != std::string::npos) Dump(sub, name);
                }
            }
    }
    Dump(localPlayer, "LocalPlayer");
    int n = ue::NumObjects();
    for (int i = 0; i < n; i++) {
        Obj o = ue::ObjAt(i);
        if (!o) continue;
        std::string nm = ue::ObjName(o);
        if (nm.compare(0, 9, "Default__") == 0) continue;
        std::string cn = ue::ObjName(ue::ClassOf(o));
        bool want = cn.find("SceneCapture") != std::string::npos || cn == "PostProcessChain" || cn.find("PostProcessEffect") != std::string::npos ||
                    cn.find("MaterialEffect") != std::string::npos || cn.find("UberPostProcess") != std::string::npos;
        if (want) Dump(o, cn.c_str());
    }
    Log("DUMP end");
}

} // namespace dbgdump
