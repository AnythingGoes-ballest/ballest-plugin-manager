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
constexpr uintptr_t kMountPakDelegateRva = 0xA405400;
constexpr uintptr_t kHandleMountPakRva   = 0x169E8D0;   // FPakPlatformFile::HandleMountPakDelegate(const FString&, int32)
constexpr uintptr_t kSigningKeysDelegRva = 0xA4052C0;
constexpr int kPakFileSignedFieldOffset  = 0x30;
// FShaderCodeLibrary::OpenLibrary(const FString& Name, const FString& Directory, bool bMonolithicOnly): a thin
// wrapper (mov rax,rcx; mov rcx,[Impl]; test; je; movzx r9d,r8b; mov r8,rdx; mov rdx,rax; jmp Impl::OpenLibrary),
// found by matching the exported UE 5.8.3 editor RenderCore code shape; unique match in the Shipping exe.
constexpr uintptr_t kOpenShaderLibraryRva = 0x3308570;

struct FString { const wchar_t* Data; int32_t Num; int32_t Max; };  // UE FString layout
// HandleMountPakDelegate(this, const FString& PakFilePath, int32 PakOrder) -> IPakFile* (null on failure).
// It builds the internal mount request and calls the real Mount; we call it so we don't have to reconstruct that struct.
using MountFn = void*(__fastcall*)(void* self, const FString* path, int32_t order);
using OpenLibFn = bool(__fastcall*)(const FString* name, const FString* dir, bool monolithicOnly);

void* PakPlatformFile() {
    const uintptr_t base = eng::Base();
    auto* instance = *reinterpret_cast<void**>(base + kMountPakDelegateRva);  // FDelegateInstance*
    if (!instance) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(instance) + 0x18);  // bound FPakPlatformFile*
}

}  // namespace

bool MountContainer(const std::wstring& pakAbsolutePath, int32_t order) {
    const uintptr_t base = eng::Base();
    // The RVAs are measured on Ballest CL 56702186. Rather than trust a stored fingerprint, self-verify the code
    // we are about to call matches what was measured: the Mount prologue bytes, and the MountPak delegate's bound
    // function being HandleMountPakDelegate at its measured RVA. If either differs, this is another build.
    static const uint8_t kHandlePrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48};  // HandleMountPakDelegate prologue
    if (std::memcmp(reinterpret_cast<void*>(base + kHandleMountPakRva), kHandlePrologue, sizeof kHandlePrologue) != 0) {
        hostlog::Info("[assets] refused: HandleMountPakDelegate prologue does not match the measured build");
        return false;
    }
    auto* instance = *reinterpret_cast<void**>(base + kMountPakDelegateRva);
    if (!instance || *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(instance) + 0x20) != base + 0x169e8d0) {
        hostlog::Info("[assets] refused: MountPak delegate is not the measured HandleMountPakDelegate");
        return false;
    }
    void* self = PakPlatformFile();
    if (!self) { hostlog::Info("[assets] could not find FPakPlatformFile"); return false; }

    {
        DWORD attr = GetFileAttributesW(pakAbsolutePath.c_str());
        std::wstring utoc = pakAbsolutePath.substr(0, pakAbsolutePath.size() - 4) + L".utoc";
        hostlog::Info("[assets] files: pak attr=" + std::to_string((int)attr) + " utoc attr=" +
                      std::to_string((int)GetFileAttributesW(utoc.c_str())) +
                      " bSigned=" + std::to_string(*(reinterpret_cast<uint8_t*>(self) + kPakFileSignedFieldOffset)) +
                      " signDelegate=" + std::to_string(*reinterpret_cast<uintptr_t*>(base + kSigningKeysDelegRva) != 0));
    }

    auto* delegateInst = reinterpret_cast<void**>(base + kSigningKeysDelegRva);  // signing-keys delegate storage
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

    auto mount = reinterpret_cast<MountFn>(base + kHandleMountPakRva);
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
    const uintptr_t base = eng::Base();
    static const uint8_t kWrapper[] = {0x48, 0x8B, 0xC1, 0x48, 0x8B, 0x0D};  // mov rax,rcx; mov rcx,[rip+Impl]
    const uint8_t* at = reinterpret_cast<const uint8_t*>(base + kOpenShaderLibraryRva);
    if (std::memcmp(at, kWrapper, sizeof kWrapper) != 0 || at[0x19] != 0xE9) {
        hostlog::Info("[assets] refused: OpenLibrary wrapper bytes do not match the measured build");
        return false;
    }
    const int32_t implDisp = *reinterpret_cast<const int32_t*>(at + 6);
    void* impl = *reinterpret_cast<void* const*>(at + 10 + implDisp);
    FString fname{ name.c_str(), static_cast<int32_t>(name.size() + 1), static_cast<int32_t>(name.size() + 1) };
    FString fdir{ directory.c_str(), static_cast<int32_t>(directory.size() + 1), static_cast<int32_t>(directory.size() + 1) };
    const bool ok = reinterpret_cast<OpenLibFn>(base + kOpenShaderLibraryRva)(&fname, &fdir, false);
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
