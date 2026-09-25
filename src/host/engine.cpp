#include "engine.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "layout.hpp"
#include "log.hpp"

namespace eng {
namespace {

uintptr_t gBase = 0;
uint32_t gImageSize = 0;
bool gKnownBuild = false;
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

// --- finding the engine's tables on a build that was not measured ------------------------------------------------
// Every pointer is checked against the process's readable memory before it is read, so looking at candidates cannot
// fault. The readable ranges are listed once per search (VirtualQuery over the address space) and looked up by
// binary search; asking VirtualQuery per candidate took 34 s for one search (measured).
std::vector<std::pair<uintptr_t, uintptr_t>> gReadable;     // [start, end), sorted

void ListReadableMemory() {
    gReadable.clear();
    const DWORD readableProtection = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
    MEMORY_BASIC_INFORMATION info{};
    for (uintptr_t a = 0x10000; a < 0x7FFFFFFFFFFF && VirtualQuery(reinterpret_cast<void*>(a), &info, sizeof info);
         a = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize) {
        if (info.State == MEM_COMMIT && (info.Protect & readableProtection) && !(info.Protect & PAGE_GUARD)) {
            const uintptr_t start = reinterpret_cast<uintptr_t>(info.BaseAddress), end = start + info.RegionSize;
            if (!gReadable.empty() && gReadable.back().second == start) gReadable.back().second = end;
            else gReadable.push_back({start, end});
        }
    }
}

bool Readable(uintptr_t address, size_t bytes) {
    if (address < 0x10000 || address > 0x7FFFFFFFFFFF) return false;
    auto it = std::upper_bound(gReadable.begin(), gReadable.end(), std::make_pair(address, UINTPTR_MAX));
    if (it == gReadable.begin()) return false;
    --it;
    return address >= it->first && address + bytes <= it->second;
}

uintptr_t ReadPointer(uintptr_t address) { return Readable(address, 8) ? At<uintptr_t>(reinterpret_cast<void*>(address), 0) : 0; }

// Every 8-byte slot of the exe's writable sections (where the engine's global tables live).
template <class F>
uintptr_t FindInWritableSections(F isIt) {
    ListReadableMemory();
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(gBase);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(gBase + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uintptr_t start = gBase + section->VirtualAddress, end = start + section->Misc.VirtualSize;
        for (uintptr_t a = start; a + 0x20 <= end; a += 8)
            if (Readable(a, 0x20) && isIt(a)) return a;
    }
    return 0;
}

// FNamePool: block 0 (at +kNamePoolBlockListOffset) starts with entry 0, "None": a header (length 4 in the bits from
// 6 up, bit 0 clear for narrow characters, hash bits between) then the letters.
uint8_t* FindNamePool() {
    return reinterpret_cast<uint8_t*>(FindInWritableSections([](uintptr_t a) {
        const uintptr_t block = ReadPointer(a + layout::kNamePoolBlockListOffset);
        if (!block || !Readable(block, 6)) return false;
        const uint16_t header = At<uint16_t>(reinterpret_cast<void*>(block), 0);
        return (header >> 6) == 4 && !(header & 1) && std::memcmp(reinterpret_cast<void*>(block + 2), "None", 4) == 0;
    }));
}

// The object array: a chunk list and a count, where the objects in the first slots each store their own slot
// number (UObject::InternalIndex). Needs the engine to have created its first objects.
uint8_t* FindObjectArray() {
    return reinterpret_cast<uint8_t*>(FindInWritableSections([](uintptr_t a) {
        const int32_t count = At<int32_t>(reinterpret_cast<void*>(a), layout::kObjectArrayObjectCountOffset);
        if (count < 1000 || count > 10000000) return false;
        const uintptr_t chunk = ReadPointer(ReadPointer(a + layout::kObjectArrayChunkListOffset));
        if (!chunk || !Readable(chunk, 200 * layout::kObjectArrayItemSizeBytes)) return false;
        int matching = 0;
        for (int i = 0; i < 200; ++i) {
            const uintptr_t object = ReadPointer(chunk + i * layout::kObjectArrayItemSizeBytes + layout::kObjectArrayItemObjectPointerOffset);
            if (object && Readable(object, 0x28) && At<int32_t>(reinterpret_cast<void*>(object), layout::kUObjectArrayIndexOffset) == i) ++matching;
        }
        return matching >= 150;
    }));
}

bool InCode(uintptr_t address) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(gBase);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(gBase + dos->e_lfanew);
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) && address >= gBase + section->VirtualAddress &&
            address < gBase + section->VirtualAddress + section->Misc.VirtualSize)
            return true;
    return false;
}

}  // namespace

// --- setup -------------------------------------------------------------------------------------------------------

bool Init(uintptr_t moduleBase) {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(moduleBase);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
    const uint32_t stamp = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage;
    hostlog::Info("game build: PE timestamp " + std::to_string(stamp) + ", image size " + hostlog::Hex(size));
    gBase = moduleBase;
    gImageSize = size;
    gKnownBuild = stamp == layout::kExpectedGameExeTimeDateStamp && size == layout::kExpectedGameExeSizeOfImage;
    // Test switch: a file named force_unknown_build next to the log makes this build count as unmeasured, so the
    // search below can be checked against the measured addresses.
    if (GetFileAttributesW((hostlog::DataDir() + L"\\force_unknown_build").c_str()) != INVALID_FILE_ATTRIBUTES) {
        hostlog::Warn("force_unknown_build: treating this build as unmeasured");
        gKnownBuild = false;
    }
    if (gKnownBuild) {
        gObjects = reinterpret_cast<uint8_t*>(moduleBase + layout::kGlobalObjectArrayOffsetInExe);
        gNamePool = reinterpret_cast<uint8_t*>(moduleBase + layout::kNamePoolOffsetInExe);
        gProcessEvent = reinterpret_cast<ProcessEventFn>(moduleBase + layout::kProcessEventFunctionOffsetInExe);
        return true;
    }
    hostlog::Warn("this game build was not measured (the host knows timestamp " + std::to_string(layout::kExpectedGameExeTimeDateStamp) +
                  "); finding the engine's tables again and trying");
    return true;
}

bool KnownBuild() { return gKnownBuild; }

// The name pool, the object array and ProcessEvent, on a build that was not measured. Called until it succeeds: the
// game builds them a moment after it starts (the name pool was measured empty when this DLL's thread starts).
bool Locate() {
    if (gNamePool && gObjects && gProcessEvent) return true;
    if (!gNamePool) {
        gNamePool = FindNamePool();
        if (!gNamePool) return false;
        hostlog::Info("name pool found at +" + hostlog::Hex(reinterpret_cast<uintptr_t>(gNamePool) - gBase));
    }
    if (!gObjects) {
        gObjects = FindObjectArray();
        if (!gObjects) return false;
        hostlog::Info("object array found at +" + hostlog::Hex(reinterpret_cast<uintptr_t>(gObjects) - gBase) + ", " +
                      std::to_string(NumObjects()) + " objects");
    }
    Obj object = FindCdo("Object");
    const uintptr_t vtable = object ? ReadPointer(reinterpret_cast<uintptr_t>(object)) : 0;
    const uintptr_t processEvent = vtable ? ReadPointer(vtable + layout::kProcessEventVtableSlot * 8) : 0;
    if (!processEvent || !InCode(processEvent)) {
        hostlog::Error("ProcessEvent not found in UObject's vtable slot " + std::to_string(layout::kProcessEventVtableSlot));
        return false;
    }
    gProcessEvent = reinterpret_cast<ProcessEventFn>(processEvent);
    hostlog::Info("ProcessEvent found at +" + hostlog::Hex(processEvent - gBase));
    return true;
}

uintptr_t Base() { return gBase; }

bool InImage(const void* address) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    return a >= gBase && a < gBase + gImageSize;
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

int32_t NumObjects() { return gObjects ? At<int32_t>(gObjects, layout::kObjectArrayObjectCountOffset) : 0; }

Obj ObjectAt(int32_t index) {
    if (!gObjects) return nullptr;
    auto** chunks = At<uint8_t**>(gObjects, layout::kObjectArrayChunkListOffset);
    uint8_t* chunk = chunks ? chunks[index / layout::kObjectArrayItemsPerChunk] : nullptr;
    return chunk ? At<Obj>(chunk, (index % layout::kObjectArrayItemsPerChunk) * layout::kObjectArrayItemSizeBytes + layout::kObjectArrayItemObjectPointerOffset) : nullptr;
}

const uint8_t* ItemOf(int32_t index) {
    if (!gObjects || index < 0 || index >= NumObjects()) return nullptr;
    auto** chunks = At<uint8_t**>(gObjects, layout::kObjectArrayChunkListOffset);
    uint8_t* chunk = chunks ? chunks[index / layout::kObjectArrayItemsPerChunk] : nullptr;
    return chunk ? chunk + (index % layout::kObjectArrayItemsPerChunk) * layout::kObjectArrayItemSizeBytes : nullptr;
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

// --- type dump -------------------------------------------------------------------------------------------------------
// Every class, struct and enum in memory, written as a .usmap (the type mappings tools like FModel and CUE4Parse need
// to read the game's cooked assets) and as a readable listing with offsets and function signatures. Container fields
// are read at the offsets in layout.hpp; every pointer followed is checked readable first and must lead to a field
// whose type name ends in "Property", and the count of any that do not is reported, so a wrong offset shows up as
// failures rather than a fault.
namespace {

struct TypeDump {
    std::vector<std::string> names;
    std::map<std::string, uint32_t> nameIndex;
    std::string listing;
    int badFields = 0;

    uint32_t NameIdx(const std::string& s) {
        auto it = nameIndex.find(s);
        if (it != nameIndex.end()) return it->second;
        names.push_back(s);
        return nameIndex[s] = static_cast<uint32_t>(names.size() - 1);
    }
    template <class T>
    void Put(std::string& out, T v) { out.append(reinterpret_cast<const char*>(&v), sizeof v); }
};

std::string FieldKind(const uint8_t* field) {
    const uint8_t* fieldClass = At<uint8_t*>(field, layout::kFFieldTypeOffset);
    return fieldClass ? Name(At<uint32_t>(fieldClass, layout::kFFieldTypeNameOffset), At<int32_t>(fieldClass, layout::kFFieldTypeNameOffset + 4)) : "";
}

// A field pointer stored at `offset` inside `field`, or null if it is not a readable FProperty.
const uint8_t* SubField(const uint8_t* field, int offset, TypeDump& d) {
    const uintptr_t p = At<uintptr_t>(field, offset);
    if (!Readable(p, 0x48) || !Readable(ReadPointer(p + layout::kFFieldTypeOffset), 0x10)) {
        ++d.badFields;
        return nullptr;
    }
    const std::string kind = FieldKind(reinterpret_cast<const uint8_t*>(p));
    if (kind.size() < 8 || kind.compare(kind.size() - 8, 8, "Property") != 0) {
        ++d.badFields;
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(p);
}

Obj SubObject(const uint8_t* field, int offset) {
    const uintptr_t p = At<uintptr_t>(field, offset);
    return Readable(p, 0x30) ? reinterpret_cast<Obj>(p) : nullptr;
}

// Appends the usmap type of a property (EPropertyType codes, the order usmap readers use) and returns it as text.
std::string WriteType(const uint8_t* field, TypeDump& d, std::string& out) {
    enum : uint8_t { Byte, Bool, Int, Float, Object, NameT, Delegate, Double, Array, Struct, Str, Text, Interface, Multicast,
                     WeakObject, LazyObject, AssetObject, SoftObject, UInt64, UInt32, UInt16, Int64, Int16, Int8, Map, Set,
                     Enum, FieldPath, Optional, Utf8Str, AnsiStr, Unknown = 0xFF };
    if (!field) {
        d.Put(out, uint8_t{Unknown});
        return "?";
    }
    const std::string kind = FieldKind(field);
    static const std::map<std::string, uint8_t> simple = {
        {"BoolProperty", Bool}, {"IntProperty", Int}, {"FloatProperty", Float}, {"ObjectProperty", Object},
        {"ClassProperty", Object}, {"ObjectPtrProperty", Object}, {"ClassPtrProperty", Object}, {"NameProperty", NameT},
        {"DelegateProperty", Delegate}, {"DoubleProperty", Double}, {"StrProperty", Str}, {"TextProperty", Text},
        {"InterfaceProperty", Interface}, {"MulticastDelegateProperty", Multicast}, {"MulticastInlineDelegateProperty", Multicast},
        {"MulticastSparseDelegateProperty", Multicast}, {"WeakObjectProperty", WeakObject}, {"LazyObjectProperty", LazyObject},
        {"SoftObjectProperty", SoftObject}, {"SoftClassProperty", SoftObject}, {"UInt64Property", UInt64},
        {"UInt32Property", UInt32}, {"UInt16Property", UInt16}, {"Int64Property", Int64}, {"Int16Property", Int16},
        {"Int8Property", Int8}, {"FieldPathProperty", FieldPath}, {"Utf8StrProperty", Utf8Str}, {"AnsiStrProperty", AnsiStr}};
    if (auto it = simple.find(kind); it != simple.end()) {
        d.Put(out, it->second);
        const std::string shortKind = kind.substr(0, kind.size() - 8);
        if (kind == "ObjectProperty" || kind == "ClassProperty" || kind == "WeakObjectProperty" || kind == "SoftObjectProperty")
            if (Obj cls = SubObject(field, layout::kFObjectPropertyClassOffset)) return shortKind + "<" + ObjName(cls) + ">";
        return shortKind;
    }
    if (kind == "ByteProperty") {
        Obj e = SubObject(field, layout::kFBytePropertyEnumOffset);
        if (!e) {
            d.Put(out, uint8_t{Byte});
            return "Byte";
        }
        d.Put(out, uint8_t{Enum});
        d.Put(out, uint8_t{Byte});
        d.Put(out, d.NameIdx(ObjName(e)));
        return "Byte<" + ObjName(e) + ">";
    }
    if (kind == "EnumProperty") {
        d.Put(out, uint8_t{Enum});
        const std::string under = WriteType(SubField(field, layout::kFEnumPropertyUnderlyingOffset, d), d, out);
        Obj e = SubObject(field, layout::kFEnumPropertyEnumOffset);
        d.Put(out, d.NameIdx(e ? ObjName(e) : "None"));
        return "Enum<" + (e ? ObjName(e) : std::string("?")) + ":" + under + ">";
    }
    if (kind == "StructProperty") {
        Obj s = SubObject(field, layout::kFStructPropertyStructTypeOffset);
        d.Put(out, uint8_t{Struct});
        d.Put(out, d.NameIdx(s ? ObjName(s) : "None"));
        return "Struct<" + (s ? ObjName(s) : std::string("?")) + ">";
    }
    if (kind == "ArrayProperty" || kind == "SetProperty" || kind == "OptionalProperty") {
        const uint8_t code = kind == "ArrayProperty" ? Array : kind == "SetProperty" ? Set : Optional;
        const int offset = kind == "ArrayProperty" ? layout::kFArrayPropertyInnerOffset
                           : kind == "SetProperty" ? layout::kFSetPropertyElementOffset : layout::kFOptionalPropertyValueOffset;
        d.Put(out, code);
        return kind.substr(0, kind.size() - 8) + "<" + WriteType(SubField(field, offset, d), d, out) + ">";
    }
    if (kind == "MapProperty") {
        d.Put(out, uint8_t{Map});
        const std::string key = WriteType(SubField(field, layout::kFMapPropertyKeyOffset, d), d, out);
        return "Map<" + key + ", " + WriteType(SubField(field, layout::kFMapPropertyValueOffset, d), d, out) + ">";
    }
    d.Put(out, uint8_t{Unknown});
    return kind + "(unmapped)";
}

}  // namespace

std::string DumpTypes(const std::wstring& directory) {
    ListReadableMemory();
    TypeDump d;
    Obj structClass = FindClass("Struct"), functionClass = FindClass("Function"), enumClass = FindClass("Enum");
    if (!structClass || !functionClass || !enumClass) return "dump: core classes not found";
    std::vector<Obj> structs, enums;
    ForEachObject([&](Obj o) {
        if (IsDefaultObject(o)) return true;
        if (IsA(o, enumClass)) enums.push_back(o);
        else if (IsA(o, structClass) && !IsA(o, functionClass)) structs.push_back(o);
        return true;
    });
    std::string enumBlock, structBlock;
    d.Put(enumBlock, static_cast<uint32_t>(enums.size()));
    for (Obj e : enums) {
        const uintptr_t namesAt = At<uintptr_t>(e, layout::kUEnumEntryNamesOffset) & ~uintptr_t{1};
        const uintptr_t valuesAt = At<uintptr_t>(e, layout::kUEnumEntryValuesOffset) & ~uintptr_t{1};
        int32_t count = At<int32_t>(e, layout::kUEnumEntryCountOffset);
        if (count < 0 || count > 0xFFFF || (count && (!Readable(namesAt, count * 8ull) || !Readable(valuesAt, count * 8ull)))) {
            ++d.badFields;
            count = 0;
        }
        const auto* entryNames = reinterpret_cast<const uint8_t*>(namesAt);
        const auto* entryValues = reinterpret_cast<const uint8_t*>(valuesAt);
        d.Put(enumBlock, d.NameIdx(ObjName(e)));
        d.Put(enumBlock, static_cast<uint16_t>(count));
        d.listing += "enum " + ObjName(e) + "  // " + PathOf(e) + "\n";
        for (int i = 0; i < count; ++i) {
            std::string entry = Name(At<uint32_t>(entryNames, i * 8), At<int32_t>(entryNames, i * 8 + 4));
            if (size_t colons = entry.rfind("::"); colons != std::string::npos) entry = entry.substr(colons + 2);
            const int64_t value = At<int64_t>(entryValues, i * 8);
            d.Put(enumBlock, static_cast<uint64_t>(value));
            d.Put(enumBlock, d.NameIdx(entry));
            d.listing += "  " + entry + " = " + std::to_string(value) + "\n";
        }
    }
    d.Put(structBlock, static_cast<uint32_t>(structs.size()));
    for (Obj s : structs) {
        Obj super = SuperOf(s);
        std::vector<const uint8_t*> fields;
        int schema = 0;
        for (uint8_t* f = At<uint8_t*>(s, layout::kUStructFirstPropertyOffset); f; f = At<uint8_t*>(f, layout::kFFieldNextFieldOffset)) {
            fields.push_back(f);
            schema += std::max(1, At<int32_t>(f, layout::kFPropertyArrayDimOffset));
        }
        d.Put(structBlock, d.NameIdx(ObjName(s)));
        d.Put(structBlock, super ? d.NameIdx(ObjName(super)) : 0xFFFFFFFFu);
        d.Put(structBlock, static_cast<uint16_t>(schema));
        d.Put(structBlock, static_cast<uint16_t>(fields.size()));
        d.listing += "\n" + ObjName(ClassOf(s)) + " " + ObjName(s) + (super ? " : " + ObjName(super) : "") + "  // " + PathOf(s) + "\n";
        int index = 0;
        for (const uint8_t* f : fields) {
            const int dim = std::max(1, At<int32_t>(f, layout::kFPropertyArrayDimOffset));
            d.Put(structBlock, static_cast<uint16_t>(index));
            d.Put(structBlock, static_cast<uint8_t>(dim));
            d.Put(structBlock, d.NameIdx(FieldName(f)));
            const std::string type = WriteType(f, d, structBlock);
            char line[64];
            std::snprintf(line, sizeof line, "  +0x%04x %5d  ", At<int32_t>(f, layout::kFPropertyValueLocationOffset),
                          At<int32_t>(f, layout::kFPropertyValueSizeOffset) * dim);
            d.listing += line + type + " " + FieldName(f) + (dim > 1 ? "[" + std::to_string(dim) + "]" : "") + "\n";
            index += dim;
        }
        // Functions: not part of a usmap, listed for reading.
        for (Obj fn = At<Obj>(s, layout::kUStructFirstFunctionOffset); fn; fn = At<Obj>(fn, layout::kUFieldNextFieldOffset)) {
            if (!IsA(fn, functionClass)) continue;
            std::string args, ret;
            for (uint8_t* f = At<uint8_t*>(fn, layout::kUStructFirstPropertyOffset); f; f = At<uint8_t*>(f, layout::kFFieldNextFieldOffset)) {
                const uint64_t flags = At<uint64_t>(f, layout::kFPropertyFlagsOffset);
                if (!(flags & layout::kPropertyFlagIsParameter)) continue;
                std::string scratch;
                const std::string type = WriteType(f, d, scratch);
                if (flags & layout::kPropertyFlagIsReturnValue) ret = " -> " + type;
                else args += (args.empty() ? "" : ", ") + type + ((flags & layout::kPropertyFlagIsOutParameter) ? "& " : " ") + FieldName(f);
            }
            d.listing += "  fn " + ObjName(fn) + "(" + args + ")" + ret + "\n";
        }
    }
    // The name table, then enums and structs, uncompressed; version 4 (explicit enum values), no package versioning.
    std::string nameBlock;
    d.Put(nameBlock, static_cast<uint32_t>(d.names.size()));
    for (const auto& n : d.names) {
        d.Put(nameBlock, static_cast<uint16_t>(n.size()));
        nameBlock += n;
    }
    const std::string body = nameBlock + enumBlock + structBlock;
    std::string file;
    d.Put(file, uint16_t{0x30C4});
    d.Put(file, uint8_t{4});
    d.Put(file, int32_t{0});
    d.Put(file, uint8_t{0});
    d.Put(file, static_cast<uint32_t>(body.size()));
    d.Put(file, static_cast<uint32_t>(body.size()));
    file += body;
    CreateDirectoryW(directory.c_str(), nullptr);
    auto write = [](const std::wstring& path, const std::string& data) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
        CloseHandle(h);
        return ok;
    };
    const bool ok = write(directory + L"\\Ballest.usmap", file) && write(directory + L"\\types.txt", d.listing);
    return std::string(ok ? "dump written: " : "dump: could not write files; ") + std::to_string(structs.size()) +
           " structs/classes, " + std::to_string(enums.size()) + " enums, " + std::to_string(d.names.size()) + " names; " +
           std::to_string(d.badFields) + " container fields unreadable";
}

bool ReadMemory(uintptr_t address, void* out, size_t bytes) {
    ListReadableMemory();
    if (!Readable(address, bytes)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
    return true;
}

}  // namespace eng
