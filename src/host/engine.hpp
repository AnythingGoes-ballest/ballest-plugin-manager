// Engine core: reflection over the running game (objects, names, properties, function calls), built only from
// the measured layouts in layout.hpp. No engine headers and no UE4SS code.
//
// Two rules keep the host from ever touching freed memory:
//   * a pointer obtained this frame (a return value, a property of a live object) may be used right away;
//   * anything kept across frames is held as a Weak and read back with Get(), which only consults the engine's
//     object array and never the object itself.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace eng {

using Obj = uint8_t*;

bool Init(uintptr_t moduleBase);        // the measured tables on the known build; on another build, Locate finds them
bool KnownBuild();                      // the game build the host was measured on
bool Locate();                          // on another build: finds the name pool, object array and ProcessEvent;
                                        // call until true
uintptr_t Base();
bool InImage(const void* address);      // inside the game exe's image (code, vtables, static data)

// --- objects and names -----------------------------------------------------------------------------------------
std::string Name(uint32_t comparisonIndex, int32_t number);
std::string ObjName(Obj o);
Obj ClassOf(Obj o);
Obj OuterOf(Obj o);
std::string PathOf(Obj o);
bool IsDefaultObject(Obj o);
Obj SuperOf(Obj structure);
bool IsA(Obj o, Obj cls);

int32_t NumObjects();
Obj ObjectAt(int32_t index);
bool IsLive(Obj freshPointer);          // reads the object: only for pointers obtained this frame
const uint8_t* ItemOf(int32_t index);   // the object's FUObjectItem in the global object array, or null

template <class F>
void ForEachObject(F&& f) {             // f(Obj) returns false to stop
    const int32_t n = NumObjects();
    for (int32_t i = 0; i < n; ++i)
        if (Obj o = ObjectAt(i); o && !f(o)) return;
}

struct Weak {
    Obj o = nullptr;
    int32_t index = -1;
};
Weak MakeWeak(Obj fresh);
Obj Get(const Weak& w);                 // the object, or null once it is gone

Obj FindClass(const std::string& name);         // UClass by short name
Obj FindObjectByName(const std::string& name);  // first object with this short name (slow: scans everything)
Obj FindCdo(const std::string& className);      // "Default__<className>", e.g. FindCdo("GameplayStatics")

// --- properties --------------------------------------------------------------------------------------------------
struct Prop {
    uint8_t* field = nullptr;           // the FProperty
    int32_t offset = 0;
    int32_t size = 0;
    explicit operator bool() const { return field != nullptr; }
};
Prop FindProp(Obj structure, const std::string& name);      // on a class/struct/function and its supers
std::string KindOf(const Prop& p);                          // "StructProperty", "BoolProperty", ...
Obj StructOf(const Prop& p);                                // the struct of a StructProperty, else null
std::vector<std::string> PropertyNames(Obj structure);
// Offset of a nested member, e.g. {"Font", "Size"} on TextBlock; -1 if any step is missing.
int NestedOffset(Obj structure, const std::vector<const char*>& path, Prop* last = nullptr);

// Object properties by name, type-checked by size. A missing property is logged once and reads as nothing.
Obj ReadObj(Obj o, const std::string& name);
bool ReadBytes(Obj o, const std::string& name, void* out, size_t size);
bool WriteBytes(Obj o, const std::string& name, const void* in, size_t size);
std::vector<Obj> ReadObjArray(Obj o, const std::string& name);    // TArray<UObject*>
bool ReadBool(Obj o, const std::string& name, bool* out);         // plain and bitfield bools
bool WriteBool(Obj o, const std::string& name, bool value);

// --- functions -----------------------------------------------------------------------------------------------------
struct ParamInfo {
    std::string name;
    int32_t offset, size;
    uint64_t flags;
    bool isReturn, isOut;
};
std::vector<ParamInfo> ParamsOf(Obj fn);
std::string Describe(Obj fn);                   // "Name(a: 8, b out: 4) -> 8"
std::vector<std::string> FunctionNames(Obj cls);
Obj FindFunction(Obj cls, const std::string& name);
Obj FunctionOn(Obj object, const char* name);   // on the object's class; logs once if missing

// A parameter buffer for one UFunction call. Every write is checked against the parameter's real size, and a
// bad write marks the call as not to be made.
class Params {
public:
    explicit Params(Obj fn);
    bool Set(const char* name, const void* data, size_t size);
    template <class T>
    bool Set(const char* name, const T& v) { return Set(name, &v, sizeof(T)); }
    bool SetArg(int index, const void* data, size_t size);          // by position, return value excluded
    template <class T>
    bool SetArg(int index, const T& v) { return SetArg(index, &v, sizeof(T)); }
    const uint8_t* Get(const char* name, size_t* size = nullptr) const;
    int32_t SizeOf(const char* name) const;
    const uint8_t* Return(size_t* size = nullptr) const;
    Obj GetObj(const char* name) const;
    Obj ReturnObj() const;
    bool ReturnBool() const;
    template <class T>
    T ReturnAs(T fallback = T{}) const {
        size_t size = 0;
        const uint8_t* r = Return(&size);
        if (r && size == sizeof(T)) std::memcpy(&fallback, r, sizeof(T));
        return fallback;
    }
    uint8_t* Data() { return buf_.data(); }
    Obj Fn() const { return fn_; }
    bool Ok() const { return ok_; }
    bool Invoked() const { return invoked_; }

private:
    friend bool Invoke(Obj, Params&);
    Obj fn_;
    std::vector<uint8_t> buf_;
    bool ok_ = true, invoked_ = false;
};

// Calls through ProcessEvent (game thread only). False if the parameters were bad.
bool Invoke(Obj object, Params& params);

// Call `function` on `object` with positional arguments; the returned Params holds the result.
template <class... Args>
Params Call(Obj object, const char* function, const Args&... args) {
    Params p(FunctionOn(object, function));
    int index = 0;
    (p.SetArg(index++, args), ...);
    Invoke(object, p);
    return p;
}

// --- engine strings ------------------------------------------------------------------------------------------------
struct FString {                        // TArray<TCHAR>, built by the host for parameters
    const wchar_t* data;
    int32_t num;
    int32_t max;
};
std::wstring Widen(const std::string& utf8);
std::string Narrow(const wchar_t* data, int len);
std::string ReadFString(const uint8_t* fstring);

// Text: MakeText returns the finished Conv_StringToText call; its return value is an FText holding one reference
// the host owns. ReleaseText drops that reference once the text has been handed to a widget, never below one,
// so the engine alone decides when the text is freed.
Params MakeText(const std::string& s);
void ReleaseText(const uint8_t* ftext);

}  // namespace eng
