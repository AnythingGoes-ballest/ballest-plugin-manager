#include "engine.hpp"

#include <windows.h>

#include <map>
#include <set>

#include "layout.hpp"
#include "log.hpp"

namespace eng {
namespace {

uintptr_t gBase = 0;
uint8_t* gObjects = nullptr;
uint8_t* gNamePool = nullptr;
using ProcessEventFn = void (*)(Obj self, Obj fn, void* params);
ProcessEventFn gProcessEvent = nullptr;

template <class T>
T At(const void* p, int offset) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(T));
    return v;
}

// Lookups keyed on a class, struct or function are cached with a Weak to that object: if it is ever unloaded
// (map-specific blueprint classes are) and its address reused, the entry no longer matches and is rebuilt.
template <class V>
struct KeyedCache {
    struct Entry {
        Weak owner;
        V value;
    };
    std::map<std::pair<Obj, std::string>, Entry> entries;

    const V* Find(Obj owner, const std::string& key) {
        auto it = entries.find({owner, key});
        if (it == entries.end() || Get(it->second.owner) != owner) return nullptr;
        return &it->second.value;
    }
    void Store(Obj owner, const std::string& key, const V& value) { entries[{owner, key}] = {MakeWeak(owner), value}; }
};

KeyedCache<Prop> gProps;
KeyedCache<Obj> gFunctions;
KeyedCache<std::vector<ParamInfo>> gParams;
std::map<std::string, Weak> gClasses, gCdos;
std::set<std::string> gReported;

void ReportOnce(const std::string& what) {
    if (gReported.insert(what).second) hostlog::Warn("missing: " + what);
}

std::string FieldName(const uint8_t* field) {
    return Name(At<uint32_t>(field, layout::kFFieldNameOffset), At<int32_t>(field, layout::kFFieldNameOffset + 4));
}

}  // namespace

// --- setup -------------------------------------------------------------------------------------------------------

bool Init(uintptr_t moduleBase) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
    const uint32_t stamp = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage;
    hostlog::Info("game build: PE timestamp " + std::to_string(stamp) + ", image size " + hostlog::Hex(size));
    if (stamp != layout::kExpectedGameExeTimeDateStamp || size != layout::kExpectedGameExeSizeOfImage) {
        hostlog::Error("unsupported game build (the host was measured against timestamp " +
                       std::to_string(layout::kExpectedGameExeTimeDateStamp) + "); staying inactive");
        return false;
    }
    gBase = moduleBase;
    gObjects = reinterpret_cast<uint8_t*>(moduleBase + layout::kGlobalObjectArrayOffsetInExe);
    gNamePool = reinterpret_cast<uint8_t*>(moduleBase + layout::kNamePoolOffsetInExe);
    gProcessEvent = reinterpret_cast<ProcessEventFn>(moduleBase + layout::kProcessEventFunctionOffsetInExe);
    return true;
}

uintptr_t Base() { return gBase; }

bool InImage(const void* address) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    return a >= gBase && a < gBase + layout::kExpectedGameExeSizeOfImage;
}

// --- objects and names -----------------------------------------------------------------------------------------

std::string Name(uint32_t comparisonIndex, int32_t number) {
    auto* blocks = reinterpret_cast<uint8_t**>(gNamePool + layout::kNamePoolBlockListOffset);
    uint8_t* block = blocks[comparisonIndex >> 16];
    if (!block) return "?";
    const uint8_t* entry = block + (comparisonIndex & 0xFFFF) * 2;
    const uint16_t header = At<uint16_t>(entry, 0);
    const int length = header >> 6;
    std::string s = (header & 1) ? Narrow(reinterpret_cast<const wchar_t*>(entry + 2), length)
                                 : std::string(reinterpret_cast<const char*>(entry + 2), length);
    if (number != 0) s += "_" + std::to_string(number - 1);
    return s;
}

std::string ObjName(Obj o) {
    return o ? Name(At<uint32_t>(o, layout::kUObjectNameOffset), At<int32_t>(o, layout::kUObjectNameOffset + 4)) : "null";
}
Obj ClassOf(Obj o) { return o ? At<Obj>(o, layout::kUObjectClassOffset) : nullptr; }
Obj OuterOf(Obj o) { return o ? At<Obj>(o, layout::kUObjectOuterOffset) : nullptr; }
Obj SuperOf(Obj structure) { return structure ? At<Obj>(structure, layout::kUStructParentStructOffset) : nullptr; }
bool IsDefaultObject(Obj o) { return ObjName(o).rfind("Default__", 0) == 0; }

std::string PathOf(Obj o) {
    std::vector<std::string> parts;
    for (; o; o = OuterOf(o)) parts.push_back(ObjName(o));
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) path += (path.empty() ? "" : ".") + *it;
    return path;
}

bool IsA(Obj o, Obj cls) {
    if (!cls) return false;
    for (Obj c = ClassOf(o); c; c = SuperOf(c))
        if (c == cls) return true;
    return false;
}

int32_t NumObjects() { return At<int32_t>(gObjects, layout::kObjectArrayObjectCountOffset); }

Obj ObjectAt(int32_t index) {
    auto** chunks = At<uint8_t**>(gObjects, layout::kObjectArrayChunkListOffset);
    uint8_t* chunk = chunks ? chunks[index / layout::kObjectArrayItemsPerChunk] : nullptr;
    return chunk ? At<Obj>(chunk, (index % layout::kObjectArrayItemsPerChunk) * layout::kObjectArrayItemSizeBytes + layout::kObjectArrayItemObjectPointerOffset) : nullptr;
}

bool IsLive(Obj o) {
    // Objects are 8-byte aligned and never in the first 64 KB; anything else is not an object pointer.
    const uintptr_t a = reinterpret_cast<uintptr_t>(o);
    if (!o || (a & 7) || a < 0x10000) return false;
    const int32_t index = At<int32_t>(o, layout::kUObjectArrayIndexOffset);
    return index >= 0 && index < NumObjects() && ObjectAt(index) == o;
}

Weak MakeWeak(Obj fresh) {
    if (!IsLive(fresh)) return {};
    return {fresh, At<int32_t>(fresh, layout::kUObjectArrayIndexOffset)};
}

Obj Get(const Weak& w) {
    if (!w.o || w.index < 0 || w.index >= NumObjects()) return nullptr;
    return ObjectAt(w.index) == w.o ? w.o : nullptr;
}

Obj FindClass(const std::string& name) {
    if (auto it = gClasses.find(name); it != gClasses.end())
        if (Obj cls = Get(it->second)) return cls;
    Obj found = nullptr;
    ForEachObject([&](Obj o) {
        if (ObjName(o) != name) return true;
        const std::string meta = ObjName(ClassOf(o));      // Class, BlueprintGeneratedClass, ...
        if (meta.size() >= 5 && meta.compare(meta.size() - 5, 5, "Class") == 0) found = o;
        return found == nullptr;
    });
    if (found) gClasses[name] = MakeWeak(found);
    else ReportOnce("class " + name);
    return found;
}

Obj FindObjectByName(const std::string& name) {
    Obj found = nullptr;
    ForEachObject([&](Obj o) {
        if (ObjName(o) == name) found = o;
        return found == nullptr;
    });
    return found;
}

Obj FindCdo(const std::string& className) {
    if (auto it = gCdos.find(className); it != gCdos.end())
        if (Obj cdo = Get(it->second)) return cdo;
    Obj cdo = FindObjectByName("Default__" + className);
    if (cdo) gCdos[className] = MakeWeak(cdo);
    else ReportOnce("default object of " + className);
    return cdo;
}

// --- properties --------------------------------------------------------------------------------------------------

Prop FindProp(Obj structure, const std::string& name) {
    if (!structure) return {};
    if (const Prop* cached = gProps.Find(structure, name)) return *cached;
    Prop result;
    for (Obj s = structure; s && !result; s = SuperOf(s))
        for (uint8_t* f = At<uint8_t*>(s, layout::kUStructFirstPropertyOffset); f; f = At<uint8_t*>(f, layout::kFFieldNextFieldOffset))
            if (FieldName(f) == name) {
                result = {f, At<int32_t>(f, layout::kFPropertyValueLocationOffset), At<int32_t>(f, layout::kFPropertyValueSizeOffset)};
                break;
            }
    gProps.Store(structure, name, result);
    return result;
}

std::string KindOf(const Prop& p) {
    if (!p) return "";
    const uint8_t* fieldClass = At<uint8_t*>(p.field, layout::kFFieldTypeOffset);
    return fieldClass
               ? Name(At<uint32_t>(fieldClass, layout::kFFieldTypeNameOffset), At<int32_t>(fieldClass, layout::kFFieldTypeNameOffset + 4))
               : "";
}

// Only a StructProperty has a struct pointer at this offset; for any other kind those bytes are something else.
Obj StructOf(const Prop& p) {
    return KindOf(p) == "StructProperty" ? At<Obj>(p.field, layout::kFStructPropertyStructTypeOffset) : nullptr;
}

std::vector<std::string> PropertyNames(Obj structure) {
    std::vector<std::string> out;
    for (Obj s = structure; s; s = SuperOf(s))
        for (uint8_t* f = At<uint8_t*>(s, layout::kUStructFirstPropertyOffset); f; f = At<uint8_t*>(f, layout::kFFieldNextFieldOffset))
            out.push_back(FieldName(f));
    return out;
}

int NestedOffset(Obj structure, const std::vector<const char*>& path, Prop* last) {
    int offset = 0;
    Prop p;
    for (const char* name : path) {
        p = FindProp(structure, name);
        if (!p) return -1;
        offset += p.offset;
        structure = StructOf(p);
    }
    if (last) *last = p;
    return offset;
}

namespace {
// The named property of a live object, checked to be exactly `size` bytes.
Prop CheckedProp(Obj o, const std::string& name, size_t size) {
    if (!IsLive(o)) return {};
    Prop p = FindProp(ClassOf(o), name);
    if (!p || static_cast<size_t>(p.size) != size) {
        ReportOnce(ObjName(ClassOf(o)) + "." + name + (p ? " (size " + std::to_string(p.size) + ")" : ""));
        return {};
    }
    return p;
}

Prop BoolProp(Obj o, const std::string& name) {
    if (!IsLive(o)) return {};
    Prop p = FindProp(ClassOf(o), name);
    if (KindOf(p) != "BoolProperty") {
        ReportOnce("bool " + ObjName(ClassOf(o)) + "." + name);
        return {};
    }
    return p;
}
}  // namespace

bool ReadBytes(Obj o, const std::string& name, void* out, size_t size) {
    Prop p = CheckedProp(o, name, size);
    if (p) std::memcpy(out, o + p.offset, size);
    return static_cast<bool>(p);
}

bool WriteBytes(Obj o, const std::string& name, const void* in, size_t size) {
    Prop p = CheckedProp(o, name, size);
    if (p) std::memcpy(o + p.offset, in, size);
    return static_cast<bool>(p);
}

Obj ReadObj(Obj o, const std::string& name) {
    Obj v = nullptr;
    return ReadBytes(o, name, &v, sizeof v) ? v : nullptr;
}

std::vector<Obj> ReadObjArray(Obj o, const std::string& name) {
    struct {
        Obj* data;
        int32_t num, max;
    } array{};
    std::vector<Obj> out;
    if (ReadBytes(o, name, &array, sizeof array) && array.data) out.assign(array.data, array.data + array.num);
    return out;
}

// A bool may share its byte with others (bitfields): the byte and mask come from the FBoolProperty.
bool ReadBool(Obj o, const std::string& name, bool* out) {
    Prop p = BoolProp(o, name);
    if (p)
        *out = (o[p.offset + At<uint8_t>(p.field, layout::kFBoolPropertyByteIndexOffset)] & At<uint8_t>(p.field, layout::kFBoolPropertyBitMaskOffset)) != 0;
    return static_cast<bool>(p);
}

bool WriteBool(Obj o, const std::string& name, bool value) {
    Prop p = BoolProp(o, name);
    if (!p) return false;
    uint8_t& byte = o[p.offset + At<uint8_t>(p.field, layout::kFBoolPropertyByteIndexOffset)];
    const uint8_t mask = At<uint8_t>(p.field, layout::kFBoolPropertyBitMaskOffset);
    byte = value ? (byte | mask) : (byte & ~mask);
    return true;
}

// --- functions -----------------------------------------------------------------------------------------------------

std::vector<ParamInfo> ParamsOf(Obj fn) {
    if (!fn) return {};
    if (const auto* cached = gParams.Find(fn, "")) return *cached;
    std::vector<ParamInfo> out;
    for (uint8_t* f = At<uint8_t*>(fn, layout::kUStructFirstPropertyOffset); f; f = At<uint8_t*>(f, layout::kFFieldNextFieldOffset)) {
        const uint64_t flags = At<uint64_t>(f, layout::kFPropertyFlagsOffset);
        if (!(flags & layout::kPropertyFlagIsParameter)) continue;
        out.push_back({FieldName(f), At<int32_t>(f, layout::kFPropertyValueLocationOffset), At<int32_t>(f, layout::kFPropertyValueSizeOffset), flags,
                       (flags & layout::kPropertyFlagIsReturnValue) != 0, (flags & layout::kPropertyFlagIsOutParameter) != 0});
    }
    gParams.Store(fn, "", out);
    return out;
}

std::string Describe(Obj fn) {
    std::string args, ret;
    for (const auto& p : ParamsOf(fn)) {
        if (p.isReturn) ret = " -> " + std::to_string(p.size);
        else args += (args.empty() ? "" : ", ") + p.name + (p.isOut ? " out" : "") + ": " + std::to_string(p.size);
    }
    return ObjName(fn) + "(" + args + ")" + ret;
}

std::vector<std::string> FunctionNames(Obj cls) {
    std::vector<std::string> out;
    for (Obj c = cls; c; c = SuperOf(c))
        for (Obj f = At<Obj>(c, layout::kUStructFirstFunctionOffset); f; f = At<Obj>(f, layout::kUFieldNextFieldOffset)) out.push_back(ObjName(f));
    return out;
}

Obj FindFunction(Obj cls, const std::string& name) {
    if (!cls) return nullptr;
    if (const Obj* cached = gFunctions.Find(cls, name)) return *cached;
    Obj found = nullptr;
    for (Obj c = cls; c && !found; c = SuperOf(c))
        for (Obj f = At<Obj>(c, layout::kUStructFirstFunctionOffset); f && !found; f = At<Obj>(f, layout::kUFieldNextFieldOffset))
            if (ObjName(f) == name) found = f;
    gFunctions.Store(cls, name, found);
    return found;
}

Obj FunctionOn(Obj object, const char* name) {
    if (!IsLive(object)) return nullptr;
    Obj fn = FindFunction(ClassOf(object), name);
    if (!fn) ReportOnce("function " + ObjName(ClassOf(object)) + "." + name);
    return fn;
}

Params::Params(Obj fn) : fn_(fn) {
    if (fn) buf_.assign(At<uint16_t>(fn, layout::kUFunctionParametersSizeOffset) + 16, 0);    // + slack
    else ok_ = false;
}

bool Params::Set(const char* name, const void* data, size_t size) {
    Prop p = fn_ ? FindProp(fn_, name) : Prop{};
    if (!p || static_cast<size_t>(p.size) != size || p.offset + size > buf_.size()) {
        if (fn_)
            ReportOnce("parameter " + ObjName(fn_) + "." + name + " (size " + std::to_string(p ? p.size : -1) + ", given " +
                       std::to_string(size) + ")");
        ok_ = false;
        return false;
    }
    std::memcpy(buf_.data() + p.offset, data, size);
    return true;
}

bool Params::SetArg(int index, const void* data, size_t size) {
    int i = 0;
    for (const auto& p : ParamsOf(fn_)) {
        if (p.isReturn || i++ != index) continue;
        if (static_cast<size_t>(p.size) == size && p.offset + size <= buf_.size()) {
            std::memcpy(buf_.data() + p.offset, data, size);
            return true;
        }
        break;
    }
    if (fn_) ReportOnce("argument " + std::to_string(index) + " of " + Describe(fn_) + " given " + std::to_string(size) + " bytes");
    ok_ = false;
    return false;
}

const uint8_t* Params::Get(const char* name, size_t* size) const {
    Prop p = fn_ ? FindProp(fn_, name) : Prop{};
    if (!p) return nullptr;
    if (size) *size = p.size;
    return buf_.data() + p.offset;
}

int32_t Params::SizeOf(const char* name) const {
    Prop p = fn_ ? FindProp(fn_, name) : Prop{};
    return p ? p.size : -1;
}

const uint8_t* Params::Return(size_t* size) const {
    for (const auto& p : ParamsOf(fn_))
        if (p.isReturn) {
            if (size) *size = p.size;
            return buf_.data() + p.offset;
        }
    return nullptr;
}

Obj Params::GetObj(const char* name) const {
    size_t size = 0;
    const uint8_t* v = Get(name, &size);
    return v && size == sizeof(Obj) ? At<Obj>(v, 0) : nullptr;
}

Obj Params::ReturnObj() const { return invoked_ ? ReturnAs<Obj>(nullptr) : nullptr; }

bool Params::ReturnBool() const {
    const uint8_t* r = invoked_ ? Return() : nullptr;
    return r && *r;
}

bool Invoke(Obj object, Params& params) {
    if (!params.Ok() || !IsLive(object)) return false;
    gProcessEvent(object, params.Fn(), params.Data());
    params.invoked_ = true;
    return true;
}

// --- strings and text --------------------------------------------------------------------------------------------

std::wstring Widen(const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
}

std::string Narrow(const wchar_t* data, int len) {
    if (!data || len <= 0) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, data, len, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, len, s.data(), n, nullptr, nullptr);
    return s;
}

std::string ReadFString(const uint8_t* p) {
    const int32_t num = At<int32_t>(p, 8);
    return num > 1 ? Narrow(At<const wchar_t*>(p, 0), num - 1) : "";
}

Params MakeText(const std::string& s) {
    const std::wstring w = Widen(s);
    const FString fs{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)};
    return Call(FindCdo("KismetTextLibrary"), "Conv_StringToText", fs);     // the string is copied during the call
}

void ReleaseText(const uint8_t* ftext) {
    uint8_t* data = ftext ? At<uint8_t*>(ftext, 0) : nullptr;
    const uintptr_t address = reinterpret_cast<uintptr_t>(data);
    if (!data || (address & 7) || address < 0x10000) return;
    auto* count = reinterpret_cast<volatile long*>(data + layout::kTextDataReferenceCountOffset);
    const long references = *count;
    if (InImage(At<void*>(data, 0)) && references >= 2 && references < 1000000) InterlockedDecrement(count);
    else ReportOnce("text reference layout (count " + std::to_string(references) + "): keeping the reference");
}

}  // namespace eng
