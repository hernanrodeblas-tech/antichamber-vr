#pragma once
// Runtime reflection over Unreal Engine 3 (UDK, 32-bit) internals.
// Everything is discovered at runtime from GNames / GObjects, so no script
// compiler or SDK is needed.
#include <cstdint>
#include <string>

namespace ue {

typedef uint8_t* Obj; // any UObject*

struct Layout {
    uintptr_t textLo = 0, textHi = 0, dataLo = 0, dataHi = 0;
    uintptr_t gnames = 0;   // address of TArray<FNameEntry*>
    uintptr_t gobjects = 0; // address of TArray<UObject*>
    int nameStrOff = -1;    // FNameEntry -> string
    int oIndex = 4, oOuter = -1, oName = -1, oClass = -1; // UObject
    int fNext = -1;         // UField::Next
    int sSuper = -1;        // UStruct::SuperField
    int sChildren = -1;     // UStruct::Children
    int pOffset = -1;       // UProperty::Offset
    int fnFunc = -1;        // UFunction::Func
    uintptr_t processInternal = 0;
};
extern Layout L;

bool Init(bool dump);
bool Ready();

int NumObjects();
Obj ObjAt(int i);
bool IsObj(const void* p);

std::string NameStr(int idx);
int NameIndex(Obj o);
std::string ObjName(Obj o);
std::string FullName(Obj o); // "Class Outer.Outer.Name"
Obj ClassOf(Obj o);
Obj OuterOf(Obj o);
Obj SuperOf(Obj strct);
bool IsA(Obj o, Obj cls);

Obj Find(const char* fullName);                    // exact FullName match
Obj FindField(Obj strct, const char* name, bool searchSuper = true);
int PropOffset(Obj strct, const char* name);       // -1 if missing
Obj FindFirstInstance(Obj cls, bool skipDefault = true);

template <typename T> inline T& At(void* base, int off) { return *(T*)((uint8_t*)base + off); }

} // namespace ue
