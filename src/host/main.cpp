// Entry point. The game loads this DLL as version.dll. DllMain only resolves the real version.dll exports and, in
// the game process, starts the init thread. The init thread checks the build, waits for the engine, then hooks
// the viewport client's per-frame Tick by giving that one object a copy of its vtable (no game code is patched).
// Everything after that runs on the game thread, once per frame, in HostFrame.
#include <windows.h>

#include <cstring>
#include <string>

#include "sandbox.hpp"
#include "draw.hpp"
#include "postprocess.hpp"
#include "ghosts.hpp"
#include "steam.hpp"
#include "tracks.hpp"
#include "workshop.hpp"
#include "hub.hpp"
#include "cosmetics.hpp"
#include "models.hpp"
#include "editor.hpp"
#include "engine.hpp"
#include "game.hpp"
#include "hud.hpp"
#include "input.hpp"
#include "leaderboard.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "plugins.hpp"
#include "race.hpp"
#include "registry.hpp"
#include "replay.hpp"
#include "testchannel.hpp"
#include "ui.hpp"

bool ResolveVersionExports();       // src/proxy/exports.cpp

namespace {

using TickFn = void (*)(void* self, float deltaSeconds);
TickFn gOriginalTick = nullptr;

// --- the fault guard ---------------------------------------------------------------------------------------------
// Each frame the host's work starts from a saved point (RtlCaptureContext). An access violation (or other hardware
// fault) whose instruction is in this DLL, on the game thread during the host's frame, resumes at that point instead
// of taking the game down: the plugin that was running is stopped until restart, or, when none was, the host turns
// itself off for the session. Typical cause: a game update that moved something the host reads. Faults in the game's
// own code are left to the game, because resuming there could leave the engine in a broken state.
alignas(16) CONTEXT gGuard;
volatile bool gGuardActive = false, gFaulted = false;
volatile uintptr_t gFaultAt = 0;
DWORD gGuardThread = 0;
uintptr_t gHostStart = 0, gHostEnd = 0;
bool gHostOff = false;

LONG CALLBACK OnFault(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (!gGuardActive || GetCurrentThreadId() != gGuardThread) return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED && code != EXCEPTION_DATATYPE_MISALIGNMENT)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t at = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    if (at < gHostStart || at >= gHostEnd) return EXCEPTION_CONTINUE_SEARCH;
    gGuardActive = false;
    gFaulted = true;
    gFaultAt = at;
    *e->ContextRecord = gGuard;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void InstallFaultGuard() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&OnFault), &self);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(self);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uintptr_t>(self) + dos->e_lfanew);
    gHostStart = reinterpret_cast<uintptr_t>(self);
    gHostEnd = gHostStart + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, &OnFault);
}

void AfterFault() {
    const std::string where = "host +" + hostlog::Hex(gFaultAt - gHostStart);
    if (plugins::RecoverFromFault(where)) return;               // logged as that plugin's status
    gHostOff = true;
    hostlog::Error("the host crashed (" + where + "); plugins are off until the game restarts");
}
std::wstring gGameDir, gPluginsDir;
bool gSlotHost = false;                     // this is a test copy's own host build (sandbox::OwnHostPath)
bool gInFrame = false, gPluginsLoaded = false;

std::wstring ExePath() {
    wchar_t buf[MAX_PATH];
    return std::wstring(buf, GetModuleFileNameW(nullptr, buf, MAX_PATH));
}

// The order matters: input and the world first, then the game state plugins read, then UI, then plugins.
void HostFrame(float dt) {
    if (!gPluginsLoaded) {
        gPluginsLoaded = true;
        hostlog::Info("first frame on the game thread; loading plugins");
        plugins::LoadAll(gPluginsDir);
        registry::Refresh();
    }
    input::Frame();
    game::Frame();
    editor::Frame();
    race::Frame();
    steam::Frame();
    ghosts::Frame();
    draw::Frame();
    postprocess::Frame();
    tracks::Frame();
    workshop::Frame();
    hub::Frame();
    hud::Frame();
    replay::Frame(dt);
    cosmetics::Frame();
    models::ProbeFrame();
    leaderboard::Frame();
    ui::Frame();
    registry::Frame();              // installs and removals land between frames of plugin code
    plugins::Frame(dt);
    testchannel::Frame();
}

void HookedTick(void* self, float deltaSeconds) {
    gOriginalTick(self, deltaSeconds);
    if (gInFrame || gHostOff) return;       // never re-entered from inside our own engine calls
    gInFrame = true;
    gFaulted = false;
    gGuardThread = GetCurrentThreadId();
    RtlCaptureContext(&gGuard);             // a fault in host code resumes here, with gFaulted set
    if (gFaulted) {
        AfterFault();
        gInFrame = false;
        return;
    }
    gGuardActive = true;
    HostFrame(deltaSeconds);
    gGuardActive = false;
    gInFrame = false;
}

eng::Obj FindViewportClient() {
    eng::Obj cls = eng::FindClass("CommonGameViewportClient");
    eng::Obj found = nullptr;
    eng::ForEachObject([&](eng::Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o)) found = o;
        return found == nullptr;
    });
    return found;
}

bool HookViewportTick(eng::Obj client) {
    void** vtable = *reinterpret_cast<void***>(client);
    // On the measured build the slot must hold the measured Tick; on another build it must at least be game code.
    void* expected = reinterpret_cast<void*>(eng::Base() + layout::kViewportClientTickFunctionOffsetInExe);
    void* slot = vtable[layout::kViewportClientTickVtableSlot];
    if (eng::KnownBuild() ? slot != expected : !eng::InImage(slot)) {
        hostlog::Error("viewport Tick slot holds " + hostlog::Hex(reinterpret_cast<uintptr_t>(vtable[layout::kViewportClientTickVtableSlot])) +
                       ", expected " + hostlog::Hex(reinterpret_cast<uintptr_t>(expected)) + "; not hooking");
        return false;
    }
    int count = 0;
    while (count < 4096 && eng::InImage(vtable[count])) ++count;
    // One extra slot in front: MSVC keeps the RTTI locator at vtable[-1].
    auto** copy = static_cast<void**>(VirtualAlloc(nullptr, (count + 1) * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!copy) return false;
    copy[0] = vtable[-1];
    std::memcpy(copy + 1, vtable, count * sizeof(void*));
    gOriginalTick = reinterpret_cast<TickFn>(vtable[layout::kViewportClientTickVtableSlot]);
    copy[1 + layout::kViewportClientTickVtableSlot] = reinterpret_cast<void*>(&HookedTick);
    game::SetViewportClient(client);
    *reinterpret_cast<void***>(client) = copy + 1;      // one pointer write: the game thread sees old or new
    hostlog::Info("per-frame hook installed on " + eng::PathOf(client) + " (vtable of " + std::to_string(count) + " entries copied)");
    return true;
}

DWORD WINAPI InitThread(LPVOID) {
    const std::wstring exe = ExePath();
    gGameDir = exe.substr(0, exe.find_last_of(L'\\'));
    hostlog::Open();
    hostlog::Info(std::string("Ballest plugin host ") + plugins::kHostVersion + " starting");
    // A sandboxed test copy with a plugins folder of its own (in its data folder) runs those plugins, so copies
    // being worked on side by side don't share one set; and it leaves the shared install (version.dll) alone.
    gPluginsDir = gGameDir + L"\\plugins";
    if (sandbox::On()) {
        const std::wstring own = hostlog::DataDir() + L"\\plugins";
        const DWORD attributes = GetFileAttributesW(own.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) gPluginsDir = own;
        if (gSlotHost) hostlog::Info("sandbox: running the copy's own host build");
        hostlog::Info("sandbox: plugins from " + eng::Narrow(gPluginsDir.c_str(), static_cast<int>(gPluginsDir.size())));
    } else {
        registry::CleanUpOldHost(gGameDir);  // the previous version.dll, if an update replaced it
    }
    if (GetFileAttributesW((gPluginsDir + L"\\DISABLED").c_str()) != INVALID_FILE_ATTRIBUTES) {
        hostlog::Info("plugins\\DISABLED exists; host stays inactive");
        return 0;
    }
    game::EarlyWindowFit(gPluginsDir);          // before the engine is up: the window appears long before plugins run
    if (sandbox::On()) hostlog::Info("sandbox mode is on for this session: nothing leaves this computer or changes your records");
    // Steam's interfaces appear during engine start: the sandbox's blocks, or in a player's game the upload guard
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            for (int i = 0; i < 6000 && !sandbox::InstallSteam(); ++i) Sleep(20);
            if (!sandbox::InstallSteam())
                hostlog::Error(sandbox::On() ? "sandbox: Steam never started; its uploads are NOT blocked" : "upload guard: Steam never started");
            if (!sandbox::On()) return 0;
            for (int i = 0; i < 6; ++i) {            // the game starts its audio during engine start
                const bool muted = sandbox::MuteAudio();
                if (i == 0 || !muted) hostlog::Info(std::string("sandbox: sound ") + (muted ? "muted" : "NOT MUTED"));
                Sleep(5000);
            }
            return 0;
        }, nullptr, 0, nullptr));
    if (!eng::Init(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))) return 0;
    InstallFaultGuard();
    for (int attempt = 0; attempt < 600; ++attempt) {        // up to five minutes
        Sleep(500);
        if (!eng::Locate()) continue;
        if (eng::NumObjects() < 1000) continue;
        if (eng::Obj client = FindViewportClient()) {
            hostlog::Info("engine ready after " + std::to_string((attempt + 1) / 2) + " s, " + std::to_string(eng::NumObjects()) + " objects");
            HookViewportTick(client);
            return 0;
        }
    }
    hostlog::Error("no viewport client appeared; host inactive");
    return 0;
}

}  // namespace

// Whether a host build has the sandbox compiled in (its status line's text is in the file).
bool HasSandbox(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string bytes;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64LL << 20)) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) bytes.clear();
        bytes.resize(read);
    }
    CloseHandle(f);
    return bytes.find("sandbox: status ") != std::string::npos && bytes.find("the game's leaderboard interface not seen yet") != std::string::npos;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        ResolveVersionExports();
        // Only the game itself gets the host; any other program that loads this DLL just gets version.dll.
        const std::wstring exe = ExePath();
        if (_wcsicmp(exe.substr(exe.find_last_of(L'\\') + 1).c_str(), L"Ballest-Win64-Shipping.exe") == 0)
        {
            // A sandboxed test copy can run a host build of its own (tools/test_instance.py --host): loaded in place
            // of this one, which then stays out of the way, so a host change is tried in a slot while every other
            // copy (and the player's game) runs the installed host. That build runs this again, as itself.
            if (sandbox::Flagged()) {
                wchar_t self[MAX_PATH];
                GetModuleFileNameW(module, self, MAX_PATH);
                const std::wstring own = sandbox::OwnHostPath();
                gSlotHost = _wcsicmp(self, own.c_str()) == 0;
                // Only a build that has the sandbox in it: a build without (releases before 0.23.5 have none) would run
                // this copy unsandboxed. Checked by the sandbox's own status text in the file (accidents, not tampering).
                if (!gSlotHost && GetFileAttributesW(own.c_str()) != INVALID_FILE_ATTRIBUTES && HasSandbox(own) && LoadLibraryW(own.c_str()))
                    return TRUE;
            }
            sandbox::InstallEarly();         // before any of the game's own code runs
            CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
        }
    }
    return TRUE;
}
