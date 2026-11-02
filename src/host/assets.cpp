// Plugin cooked content: see assets.hpp and docs/custom-assets-recipe.md.
#include "assets.hpp"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cosmetics.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "modcontainer.hpp"

namespace assets {
namespace {

// Measured on Ballest-Win64-Shipping.exe, UE 5.8 CL 56702186 (each checked by its bytes before use):
//   - FCoreDelegates::MountPak delegate storage; its instance+0x18 is the live FPakPlatformFile*, instance+0x20
//     is HandleMountPakDelegate (verified at runtime with tools/memread.py).
//   - HandleMountPakDelegate builds the request the internal FPakPlatformFile::Mount takes (a struct, not a path)
//     and calls it; calling the delegate's own target means we don't rebuild that struct.
//   - FPakPlatformFile::bSigned at this+0x30 (set in Initialize from the signing-keys delegate).
//   - GetPakSigningKeysDelegate() returns the delegate at this RVA; its instance ptr (offset 0) being null means
//     "not bound", so signature checks are skipped.
//
// Each game update moves these. The measured RVAs below are tried first; when their bytes don't match, the same
// things are found again in the running game (FindAddresses) by the shapes that identify them, and used only when
// exactly one candidate fits every check.
//   2026-10-xx (CL 56702186): MountPak 0xA405400, HandleMountPak 0x169E8D0, signing 0xA4052C0, OpenLibrary 0x3308570
//   2026-11-01 update:        MountPak 0xA706D80, HandleMountPak 0x16A2CB0, signing 0xA706C40, OpenLibrary 0x330F470
constexpr uintptr_t kMountPakDelegateRva = 0xA706D80;
constexpr uintptr_t kHandleMountPakRva   = 0x16A2CB0;   // FPakPlatformFile::HandleMountPakDelegate(const FString&, int32)
// FCoreDelegates statics: the signing-keys delegate is declared 0x140 before MountPak, and MountPak is followed by
// OnUnmountPak (+0x20), which FPakPlatformFile binds to itself too (measured in both builds above).
constexpr uintptr_t kSigningBeforeMount  = 0x140;
constexpr uintptr_t kUnmountAfterMount   = 0x20;
constexpr int kPakFileSignedFieldOffset  = 0x30;
// FShaderCodeLibrary::OpenLibrary(const FString& Name, const FString& Directory, bool bMonolithicOnly): a thin
// wrapper (mov rax,rcx; mov rcx,[Impl]; test; je; movzx r9d,r8b; mov r8,rdx; mov rdx,rax; jmp Impl::OpenLibrary),
// found by matching the exported UE 5.8.3 editor RenderCore code shape; unique match in the Shipping exe.
constexpr uintptr_t kOpenShaderLibraryRva = 0x330F470;
static const uint8_t kHandlePrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48};  // HandleMountPakDelegate prologue

struct FString { const wchar_t* Data; int32_t Num; int32_t Max; };  // UE FString layout
// HandleMountPakDelegate(this, const FString& PakFilePath, int32 PakOrder) -> IPakFile* (null on failure).
// It builds the internal mount request and calls the real Mount; we call it so we don't have to reconstruct that struct.
using MountFn = void*(__fastcall*)(void* self, const FString* path, int32_t order);
using OpenLibFn = bool(__fastcall*)(const FString* name, const FString* dir, bool monolithicOnly);

// Reads that fail instead of faulting (heap pointers found while searching may be anything).
bool Peek(uintptr_t at, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(at), out, n, &got) && got == n;
}

struct Section { uintptr_t lo = 0, hi = 0; bool Has(uintptr_t a) const { return a >= lo && a < hi; } };
Section SectionOf(const char* name) {
    const uintptr_t base = eng::Base();
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (std::strncmp(reinterpret_cast<const char*>(s->Name), name, 8) == 0)
            return {base + s->VirtualAddress, base + s->VirtualAddress + s->Misc.VirtualSize};
    return {};
}

std::string Hex(uintptr_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

struct Addresses {
    uintptr_t mountSlot = 0, handleMount = 0, signingSlot = 0, openLibrary = 0;
    bool ok = false;
};

// A delegate slot bound by FPakPlatformFile: slot -> instance (heap) -> {+0x18 object, +0x20 method}.
bool BoundInstance(uintptr_t slot, const Section& text, uintptr_t* object, uintptr_t* method) {
    uintptr_t inst = 0;
    if (!Peek(slot, &inst, 8) || !inst) return false;
    uintptr_t pair[2];
    if (!Peek(inst + 0x18, pair, sizeof pair) || !pair[0] || !text.Has(pair[1])) return false;
    *object = pair[0];
    *method = pair[1];
    return true;
}

bool OpenLibraryShape(uintptr_t at) {
    static const uint8_t kHead[] = {0x48, 0x8B, 0xC1, 0x48, 0x8B, 0x0D};  // mov rax,rcx; mov rcx,[rip+Impl]
    static const uint8_t kTail[] = {0x48, 0x85, 0xC9, 0x74};              // test rcx,rcx; je
    static const uint8_t kArgs[] = {0x45, 0x0F, 0xB6, 0xC8, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0xD0, 0xE9};
    const auto* p = reinterpret_cast<const uint8_t*>(at);
    return std::memcmp(p, kHead, sizeof kHead) == 0 && std::memcmp(p + 10, kTail, sizeof kTail) == 0 &&
           std::memcmp(p + 15, kArgs, sizeof kArgs) == 0;
}

bool MountSlotShape(uintptr_t slot, const Section& text, const Section& image, uintptr_t* handle) {
    uintptr_t obj = 0, fn = 0, obj2 = 0, fn2 = 0, signing = 0;
    if (!BoundInstance(slot, text, &obj, &fn) || image.Has(obj)) return false;
    if (!BoundInstance(slot + kUnmountAfterMount, text, &obj2, &fn2) || obj2 != obj || fn2 == fn) return false;
    if (!Peek(slot - kSigningBeforeMount, &signing, 8) || !signing) return false;
    uint8_t prologue[sizeof kHandlePrologue], isSigned = 2;
    if (!Peek(fn, prologue, sizeof prologue) || std::memcmp(prologue, kHandlePrologue, sizeof prologue) != 0) return false;
    if (!Peek(obj + kPakFileSignedFieldOffset, &isSigned, 1) || isSigned > 1) return false;
    *handle = fn;
    return true;
}

const Addresses& FindAddresses() {
    static Addresses a;
    static bool done = false;
    if (done) return a;
    done = true;
    const uintptr_t base = eng::Base();
    const Section text = SectionOf(".text"), data = SectionOf(".data");
    const Section image{base, data.hi};
    if (!text.lo || !data.lo) { hostlog::Info("[assets] no .text/.data sections"); return a; }

    uintptr_t handle = 0;
    if (MountSlotShape(base + kMountPakDelegateRva, text, image, &handle) && handle == base + kHandleMountPakRva) {
        a.mountSlot = base + kMountPakDelegateRva;
    } else {
        int found = 0;
        for (uintptr_t s = data.lo + kSigningBeforeMount; s + kUnmountAfterMount + 8 <= data.hi; s += 8) {
            uintptr_t h = 0;
            if (MountSlotShape(s, text, image, &h)) { ++found; a.mountSlot = s; handle = h; }
        }
        if (found != 1) {
            hostlog::Info("[assets] MountPak delegate not found in this build (" + std::to_string(found) + " candidates)");
            return a;
        }
        hostlog::Info("[assets] MountPak delegate found again at rva " + Hex(a.mountSlot - base));
    }
    a.handleMount = handle;
    a.signingSlot = a.mountSlot - kSigningBeforeMount;

    if (OpenLibraryShape(base + kOpenShaderLibraryRva)) {
        a.openLibrary = base + kOpenShaderLibraryRva;
    } else {
        int found = 0;
        for (uintptr_t p = text.lo; p + 26 <= text.hi; ++p)
            if (*reinterpret_cast<const uint8_t*>(p) == 0x48 && OpenLibraryShape(p)) { ++found; a.openLibrary = p; }
        if (found != 1) {
            hostlog::Info("[assets] OpenLibrary not found in this build (" + std::to_string(found) + " candidates)");
            a.openLibrary = 0;
            return a;
        }
        hostlog::Info("[assets] OpenLibrary found again at rva " + Hex(a.openLibrary - base));
    }
    a.ok = true;
    return a;
}

}  // namespace

bool MountContainer(const std::wstring& pakAbsolutePath, int32_t order) {
    // The addresses are measured per game build (see above); FindAddresses checks each by its shape before use,
    // and refuses rather than calling something that doesn't fit.
    const Addresses& at = FindAddresses();
    if (!at.ok) {
        hostlog::Info("[assets] refused: the pak mount code of this game build was not found");
        return false;
    }
    uintptr_t obj = 0, fn = 0;
    if (!BoundInstance(at.mountSlot, SectionOf(".text"), &obj, &fn) || fn != at.handleMount) {
        hostlog::Info("[assets] refused: MountPak delegate is not HandleMountPakDelegate");
        return false;
    }
    void* self = reinterpret_cast<void*>(obj);   // the bound FPakPlatformFile

    {
        DWORD attr = GetFileAttributesW(pakAbsolutePath.c_str());
        std::wstring utoc = pakAbsolutePath.substr(0, pakAbsolutePath.size() - 4) + L".utoc";
        hostlog::Info("[assets] files: pak attr=" + std::to_string((int)attr) + " utoc attr=" +
                      std::to_string((int)GetFileAttributesW(utoc.c_str())) +
                      " bSigned=" + std::to_string(*(reinterpret_cast<uint8_t*>(self) + kPakFileSignedFieldOffset)) +
                      " signDelegate=" + std::to_string(*reinterpret_cast<uintptr_t*>(at.signingSlot) != 0));
    }

    auto* delegateInst = reinterpret_cast<void**>(at.signingSlot);  // signing-keys delegate storage
    auto* signedByte = reinterpret_cast<uint8_t*>(self) + kPakFileSignedFieldOffset;

    // Get past the signature check for OUR mount only: make the signing-keys delegate look unbound (so
    // IsSigningEnabled() is false and the IoStore TOC read skips signatures) and clear the pak-side bSigned flag.
    // The game's own containers are already mounted and validated; this is restored immediately after.
    void* savedDelegate = *delegateInst;
    uint8_t savedSigned = *signedByte;
    DWORD oldProtect = 0;
    VirtualProtect(delegateInst, sizeof(void*), PAGE_READWRITE, &oldProtect);
    *delegateInst = nullptr;
    *signedByte = 0;

    auto mount = reinterpret_cast<MountFn>(at.handleMount);
    FString fpath{ pakAbsolutePath.c_str(), static_cast<int32_t>(pakAbsolutePath.size() + 1),
                   static_cast<int32_t>(pakAbsolutePath.size() + 1) };
    // The host's per-frame vectored exception handler guards against a fault in here.
    bool ok = mount(self, &fpath, order) != nullptr;

    *delegateInst = savedDelegate;  // restore before anything else can observe it
    *signedByte = savedSigned;
    VirtualProtect(delegateInst, sizeof(void*), oldProtect, &oldProtect);

    hostlog::Info("[assets] mount order " + std::to_string(order) + ": HandleMountPakDelegate returned " +
                  (ok ? std::string("non-null (mounted)") : std::string("null")));
    return ok;
}

bool OpenShaderLibrary(const std::wstring& name, const std::wstring& directory) {
    const Addresses& found = FindAddresses();
    if (!found.ok || !OpenLibraryShape(found.openLibrary)) {
        hostlog::Info("[assets] refused: OpenLibrary of this game build was not found");
        return false;
    }
    const uint8_t* at = reinterpret_cast<const uint8_t*>(found.openLibrary);
    const int32_t implDisp = *reinterpret_cast<const int32_t*>(at + 6);
    void* impl = *reinterpret_cast<void* const*>(at + 10 + implDisp);
    FString fname{ name.c_str(), static_cast<int32_t>(name.size() + 1), static_cast<int32_t>(name.size() + 1) };
    FString fdir{ directory.c_str(), static_cast<int32_t>(directory.size() + 1), static_cast<int32_t>(directory.size() + 1) };
    const bool ok = reinterpret_cast<OpenLibFn>(found.openLibrary)(&fname, &fdir, false);
    hostlog::Info("[assets] OpenLibrary(" + std::string(name.begin(), name.end()) + ", " +
                  std::string(directory.begin(), directory.end()) + ") Impl=" + (impl ? "set" : "null") +
                  " -> " + (ok ? "true" : "false"));
    return ok;
}

namespace {
std::set<std::wstring> gMountedContainers;   // .utoc paths mounted this session (a mount can't be undone safely)
std::map<std::string, int> gNewestBuild;      // mod name -> newest mounted build number ("<mod>_b<NNNN>-Windows")
int32_t gNextOrder = 1;                       // each newer build mounts above the last, so its packages win
}

int MountPluginAssets(const std::string& pluginId, const std::wstring& assetsDir) {
    // Every container in <plugin>/assets, oldest build first (names sort by build: "<id>_b0007-Windows.utoc").
    std::vector<std::wstring> tocs;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((assetsDir + L"\\*.utoc").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { tocs.push_back(assetsDir + L"\\" + fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(tocs.begin(), tocs.end());

    int mounted = 0;
    for (const std::wstring& utoc : tocs) {
        if (gMountedContainers.count(utoc)) continue;
        const std::string name = eng::Narrow(utoc.c_str(), static_cast<int>(utoc.size()));
        // SECURITY GATE: only packages under /Game/Mods/<id>/ and the plugin's own shader library, so a plugin can
        // never replace a game or engine asset. Checked from the .utoc alone (directory index + package ids).
        // A plugin id may contain '-', a UE project/folder name can't: the mod name maps '-' to '_'
        // ("elemental-balls" -> project, /Game/Mods/ folder and shader library "elemental_balls").
        std::string mod = pluginId;
        std::replace(mod.begin(), mod.end(), '-', '_');
        std::string err, library;
        if (!modcontainer::Verify(utoc, mod, err, &library)) {
            hostlog::Write("error", pluginId, "assets: container REFUSED (" + err + "): " + name);
            gMountedContainers.insert(utoc);          // don't retry a refused file every load
            continue;
        }
        const std::wstring pak = utoc.substr(0, utoc.size() - 5) + L".pak";
        if (!MountContainer(pak, gNextOrder)) {
            hostlog::Write("error", pluginId, "assets: mount failed: " + name);
            continue;
        }
        gMountedContainers.insert(utoc);
        ++gNextOrder;
        {   // "<dir>\<mod>_b<NNNN>-Windows.utoc": remember the newest build, whose packages live under b<NNNN>/
            const size_t slash = name.find_last_of("\\/");
            const std::string file = name.substr(slash == std::string::npos ? 0 : slash + 1);
            const std::string tag = mod + "_b";
            if (file.rfind(tag, 0) == 0) {
                const int build = std::atoi(file.c_str() + tag.size());
                if (build > gNewestBuild[mod]) gNewestBuild[mod] = build;
            }
        }
        // The build's own shader library. IoStore libraries are de-duplicated by NAME, so every build carries its own
        // name ("<id>_b<N>", cooked with -OverrideShaderLibraryName); reopening an old name would be a no-op.
        if (!library.empty()) {
            const std::wstring lib(library.begin(), library.end());
            OpenShaderLibrary(lib, L"../../../" + lib + L"/Content/");
        }
        hostlog::Write("info", pluginId, "assets: mounted " + name + (library.empty() ? "" : " (shaders: " + library + ")"));
        ++mounted;
    }
    return mounted;
}

std::wstring ResolveModPath(const std::wstring& path) {
    // "/Game/Mods/<mod>/<rest>" -> "/Game/Mods/<mod>/b<NNNN>/<rest>" for the newest mounted build of <mod>: every build
    // is cooked under its own folder, so a newer build is a NEW package the engine loads fresh (an already loaded package
    // of the same name would otherwise be reused, and the host keeps its cosmetics' objects alive).
    const std::wstring prefix = L"/Game/Mods/";
    if (path.rfind(prefix, 0) != 0) return path;
    const size_t slash = path.find(L'/', prefix.size());
    if (slash == std::wstring::npos) return path;
    const std::string mod = eng::Narrow(path.c_str() + prefix.size(), static_cast<int>(slash - prefix.size()));
    const auto it = gNewestBuild.find(mod);
    if (it == gNewestBuild.end()) return path;
    wchar_t folder[16];
    swprintf(folder, 16, L"b%04d/", it->second);
    return path.substr(0, slash + 1) + folder + path.substr(slash + 1);
}

void CollectGarbage() {
    eng::Call(eng::FindCdo("KismetSystemLibrary"), "CollectGarbage");
}

}  // namespace assets
