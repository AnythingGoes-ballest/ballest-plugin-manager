// Entry point. The game loads this DLL as version.dll. DllMain only resolves the real version.dll exports and, in
// the game process, starts the init thread. The init thread checks the build, waits for the engine, then hooks
// the viewport client's per-frame Tick by giving that one object a copy of its vtable (no game code is patched).
// Everything after that runs on the game thread, once per frame, in HostFrame.
#include <windows.h>

#include <cstring>
#include <string>

#include "cosmetics.hpp"
#include "editor.hpp"
#include "engine.hpp"
#include "game.hpp"
#include "input.hpp"
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
std::wstring gGameDir;
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
        plugins::LoadAll(gGameDir + L"\\plugins");
        registry::Refresh();
    }
    input::Frame();
    game::Frame();
    editor::Frame();
    race::Frame();
    replay::Frame(dt);
    cosmetics::Frame();
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
    registry::CleanUpOldHost(gGameDir);      // the previous version.dll, if an update replaced it
    if (GetFileAttributesW((gGameDir + L"\\plugins\\DISABLED").c_str()) != INVALID_FILE_ATTRIBUTES) {
        hostlog::Info("plugins\\DISABLED exists; host stays inactive");
        return 0;
    }
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

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        ResolveVersionExports();
        // Only the game itself gets the host; any other program that loads this DLL just gets version.dll.
        const std::wstring exe = ExePath();
        if (_wcsicmp(exe.substr(exe.find_last_of(L'\\') + 1).c_str(), L"Ballest-Win64-Shipping.exe") == 0)
            CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
