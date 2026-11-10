#include "ue3.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <map>
#include <vector>

namespace ue {

Layout L;
static bool g_ready = false;

struct TArr {
    uint8_t** data;
    int num;
    int max;
};

static TArr* Names() { return (TArr*)L.gnames; }
static TArr* Objects() { return (TArr*)L.gobjects; }

bool Ready() { return g_ready; }
int NumObjects() { return L.gobjects ? Objects()->num : 0; }
Obj ObjAt(int i) {
    TArr* a = Objects();
    if (i < 0 || i >= a->num) return nullptr;
    return a->data[i];
}

bool IsObj(const void* p) {
    if (!p || ((uintptr_t)p & 3)) return false;
    if (!Readable(p, 0x40)) return false;
    int idx = *(int*)((uint8_t*)p + L.oIndex);
    TArr* a = Objects();
    if (idx < 0 || idx >= a->num) return false;
    return a->data[idx] == (uint8_t*)p;
}

static void FindSections() {
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uintptr_t lo = (uintptr_t)base + sec[i].VirtualAddress;
        uintptr_t hi = lo + sec[i].Misc.VirtualSize;
        if (!memcmp(sec[i].Name, ".text", 6)) { L.textLo = lo; L.textHi = hi; }
        if (!memcmp(sec[i].Name, ".data", 6)) { L.dataLo = lo; L.dataHi = hi; }
    }
    Log("sections: text %08X-%08X data %08X-%08X", (unsigned)L.textLo, (unsigned)L.textHi,
        (unsigned)L.dataLo, (unsigned)L.dataHi);
}

static int FindStrIn(const uint8_t* e, const char* s) {
    size_t n = strlen(s);
    for (int off = 0; off <= 0x18; off += 4) {
        if (!memcmp(e + off, s, n + 1)) return off;
    }
    return -1;
}

static bool FindGNames() {
    for (uintptr_t x = L.dataLo; x + 12 <= L.dataHi; x += 4) {
        TArr* a = (TArr*)x;
        if (a->num < 1000 || a->num > a->max || a->max > 5000000) continue;
        if (!Readable(a->data, 16)) continue;
        uint8_t* e0 = a->data[0];
        uint8_t* e1 = a->data[1];
        if (!Readable(e0, 0x30) || !Readable(e1, 0x30)) continue;
        int off = FindStrIn(e0, "None");
        if (off < 0) continue;
        if (memcmp(e1 + off, "ByteProperty", 13)) continue;
        L.gnames = x;
        L.nameStrOff = off;
        Log("GNames @%08X num=%d strOff=0x%X", (unsigned)x, a->num, off);
        return true;
    }
    return false;
}

static bool FindGObjects() {
    for (uintptr_t x = L.dataLo; x + 12 <= L.dataHi; x += 4) {
        TArr* a = (TArr*)x;
        if (a->num < 1000 || a->num > a->max || a->max > 5000000) continue;
        if (!Readable(a->data, 64 * 4)) continue;
        bool ok = true;
        for (int i = 0; i < 64 && ok; i++) {
            uint8_t* o = a->data[i];
            if (!o) continue;
            if (!Readable(o, 0x40) || *(int*)(o + 4) != i) ok = false;
        }
        if (!ok || !a->data[0] || !a->data[1]) continue;
        L.gobjects = x;
        Log("GObjects @%08X num=%d", (unsigned)x, a->num);
        return true;
    }
    return false;
}

std::string NameStr(int idx) {
    TArr* a = Names();
    if (!a || idx < 0 || idx >= a->num) return "<badname>";
    uint8_t* e = a->data[idx];
    if (!e || !Readable(e, L.nameStrOff + 2)) return "<nullname>";
    const uint8_t* s = e + L.nameStrOff;
    std::string out;
    // Wide entries have bit 0 of the index field (just before HashNext) set.
    bool wide = (L.nameStrOff >= 8) && (*(uint32_t*)(e + L.nameStrOff - 8) & 1);
    if (wide) {
        const wchar_t* w = (const wchar_t*)s;
        for (int i = 0; i < 256 && w[i]; i++) out.push_back(w[i] < 128 ? (char)w[i] : '?');
    } else {
        for (int i = 0; i < 256 && s[i]; i++) out.push_back((char)s[i]);
    }
    return out;
}

int NameIndex(Obj o) { return At<int>(o, L.oName); }
std::string ObjName(Obj o) {
    if (!o) return "null";
    std::string s = NameStr(NameIndex(o));
    int number = At<int>(o, L.oName + 4);
    if (number > 0) s += "_" + std::to_string(number - 1);
    return s;
}
Obj ClassOf(Obj o) { return o ? At<Obj>(o, L.oClass) : nullptr; }
Obj OuterOf(Obj o) { return o ? At<Obj>(o, L.oOuter) : nullptr; }
Obj SuperOf(Obj s) { return s ? At<Obj>(s, L.sSuper) : nullptr; }

std::string FullName(Obj o) {
    if (!o) return "null";
    std::string path = ObjName(o);
    for (Obj out = OuterOf(o); out && IsObj(out); out = OuterOf(out)) path = ObjName(out) + "." + path;
    return ObjName(ClassOf(o)) + " " + path;
}

bool IsA(Obj o, Obj cls) {
    if (!o || !cls) return false;
    for (Obj c = ClassOf(o); c; c = SuperOf(c))
        if (c == cls) return true;
    return false;
}

Obj Find(const char* fullName) {
    // Compare leaf name first for speed.
    const char* leaf = strrchr(fullName, '.');
    const char* sp = strrchr(fullName, ' ');
    if (!leaf || (sp && sp > leaf)) leaf = sp;
    leaf = leaf ? leaf + 1 : fullName;
    int n = NumObjects();
    for (int i = 0; i < n; i++) {
        Obj o = ObjAt(i);
        if (!o) continue;
        if (At<int>(o, L.oName + 4) != 0) continue;
        if (NameStr(NameIndex(o)) != leaf) continue;
        if (FullName(o) == fullName) return o;
    }
    return nullptr;
}

Obj FindField(Obj strct, const char* name, bool searchSuper) {
    for (Obj s = strct; s; s = searchSuper ? SuperOf(s) : nullptr) {
        int guard = 0;
        for (Obj f = At<Obj>(s, L.sChildren); f && guard < 20000; f = At<Obj>(f, L.fNext), guard++) {
            if (NameStr(NameIndex(f)) == name) return f;
        }
    }
    return nullptr;
}

int PropOffset(Obj strct, const char* name) {
    Obj f = FindField(strct, name);
    if (!f) {
        Log("PropOffset: '%s' not found in %s", name, FullName(strct).c_str());
        return -1;
    }
    return At<int>(f, L.pOffset);
}

Obj FindFirstInstance(Obj cls, bool skipDefault) {
    int n = NumObjects();
    for (int i = 0; i < n; i++) {
        Obj o = ObjAt(i);
        if (!o || !IsA(o, cls)) continue;
        if (skipDefault && !strncmp(NameStr(NameIndex(o)).c_str(), "Default__", 9)) continue;
        return o;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Layout discovery
// ---------------------------------------------------------------------------

static bool DiscoverObjectLayout() {
    int n = NumObjects();
    int sample = n < 2000 ? n : 2000;
    // Class offset: every object's value is an object, and class-of-class is a fixed point.
    for (int off = 0x10; off <= 0x48 && L.oClass < 0; off += 4) {
        Obj fixed = nullptr;
        bool ok = true;
        int seen = 0;
        for (int i = 0; i < sample && ok; i++) {
            Obj o = ObjAt(i);
            if (!o) continue;
            Obj c = At<Obj>(o, off);
            if (!IsObj(c)) { ok = false; break; }
            Obj cc = At<Obj>(c, off);
            if (!IsObj(cc)) { ok = false; break; }
            if (At<Obj>(cc, off) != cc) { ok = false; break; }
            if (!fixed) fixed = cc;
            if (cc != fixed) ok = false;
            seen++;
        }
        if (ok && seen > 100) {
            L.oClass = off;
            // Name offset: the fixed point is named "Class".
            for (int noff = 0x10; noff <= 0x48; noff += 4) {
                int idx = At<int>(fixed, noff);
                if (idx > 0 && idx < Names()->num && NameStr(idx) == "Class" && At<int>(fixed, noff + 4) == 0) {
                    L.oName = noff;
                    break;
                }
            }
            // Outer offset: UClass "Class" lives in package "Core".
            if (L.oName >= 0) {
                for (int ooff = 0x10; ooff <= 0x48; ooff += 4) {
                    if (ooff == L.oClass) continue;
                    Obj out = At<Obj>(fixed, ooff);
                    if (IsObj(out) && NameStr(At<int>(out, L.oName)) == "Core") {
                        L.oOuter = ooff;
                        break;
                    }
                }
            }
        }
    }
    Log("UObject layout: Class=0x%X Name=0x%X Outer=0x%X", L.oClass, L.oName, L.oOuter);
    return L.oClass >= 0 && L.oName >= 0 && L.oOuter >= 0;
}

static Obj FindByNameAndOuter(const char* name, Obj outer, const char* className = nullptr) {
    int n = NumObjects();
    for (int i = 0; i < n; i++) {
        Obj o = ObjAt(i);
        if (!o || OuterOf(o) != outer) continue;
        if (At<int>(o, L.oName + 4) != 0) continue;
        if (NameStr(NameIndex(o)) != name) continue;
        if (className && ObjName(ClassOf(o)) != className) continue;
        return o;
    }
    return nullptr;
}

static bool DiscoverReflection(bool dump) {
    Obj core = nullptr, engine = nullptr;
    int n = NumObjects();
    for (int i = 0; i < n && (!core || !engine); i++) {
        Obj o = ObjAt(i);
        if (!o || OuterOf(o)) continue;
        if (ObjName(ClassOf(o)) != "Package") continue;
        std::string nm = ObjName(o);
        if (nm == "Core") core = o;
        if (nm == "Engine") engine = o;
    }
    Obj clsObject = FindByNameAndOuter("Object", core, "Class");
    Obj clsActor = FindByNameAndOuter("Actor", engine, "Class");
    Log("Core=%p Engine=%p Class Object=%p Class Actor=%p", core, engine, clsObject, clsActor);
    if (!clsObject || !clsActor) return false;

    Obj pClass = FindByNameAndOuter("Class", clsObject);
    Obj pOuter = FindByNameAndOuter("Outer", clsObject);
    Obj pName = FindByNameAndOuter("Name", clsObject);
    Obj pIdx = FindByNameAndOuter("ObjectInternalInteger", clsObject);
    Log("props: Class=%p(%s) Outer=%p Name=%p Idx=%p", pClass, FullName(pClass).c_str(), pOuter, pName, pIdx);
    if (dump) {
        LogHex("UClass Object", clsObject, 0x140);
        LogHex("UClass Actor", clsActor, 0x140);
        if (pClass) LogHex("UProperty Object.Class", pClass, 0xA0);
        if (pOuter) LogHex("UProperty Object.Outer", pOuter, 0xA0);
        if (pIdx) LogHex("UProperty Object.ObjectInternalInteger", pIdx, 0xA0);
    }
    if (!pClass || !pOuter || !pName || !pIdx) return false;

    for (int k = 0x3C; k <= 0x9C; k += 4) {
        if (At<int>(pClass, k) == L.oClass && At<int>(pOuter, k) == L.oOuter && At<int>(pName, k) == L.oName &&
            At<int>(pIdx, k) == L.oIndex) {
            L.pOffset = k;
            break;
        }
    }
    for (int k = 0x3C; k <= 0x100; k += 4) {
        if (At<Obj>(clsActor, k) == clsObject && At<Obj>(clsObject, k) == nullptr) {
            L.sSuper = k;
            break;
        }
    }
    // UField::Next: all four props point to null or a sibling with the same outer.
    for (int k = 0x3C; k <= 0x60 && L.fNext < 0; k += 4) {
        bool ok = true;
        int nonNull = 0;
        Obj ps[4] = {pClass, pOuter, pName, pIdx};
        for (Obj p : ps) {
            Obj nx = At<Obj>(p, k);
            if (!nx) continue;
            if (!IsObj(nx) || OuterOf(nx) != clsObject) { ok = false; break; }
            nonNull++;
        }
        if (ok && nonNull >= 3) L.fNext = k;
    }
    // UStruct::Children: pointer to a field owned by the struct with the longest Next chain.
    if (L.fNext >= 0) {
        int best = 0;
        for (int k = 0x3C; k <= 0x100; k += 4) {
            if (k == L.sSuper) continue;
            Obj f = At<Obj>(clsObject, k);
            if (!IsObj(f) || OuterOf(f) != clsObject) continue;
            int len = 0;
            for (Obj c = f; c && IsObj(c) && len < 5000; c = At<Obj>(c, L.fNext)) len++;
            if (len > best) { best = len; L.sChildren = k; }
        }
        Log("Children chain length of Object: %d", best);
    }
    // UFunction::Func: most common .text pointer among functions = UObject::ProcessInternal.
    {
        std::map<int, std::map<uintptr_t, int>> hist;
        int funcs = 0;
        for (int i = 0; i < n && funcs < 4000; i++) {
            Obj o = ObjAt(i);
            if (!o || ObjName(ClassOf(o)) != "Function") continue;
            if (!Readable(o, 0x100)) continue;
            funcs++;
            for (int k = 0x60; k <= 0xF0; k += 4) {
                uintptr_t v = At<uintptr_t>(o, k);
                if (v >= L.textLo && v < L.textHi) hist[k][v]++;
            }
        }
        int bestCount = 0;
        for (auto& kv : hist)
            for (auto& vv : kv.second)
                if (vv.second > bestCount) {
                    bestCount = vv.second;
                    L.fnFunc = kv.first;
                    L.processInternal = vv.first;
                }
        Log("functions sampled=%d, Func off=0x%X ProcessInternal=%08X (count %d)", funcs, L.fnFunc,
            (unsigned)L.processInternal, bestCount);
    }
    Log("reflection: pOffset=0x%X sSuper=0x%X fNext=0x%X sChildren=0x%X", L.pOffset, L.sSuper, L.fNext, L.sChildren);
    return L.pOffset >= 0 && L.sSuper >= 0 && L.fNext >= 0 && L.sChildren >= 0 && L.fnFunc >= 0;
}

static void DumpStruct(const char* fullName, int rawBytes) {
    Obj s = Find(fullName);
    if (!s) {
        Log("DUMP %s: not found", fullName);
        return;
    }
    Log("DUMP %s @%p super=%s", fullName, s, FullName(SuperOf(s)).c_str());
    if (rawBytes) LogHex("  raw", s, rawBytes);
    int guard = 0;
    for (Obj f = At<Obj>(s, L.sChildren); f && guard < 400; f = At<Obj>(f, L.fNext), guard++) {
        Log("   %-18s %-34s off=0x%04X", ObjName(ClassOf(f)).c_str(), ObjName(f).c_str(), At<int>(f, L.pOffset));
    }
}

bool Init(bool dump) {
    if (g_ready) return true;
    FindSections();
    if (!L.dataLo) return false;
    if (!FindGNames()) { Log("GNames not found"); return false; }
    if (!FindGObjects()) { Log("GObjects not found"); return false; }
    if (dump) {
        for (int i = 0; i < 3; i++) LogHex("FNameEntry", Names()->data[i], 0x20);
        for (int i = 0; i < 3; i++) LogHex("UObject", ObjAt(i), 0x50);
    }
    if (!DiscoverObjectLayout()) return false;
    if (!DiscoverReflection(dump)) return false;
    g_ready = true;
    if (dump) {
        DumpStruct("Function Engine.Controller.GetPlayerViewPoint", 0x100);
        DumpStruct("Function Engine.PlayerController.GetPlayerViewPoint", 0x100);
        DumpStruct("Function Engine.PlayerController.GetFOVAngle", 0x100);
        DumpStruct("Function Engine.PlayerController.ConsoleCommand", 0x100);
        DumpStruct("Function Engine.Actor.Move", 0x100);
        DumpStruct("Function Engine.Actor.SetLocation", 0x100);
        DumpStruct("Class Hazard.HazardPlayerController", 0);
        DumpStruct("Class Hazard.HazardPawn", 0);
        DumpStruct("Class Hazard.HazardPlayerInput", 0);
        DumpStruct("Class Hazard.HazardWeap_TileGun", 0);
        DumpStruct("Class Hazard.HazardGun", 0);
        DumpStruct("Class Hazard.HazardHUD", 0);
        DumpStruct("Class Hazard.HazardGame", 0);
        int n = NumObjects();
        for (int i = 0; i < n; i++) {
            Obj o = ObjAt(i);
            if (!o) continue;
            std::string cn = ObjName(ClassOf(o));
            if (cn == "Class") {
                std::string fn = FullName(o);
                if (fn.find("Hazard.") != std::string::npos)
                    Log("HAZCLASS %s : %s", fn.c_str(), FullName(SuperOf(o)).c_str());
            }
        }
    }
    return true;
}

} // namespace ue
