#include "together.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstdint>
#include <utility>
#include <array>
#include <atomic>
#include <cwctype>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <map>
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "editor.hpp"
#include "layout.hpp"
#include "lobby.hpp"
#include "log.hpp"
#include "sandbox.hpp"
#include "steam.hpp"
#include "tracks.hpp"

using eng::Obj;

namespace together {
std::wstring SavedMapsFolder();
extern bool gJoinPending;
namespace {

using NativeFunction = void (*)(Obj context, uint8_t* frame, void* result);
NativeFunction gOpenOriginal = nullptr;
bool gHookFailed = false, gArmed = false, gReopening = false;
std::string gSuffix = "listen";       // test: what the reopen adds ("" = the same travel again)
std::string gLastTravel;

Obj GameInstance() {
    Obj controller = game::PlayerController();
    if (!controller) return nullptr;
    return eng::Call(eng::FindCdo("GameplayStatics"), "GetGameInstance", controller).ReturnObj();
}

// The travel URL the engine keeps for its next tick (the game instance's FWorldContext's TravelURL, an FString),
// found by what it holds rather than by offset: an FString among the first bytes of a struct the game instance points
// to, whose text starts with `start`. Read-only. "" if none matches.
std::string FindTravelUrl(const std::wstring& start) {
    Obj gi = GameInstance();
    if (!gi || start.empty()) return "";
    for (int at = 0x28; at < 0x400; at += 8) {
        uintptr_t context = 0;
        if (!eng::ReadMemory(reinterpret_cast<uintptr_t>(gi) + at, &context, sizeof context) || context < 0x10000) continue;
        for (int off = 0; off < 0x800; off += 8) {
            struct {
                uintptr_t data;
                int32_t num, max;
            } fs{};
            if (!eng::ReadMemory(context + off, &fs, sizeof fs)) break;
            if (fs.data < 0x10000 || fs.num < static_cast<int32_t>(start.size()) + 1 || fs.num > 4096 || fs.max < fs.num) continue;
            std::wstring text(static_cast<size_t>(fs.num - 1), L'\0');
            if (!eng::ReadMemory(fs.data, text.data(), text.size() * sizeof(wchar_t))) continue;
            if (text.compare(0, start.size(), start) == 0) return eng::Narrow(text.c_str(), static_cast<int>(text.size()));
        }
    }
    return "";
}

// OpenLevel(LevelName, bAbsolute, Options) as the game calls it, through the host (ProcessEvent).
bool CallOpenLevel(const std::string& level, const std::string& options) {
    Obj strings = eng::FindCdo("KismetStringLibrary"), statics = eng::FindCdo("GameplayStatics");
    Obj controller = game::PlayerController();
    if (!strings || !statics || !controller) return false;
    const std::wstring wl = eng::Widen(level), wo = eng::Widen(options);
    const eng::FString fl{wl.c_str(), static_cast<int32_t>(wl.size() + 1), static_cast<int32_t>(wl.size() + 1)};
    const eng::FString fo{wo.c_str(), static_cast<int32_t>(wo.size() + 1), static_cast<int32_t>(wo.size() + 1)};
    const eng::Params name = eng::Call(strings, "Conv_StringToName", fl);
    size_t size = 0;
    const uint8_t* fname = name.Return(&size);
    if (!name.Invoked() || !fname) return false;
    eng::Params open(eng::FunctionOn(statics, "OpenLevel"));
    open.SetArg(0, controller);
    open.SetArg(1, fname, size);
    open.SetArg(2, uint8_t{1});
    if (!options.empty()) open.SetArg(3, fo);
    return eng::Invoke(statics, open);
}

// GameplayStatics::OpenLevel, every call (the game's blueprints call it natively): after the game's own call, an
// armed editor open is made again with "?listen" in its options. The second OpenLevel replaces the first travel
// before the engine's next tick starts it, so the editor loads once, as a listen server, with everything the game set
// up for it (its game mode, the map's text in the game instance).
void OpenLevelHooked(Obj context, uint8_t* frame, void* result) {
    gOpenOriginal(context, frame, result);
    if (gReopening || !gArmed) return;     // only an armed open is looked at (the search reads memory word by word)
    const std::string url = FindTravelUrl(L"Map_LevelEditorMain");
    gLastTravel = url;
    gArmed = false;                     // used up by this travel, whatever it was
    if (url.empty()) return;
    if (!gSuffix.empty() && url.find("?" + gSuffix) != std::string::npos) return;
    const size_t q = url.find('?');
    const std::string level = url.substr(0, q), options = q == std::string::npos ? "" : url.substr(q + 1);
    gReopening = true;
    const bool ok = CallOpenLevel(level, gSuffix.empty() ? options : options.empty() ? gSuffix : options + "?" + gSuffix);
    gReopening = false;
    hostlog::Info(std::string("together: the editor opens as a listen server on port ") + std::to_string(ListenPort()) +
                  (ok ? "" : " (failed: the travel couldn't be made again)") + ": " + FindTravelUrl(L"Map_LevelEditorMain"));
}

void HookOpenLevel() {
    if (gOpenOriginal || gHookFailed) return;
    Obj fn = eng::FindFunction(eng::FindClass("GameplayStatics"), "OpenLevel");
    NativeFunction entry = nullptr;
    if (fn) std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
    const NativeFunction hooked = &OpenLevelHooked;
    if (entry == hooked) return;
    if (!fn || !entry || !eng::InImage(reinterpret_cast<void*>(entry))) {
        gHookFailed = true;
        hostlog::Warn("together: GameplayStatics.OpenLevel is not as measured; editors can't be opened as listen servers");
        return;
    }
    gOpenOriginal = entry;
    std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
    hostlog::Info("together: watching level opens");
}

bool SystemBool(const char* function) {
    Obj controller = game::PlayerController();
    return controller && eng::Call(eng::FindCdo("KismetSystemLibrary"), function, controller).ReturnBool();
}

// A sandboxed test copy can't use Steam's networking (sandbox.cpp blocks every Steam socket), and the game's net driver
// is SteamSocketsNetDriver (DefaultEngine.ini: GameNetDriver = SteamSockets.SteamSocketsNetDriver, falling back to
// OnlineSubsystemUtils.IpNetDriver). So in a test copy the engine's GameNetDriver definition is given its fallback, plain
// UDP, before any world listens or connects: two copies then reach each other on loopback (kTogetherFirstPort). The
// player's own game keeps the game's definition (Steam).
// ...and the plain UDP driver must use the computer's own sockets. UIpNetDriver::GetSocketSubsystem (vtable slot 127,
// measured 2026-11-05: `xor eax,eax; lea rcx,[rsp+38h]; mov [rsp+38h],rax; call ISocketSubsystem::Get`) asks for the
// default subsystem, which OnlineSubsystemSteam has made Steam's (blocked in a test copy: no driver could start). In a
// test copy that slot answers ISocketSubsystem::Get("WINDOWS"), as SteamNetDriver's own passthrough does. The call's
// target is read from those instruction bytes, so nothing is assumed about where Get is.
constexpr int kGetSocketSubsystemSlot = 127;
using GetSubsystemFn = void* (*)(const void* name);
GetSubsystemFn gGetSubsystem = nullptr;
struct FNameBytes {
    uint32_t index;
    int32_t number;
} gWindowsName{};
void* WindowsSocketSubsystem(void*) { return gGetSubsystem(&gWindowsName); }
std::string PatchIpDriverSockets() {
    Obj cdo = eng::FindCdo("IpNetDriver");
    uintptr_t* vtable = nullptr;
    if (cdo) std::memcpy(&vtable, cdo, sizeof vtable);
    if (!vtable) return "no IpNetDriver";
    uintptr_t* slot = vtable + kGetSocketSubsystemSlot;
    const uintptr_t code = *slot;
    if (code == reinterpret_cast<uintptr_t>(&WindowsSocketSubsystem)) return "sockets already the computer's own";
    static const uint8_t kShape[] = {0x48, 0x83, 0xEC, 0x28, 0x33, 0xC0, 0x48, 0x8D, 0x4C, 0x24, 0x38, 0x48, 0x89, 0x44, 0x24, 0x38, 0xE8};
    if (!eng::InImage(reinterpret_cast<void*>(code)) || std::memcmp(reinterpret_cast<const void*>(code), kShape, sizeof kShape) != 0)
        return "GetSocketSubsystem not as measured (left alone)";
    int32_t rel = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(code + sizeof kShape), 4);
    gGetSubsystem = reinterpret_cast<GetSubsystemFn>(code + sizeof kShape + 4 + rel);
    const eng::Params n = eng::Call(eng::FindCdo("KismetStringLibrary"), "Conv_StringToName", eng::FString{L"WINDOWS", 8, 8});
    if (const uint8_t* r = n.Return()) std::memcpy(&gWindowsName, r, sizeof gWindowsName);
    if (!gGetSubsystem(&gWindowsName)) return "no WINDOWS socket subsystem";
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return "vtable not writable";
    *slot = reinterpret_cast<uintptr_t>(&WindowsSocketSubsystem);
    VirtualProtect(slot, sizeof *slot, old, &old);
    return "UDP driver on the computer's own sockets";
}
bool gDriverChecked = false;
std::string gSandboxDriver;            // test: a driver class for test copies ("" = the fallback)
std::string gDriverNote;
// A player's game (no sandbox): the same definition's fallback is made the Steam driver itself, so an editor session is
// only ever on Steam's networking (its relay, authenticated peers) and never a plain UDP port: if Steam's sockets can't
// start, the listen fails instead of falling back to IpNetDriver on a public port.
bool gSteamOnlyChecked = false;
std::string gSteamOnlyNote;
bool gSteamOnly = false;
bool SteamOnlyInPlayersGame() {
    if (sandbox::On()) return true;
    if (gSteamOnlyChecked) return gSteamOnly;
    Obj gi = GameInstance();
    Obj engine = gi ? eng::OuterOf(gi) : nullptr;
    if (!engine) return false;
    gSteamOnlyChecked = true;
    const eng::Prop defs = eng::FindProp(eng::ClassOf(engine), "NetDriverDefinitions");
    if (!defs || eng::InnerOf(defs).size != 0x20) {
        gSteamOnlyNote = "the engine's net driver definitions are not as measured: sessions are off";
        hostlog::Warn("together: " + gSteamOnlyNote);
        return false;
    }
    struct Array {
        uint8_t* data;
        int32_t num, max;
    } array{};
    std::memcpy(&array, engine + defs.offset, sizeof array);
    for (int32_t i = 0; array.data && i < array.num && i < 16; ++i) {
        uint8_t* def = array.data + i * 0x20;
        uint32_t nameIndex = 0;
        int32_t nameNumber = 0, clsNumber = 0;
        uint32_t clsIndex = 0;
        std::memcpy(&nameIndex, def, 4), std::memcpy(&nameNumber, def + 4, 4);
        std::memcpy(&clsIndex, def + 8, 4), std::memcpy(&clsNumber, def + 12, 4);
        if (eng::Name(nameIndex, nameNumber) != "GameNetDriver") continue;
        const std::string cls = eng::Name(clsIndex, clsNumber);
        if (cls.find("SteamSockets") == std::string::npos) {
            gSteamOnlyNote = "the game's net driver is " + cls + ", not Steam's: sessions are off";
            hostlog::Warn("together: " + gSteamOnlyNote);
            return false;
        }
        std::memcpy(def + 16, def + 8, 8);         // fallback := SteamSockets
        gSteamOnlyNote = "Steam networking only (" + cls + ", no fallback)";
        gSteamOnly = true;
        return true;
    }
    return false;
}

void UseFallbackDriverInSandbox() {
    if (gDriverChecked || !sandbox::On()) return;
    Obj gi = GameInstance();
    Obj engine = gi ? eng::OuterOf(gi) : nullptr;
    if (!engine) return;
    gDriverChecked = true;
    const eng::Prop defs = eng::FindProp(eng::ClassOf(engine), "NetDriverDefinitions");
    if (!defs) {
        gDriverNote = "no NetDriverDefinitions";
        return;
    }
    struct Array {
        uint8_t* data;
        int32_t num, max;
    } array{};
    std::memcpy(&array, engine + defs.offset, sizeof array);
    struct FName {
        uint32_t index;
        int32_t number;
    };
    constexpr int kStride = 0x20;           // FNetDriverDefinition: three FNames, an int, a bool (types dump)
    for (int32_t i = 0; array.data && i < array.num && i < 16; ++i) {
        uint8_t* def = array.data + i * kStride;
        FName name{}, cls{}, fallback{};
        std::memcpy(&name, def, 8);
        std::memcpy(&cls, def + 8, 8);
        std::memcpy(&fallback, def + 16, 8);
        if (eng::Name(name.index, name.number) != "GameNetDriver") continue;
        const std::string from = eng::Name(cls.index, cls.number), to = eng::Name(fallback.index, fallback.number);
        if (to.empty() || to == "None") {
            gDriverNote = "GameNetDriver has no fallback (" + from + ")";
            return;
        }
        FName chosen = fallback;
        std::string toName = to;
        if (!gSandboxDriver.empty()) {          // test: another driver class, by name
            const eng::Params n = eng::Call(eng::FindCdo("KismetStringLibrary"), "Conv_StringToName",
                                            eng::FString{eng::Widen(gSandboxDriver).c_str(), static_cast<int32_t>(gSandboxDriver.size() + 1),
                                                         static_cast<int32_t>(gSandboxDriver.size() + 1)});
            if (const uint8_t* r = n.Return()) std::memcpy(&chosen, r, 8), toName = gSandboxDriver;
        }
        std::memcpy(def + 8, &chosen, 8);
        gDriverNote = "test copy: GameNetDriver " + from + " -> " + toName + "; " + PatchIpDriverSockets();
        hostlog::Info("together: " + gDriverNote);
        return;
    }
    gDriverNote = "no GameNetDriver definition";
}

// Test: the game instance's ProcessEvent calls whose function names mention travel or network errors, logged with their
// first byte (ETravelFailure / ENetworkFailure). The game instance alone gets a copy of its vtable with ProcessEvent
// replaced; nothing else is touched. Only for finding why a travel fails (shipping builds write no log).
using ProcessEventFn = void (*)(Obj self, Obj fn, void* params);
ProcessEventFn gGiProcessEvent = nullptr;
std::vector<uintptr_t> gGiVtable;
Obj gWatchedGi = nullptr;
void GiProcessEvent(Obj self, Obj fn, void* params) {
    const std::string name = fn ? eng::ObjName(fn) : "";
    if (name.find("Error") != std::string::npos || name.find("Travel") != std::string::npos || name.find("Network") != std::string::npos) {
        uint8_t first = 0;
        if (params) std::memcpy(&first, params, 1);
        hostlog::Info("together: game instance " + name + " (" + std::to_string(first) + ")");
    }
    gGiProcessEvent(self, fn, params);
}
}  // namespace
void WatchGameInstance() {
    Obj gi = GameInstance();
    if (!gi || gi == gWatchedGi) return;
    uintptr_t* vtable = nullptr;
    std::memcpy(&vtable, gi, sizeof vtable);
    if (!vtable) return;
    gGiVtable.assign(vtable - 1, vtable + 200);     // the entry before the table (its RTTI locator) comes along
    gGiProcessEvent = reinterpret_cast<ProcessEventFn>(vtable[layout::kProcessEventVtableSlot]);
    gGiVtable[1 + layout::kProcessEventVtableSlot] = reinterpret_cast<uintptr_t>(&GiProcessEvent);
    uintptr_t* copy = gGiVtable.data() + 1;
    std::memcpy(gi, &copy, sizeof copy);
    gWatchedGi = gi;
    hostlog::Info("together: watching the game instance's travel and network errors");
}
namespace {
// A client's own editor pawn. The pawn's ReceivePossessed (P_LevelEditorPawn's blueprint) is what sets the editor up:
// SKGLevelEditorHandler = SKGMLEHandler, the handler's Init, HandleHUD (makes W_MapEditor for a locally controlled
// pawn) and the handler's change delegates. Possession runs on the server only, so a client's pawn arrives without
// any of it; the host does the same for the client's own pawn once it has one.
eng::Weak gSetUpPawn;
std::string gNotice;                    // the last thing the host refused or ended, for the plugin to show (TakeNotice)
void Notice(const std::string& text) {
    gNotice = text;
    hostlog::Info("together: " + text);
}
ULONGLONG gLastClientCheck = 0;
std::string gClientNote;
// The live instance of a class (in the viewport, for widgets), cached: the object array is scanned again only when the
// cached one is gone, at most every 2 s per class (a scan goes through every object).
Obj LiveInstance(const char* className, bool inViewport = true) {
    struct Cached {
        eng::Weak object;
        ULONGLONG scanned = 0;
    };
    static std::map<std::string, Cached> cache;
    Cached& c = cache[std::string(className) + (inViewport ? "" : "#any")];
    if (Obj o = eng::Get(c.object)) {
        if (!inViewport || !eng::FindFunction(eng::ClassOf(o), "IsInViewport") || eng::Call(o, "IsInViewport").ReturnBool()) return o;
        c.object = {};                  // gone from the screen (a test run makes the editor again): look again
    }
    if (GetTickCount64() - c.scanned < 2000) return nullptr;
    c.scanned = GetTickCount64();
    Obj cls = eng::FindClass(className), found = nullptr;
    if (!cls) return nullptr;
    const bool widget = inViewport && eng::FindFunction(cls, "IsInViewport") != nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && (!widget || eng::Call(o, "IsInViewport").ReturnBool())) found = o;
        return found == nullptr;
    });
    c.object = eng::MakeWeak(found);
    return found;
}
// A client's handler keeps its own list of pieces (AllActors), filled once by Init: pieces the host (or another
// player) adds later arrive by replication but not in that list, so the client's select-all, outliner and its own save
// would miss them. The handler's GetAllActors(bReCache) builds the list again from the world; on a client the host
// asks for that once a second.
int gRecached = 0;
void RecacheClientPieces(Obj handler) {
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) return;    // a drag or a look: later
    if (eng::Call(handler, "IsDraggingLocalActor").ReturnBool()) return;
    eng::Params p(eng::FunctionOn(handler, "GetAllActors"));
    p.SetArg(0, uint8_t{1});
    p.SetArg(1, uint8_t{0});
    if (eng::Invoke(handler, p)) ++gRecached;
}

// The map is the host's alone (user decision 2026-11-05): a client can't save or publish it. Its editor's save button
// (WBP_SaveButton), publish tab (WBP_PublishButton), publisher tools (WBP_PublisherToolsButton, which the game shows
// and hides itself) and publish window are kept hidden, checked again 4 times a second. The world settings tab goes too:
// its light rotation is set on the joiner's own light (W_WorldSettings: K2_SetActorRotation, no Server_ call), which
// would leave the joiner's world different from everyone else's. Test runs are the host's: a joiner's play buttons go,
// and the host refuses a joiner's ServerPlayHere / ServerPlayFromStart (RefuseJoinersEditorCalls). Before that, the light
// would leave the joiner's world different from everyone else's. Saves that still start (the editor's exit and menu
// actions save too) are refused where they write (GuardMapWrites).
int gHiddenSaving = 0;
void HideClientSaving() {
    Obj editorUi = LiveInstance("W_MapEditor_C");
    if (!editorUi) return;
    for (const char* name : {"WBP_SaveButton", "WBP_PublishButton", "WBP_PublisherToolsButton", "WBP_PublishWindow",
                             "WBP_WorldSettingsButton", "WBP_PlayHereButton", "WBP_PlayFromStartButton"}) {
        Obj widget = eng::FindProp(eng::ClassOf(editorUi), name) ? eng::ReadObj(editorUi, name) : nullptr;
        if (!widget) continue;
        constexpr uint8_t kCollapsed = 1;
        if (eng::Call(widget, "GetVisibility").ReturnAs<uint8_t>(0) != kCollapsed) {
            eng::Call(widget, "SetVisibility", kCollapsed);
            ++gHiddenSaving;
        }
    }
}

// On the host, a player joining clears the editor's map name (its header box and the level data a save uses; the game
// instance's CurrentUGCLevelName keeps it; measured 2026-11-05), so the host's save went to a nameless file. Whenever the
// player count changes, the name is given back the editor's own way: W_MapEditor.PassTextToUIManager(EnteredText), what
// typing it in the header does (WBP_LevelSubmission.WithLevelNames and the header's box).
int gLastPlayers = -1, gRestoresLeft = 0;
std::string gHostMapName;
// The map's display name as the editor holds it: WBP_LevelSubmission.GetCurrentLevelData's LevelDisplayName (an FString
// at 0x98 of SBallgameLevelData, from the types dump), "" if there's no panel or no name.
std::string EditorMapName() {
    Obj panel = LiveInstance("WBP_LevelSubmission_C", false);
    Obj fn = panel ? eng::FindFunction(eng::ClassOf(panel), "GetCurrentLevelData") : nullptr;
    if (!fn) return "";
    eng::Params p(fn);
    if (!eng::Invoke(panel, p)) return "";
    size_t size = 0;
    const uint8_t* data = p.Get("LevelData", &size);
    return data && size > 0xa8 ? eng::ReadFString(data + 0x98) : "";
}
ULONGLONG gPlayersChangedAt = 0;
int gNameRestores = 0;
std::string GameInstanceMapName() {
    Obj gi = GameInstance();
    const eng::Prop p = gi ? eng::FindProp(eng::ClassOf(gi), "CurrentUGCLevelName") : eng::Prop{};
    return p && eng::KindOf(p) == "StrProperty" ? eng::ReadFString(gi + p.offset) : "";
}
bool PassMapName(const std::string& name) {
    Obj editorUi = LiveInstance("W_MapEditor_C");
    if (!editorUi || name.empty()) return false;
    const eng::Params text = eng::MakeText(name);
    const uint8_t* ftext = text.Return();
    if (!ftext) return false;
    eng::Params p(eng::FunctionOn(editorUi, "PassTextToUIManager"));
    p.SetArg(0, ftext, 16);
    const bool ok = eng::Invoke(editorUi, p);
    eng::ReleaseText(ftext);
    return ok;
}
void KeepHostMapName() {
    if (!IsServer()) {
        gLastPlayers = -1;
        gHostMapName.clear();
        return;
    }
    // the host's name for the map as the editor holds it (its submission panel's level data: a renamed or newly named
    // map's too), read while no player is arriving; the game instance's as a fallback
    const int players = Players();
    if (players == gLastPlayers && !gPlayersChangedAt) {
        std::string name = EditorMapName();
        if (name.empty() && gHostMapName.empty()) name = GameInstanceMapName();
        if (!name.empty()) gHostMapName = name;
    }
    if (players != gLastPlayers) {
        gLastPlayers = players;
        gPlayersChangedAt = GetTickCount64();
        gRestoresLeft = 2;
    }
    // a moment after the change (the joiner's arrival runs over a few frames), and again a few seconds later
    const ULONGLONG since = gPlayersChangedAt ? GetTickCount64() - gPlayersChangedAt : 0;
    if (gPlayersChangedAt && ((gRestoresLeft == 2 && since > 1200) || (gRestoresLeft == 1 && since > 4000))) {
        --gRestoresLeft;
        if (!gHostMapName.empty() && EditorMapName() != gHostMapName && PassMapName(gHostMapName)) ++gNameRestores;
        if (gRestoresLeft == 0) gPlayersChangedAt = 0;
    }
}

void SetUpClientEditor() {
    if (GetTickCount64() - gLastClientCheck < 250) return;
    gLastClientCheck = GetTickCount64();
    static ULONGLONG lastRecache = 0;
    const bool recache = GetTickCount64() - lastRecache > 2000;
    if (recache) lastRecache = GetTickCount64();
    KeepHostMapName();
    // the host's own list misses pieces another player's handler brings back (a joiner's undo of a delete): rebuilt too
    if (IsServer() && recache)
        if (Obj own = editor::OwnHandler()) RecacheClientPieces(own);
    if (!IsClient()) return;
    Obj controller = game::PlayerController();
    Obj pawn = controller ? eng::Call(controller, "K2_GetPawn").ReturnObj() : nullptr;
    HideClientSaving();                 // a test run's ball pawn has no handler: hidden all the same
    if (!pawn || !eng::FindProp(eng::ClassOf(pawn), "SKGMLEHandler")) return;
    if (eng::Get(gSetUpPawn) == pawn) {
        if (recache)
            if (Obj handler = eng::ReadObj(pawn, "SKGMLEHandler")) RecacheClientPieces(handler);
        return;
    }
    Obj handler = eng::ReadObj(pawn, "SKGMLEHandler");
    if (!handler) return;
    gSetUpPawn = eng::MakeWeak(pawn);
    eng::WriteBytes(pawn, "SKGLevelEditorHandler", &handler, sizeof handler);
    const bool init = eng::Call(handler, "Init").Invoked();
    const bool hud = eng::Call(pawn, "HandleHUD").Invoked();
    eng::Call(handler, "SetGizmoType", uint8_t{20});
    gClientNote = std::string("client editor set up (init ") + (init ? "ok" : "failed") + ", hud " + (hud ? "ok" : "failed") + ")";
    hostlog::Info("together: " + gClientNote);
}

// Undo and redo are each player's own, as in a shared document: a player's Ctrl+Z steps back their own changes only.
// The editor keeps its history in the world's SKGMLEGizmoWorldSubsystem.HistoryManager, shared by every handler, unless
// a handler has its own (SKGMLEHandlerComponent.HistoryManagerOverride, a BallestHistoryManager). Every player's changes
// run on the host through that player's own handler (their pawn's), so on the host each joiner's handler gets a
// history of its own; the host's handler keeps the world's. A joiner's own game has no history (its handler only asks
// the host), so its undo and redo are sent to the host as lobby messages ("undo <PlayerId>" / "redo <PlayerId>", the
// PlayerState's replicated PlayerId, the same on every machine), and the host runs them on that player's handler. The
// host takes them only from members of its own lobby.
int gSentSteps = 0, gRanSteps = 0, gOwnHistories = 0;
bool gRedirecting = false;

Obj LocalPlayerState() {
    Obj controller = game::PlayerController();
    return controller ? eng::ReadObj(controller, "PlayerState") : nullptr;
}
int PlayerIdOf(Obj state) {
    int32_t id = -1;
    return state && eng::ReadBytes(state, "PlayerId", &id, sizeof id) ? id : -1;
}

// On the host: a history of its own for every other player's handler (made once per handler; the handler's property
// keeps it alive).
void GiveJoinersOwnHistories() {
    Obj controller = game::PlayerController();
    Obj gameState = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameState", controller).ReturnObj() : nullptr;
    Obj own = LocalPlayerState(), cls = eng::FindClass("BallestHistoryManager");
    if (!gameState || !cls) return;
    for (Obj ps : eng::ReadObjArray(gameState, "PlayerArray")) {
        if (ps == own) continue;
        Obj pawn = eng::ReadObj(ps, "PawnPrivate");
        Obj handler = pawn && eng::FindProp(eng::ClassOf(pawn), "SKGMLEHandler") ? eng::ReadObj(pawn, "SKGMLEHandler") : nullptr;
        if (!handler || eng::ReadObj(handler, "HistoryManagerOverride")) continue;
        Obj history = eng::Call(eng::FindCdo("GameplayStatics"), "SpawnObject", cls, handler).ReturnObj();
        if (!history) continue;
        eng::WriteBytes(handler, "HistoryManagerOverride", &history, sizeof history);
        ++gOwnHistories;
        hostlog::Info("together: player " + std::to_string(PlayerIdOf(ps)) + " has an undo history of their own");
    }
}

int TransactionsOf(Obj history) {
    struct {
        void* data;
        int32_t num, max;
    } a{};
    return history && eng::ReadBytes(history, "Transactions", &a, sizeof a) ? a.num : -1;
}
}  // namespace
std::string HistoryStatus() {
    std::string out;
    Obj controller = game::PlayerController();
    Obj gameState = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameState", controller).ReturnObj() : nullptr;
    if (Obj sub = eng::FindClass("SKGMLEGizmoWorldSubsystem")) {
        Obj found = nullptr;
        eng::ForEachObject([&](Obj o) {
            if (eng::ClassOf(o) == sub && !eng::IsDefaultObject(o)) found = o;
            return found == nullptr;
        });
        out += "world=" + std::to_string(TransactionsOf(found ? eng::ReadObj(found, "HistoryManager") : nullptr));
    }
    if (gameState)
        for (Obj ps : eng::ReadObjArray(gameState, "PlayerArray")) {
            Obj pawn = eng::ReadObj(ps, "PawnPrivate");
            Obj handler = pawn && eng::FindProp(eng::ClassOf(pawn), "SKGMLEHandler") ? eng::ReadObj(pawn, "SKGMLEHandler") : nullptr;
            out += " player" + std::to_string(PlayerIdOf(ps)) + "=" +
                   (handler ? std::to_string(TransactionsOf(eng::ReadObj(handler, "HistoryManagerOverride"))) : std::string("no handler"));
            if (handler) {
                Obj member = nullptr;           // the native member Undo needs (+0x3c0, measured from its code)
                std::memcpy(&member, handler + 0x3c0, sizeof member);
                out += "(+3c0 " + (member && eng::IsLive(member) ? eng::ObjName(eng::ClassOf(member)) : std::string(member ? "?" : "null")) + ")";
            }
        }
    return out;
}
namespace {

// Recording: every change a joiner makes runs on the host as one of their handler's Server_ functions (RPCs: Server_Delete,
// Server_SpawnActorPaste, Server_SetActorTransforms, Server_ApplyMaterial, ...), and those record their step into the
// world's history (SKGMLEGizmoWorldSubsystem.HistoryManager; measured: a joiner's delete added to it, not to the
// handler's own). So while one of a joiner's Server_ functions runs on the host, the world's history is that joiner's
// own (their handler's HistoryManagerOverride), and the host's again right after: each step lands in its player's
// history, which that player's Undo / Redo (the handler's, it reads the override) then steps through.
constexpr int kMaxWrapped = 64;
NativeFunction gServerOriginal[kMaxWrapped] = {};
std::string gServerName[kMaxWrapped];
int gWrapped = 0;
bool gWrapOverflow = false;             // more Server_ functions than wrappers: some would skip the gate (hosting refused)
bool gWrapTried = false;
eng::Weak gWorldHistoryOwner;           // the SKGMLEGizmoWorldSubsystem
Obj WorldHistorySubsystem() {
    if (Obj s = eng::Get(gWorldHistoryOwner)) return s;
    Obj cls = eng::FindClass("SKGMLEGizmoWorldSubsystem"), found = nullptr;
    if (!cls) return nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o)) found = o;
        return found == nullptr;
    });
    gWorldHistoryOwner = eng::MakeWeak(found);
    return found;
}
int gInPlayerCall = 0;                  // a joiner's Server_ call (or step) is running: the world history is theirs
eng::Weak gHostHistory;                 // the world's own history (the host's), seen outside any such call
std::set<std::pair<int32_t, Obj>> gAdmittedHandlers;    // the handlers (object slot and object) of players let in
bool gGateRan = false;                   // GateConnections has decided at least once
bool RunPlayerStep(Obj handler, bool redo);
// A joiner's undo or redo comes as their handler's Server_ApplyHistoryProperties with no properties and their own pawn
// (undo) or player state (redo) as its actor: on the game's own connection, so it reaches the host in order with their
// changes, and the connection says who sent it. The original would apply nothing; the step runs instead.
int StepMarker(int i, Obj context, uint8_t* frame) {
    if (gServerName[i] != "Server_ApplyHistoryProperties" || !frame) return 0;
    uint8_t* locals = nullptr;
    std::memcpy(&locals, frame + layout::kFFrameParametersOffset, sizeof locals);
    Obj fn = eng::FindFunction(eng::ClassOf(context), gServerName[i]);
    const eng::Prop actorProp = fn ? eng::FindProp(fn, "Actor") : eng::Prop{};
    if (!locals || !actorProp || actorProp.size != 8) return 0;
    Obj actor = nullptr;
    std::memcpy(&actor, locals + actorProp.offset, sizeof actor);
    for (const char* list : {"FloatProperties", "IntProperties", "BoolProperties", "TextProperties", "NameProperties", "StringProperties"}) {
        const eng::Prop p = eng::FindProp(fn, list);
        int32_t num = 0;
        if (p) std::memcpy(&num, locals + p.offset + 8, sizeof num);
        if (num != 0) return 0;
    }
    Obj pawn = eng::Call(context, "GetOwner").ReturnObj();
    if (!actor || !pawn) return 0;
    if (actor == pawn) return 1;
    return actor == eng::ReadObj(pawn, "PlayerState") ? 2 : 0;
}
void RunAsPlayer(int i, Obj context, uint8_t* frame, void* result) {
    const bool joiner = context && IsServer() && context != editor::OwnHandler();
    // on the host, a joiner's change runs only once that joiner is let in (these calls come from the network: no
    // bytecode to step when one is skipped)
    if (joiner && (!gGateRan || !gAdmittedHandlers.count({eng::MakeWeak(context).index, context}))) {
        static ULONGLONG said = 0;
        if (GetTickCount64() - said > 2000) {
            said = GetTickCount64();
            hostlog::Warn("together: a change from a player who isn't let in was refused");
        }
        return;
    }
    // the world's lighting is the host's (its world settings): a joiner's is refused
    if (joiner && gServerName[i].rfind("Server_SetWorld", 0) == 0) {
        static ULONGLONG said = 0;
        if (GetTickCount64() - said > 2000) {
            said = GetTickCount64();
            hostlog::Warn("together: a joiner's " + gServerName[i] + " was refused (the world settings are the host's)");
        }
        return;
    }
    if (joiner) {
        if (const int step = StepMarker(i, context, frame)) {
            if (RunPlayerStep(context, step == 2)) ++gRanSteps;
            return;
        }
    }
    Obj own = context && IsServer() && eng::FindProp(eng::ClassOf(context), "HistoryManagerOverride")
                  ? eng::ReadObj(context, "HistoryManagerOverride") : nullptr;
    Obj world = own ? WorldHistorySubsystem() : nullptr;
    Obj before = world ? eng::ReadObj(world, "HistoryManager") : nullptr;
    const bool swap = world && before && before != own;
    if (swap) eng::WriteBytes(world, "HistoryManager", &own, sizeof own);
    ++gInPlayerCall;
    gServerOriginal[i](context, frame, result);
    --gInPlayerCall;
    if (swap) eng::WriteBytes(world, "HistoryManager", &before, sizeof before);
}
// Outside any joiner's call the world's history must be the host's: it is put back if anything ever left it otherwise.
void KeepHostHistory() {
    if (!IsServer() || gInPlayerCall) return;
    Obj world = WorldHistorySubsystem();
    Obj now = world ? eng::ReadObj(world, "HistoryManager") : nullptr;
    if (!now) return;
    Obj host = eng::Get(gHostHistory);
    bool someonesOwn = false;
    Obj controller = game::PlayerController();
    Obj gameState = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameState", controller).ReturnObj() : nullptr;
    if (gameState)
        for (Obj ps : eng::ReadObjArray(gameState, "PlayerArray")) {
            Obj pawn = eng::ReadObj(ps, "PawnPrivate");
            Obj handler = pawn && eng::FindProp(eng::ClassOf(pawn), "SKGMLEHandler") ? eng::ReadObj(pawn, "SKGMLEHandler") : nullptr;
            someonesOwn = someonesOwn || (handler && eng::ReadObj(handler, "HistoryManagerOverride") == now);
        }
    if (!someonesOwn) {
        gHostHistory = eng::MakeWeak(now);
    } else if (host) {
        eng::WriteBytes(world, "HistoryManager", &host, sizeof host);
        hostlog::Warn("together: the world's history was a joiner's outside their call: the host's is back");
    }
}
template <int I>
void ServerWrapped(Obj context, uint8_t* frame, void* result) { RunAsPlayer(I, context, frame, result); }
template <int... I>
constexpr std::array<NativeFunction, sizeof...(I)> MakeWrappers(std::integer_sequence<int, I...>) { return {&ServerWrapped<I>...}; }
const std::array<NativeFunction, kMaxWrapped> kServerWrappers = MakeWrappers(std::make_integer_sequence<int, kMaxWrapped>{});

void WrapServerFunctions(Obj handler) {
    if (gWrapTried || !handler) return;
    gWrapTried = true;
    Obj cls = eng::ClassOf(handler);
    for (Obj c = cls; c; c = eng::SuperOf(c)) {
        for (const std::string& name : eng::FunctionNames(c)) {
            if (name.rfind("Server_", 0) != 0) continue;
            if (gWrapped >= kMaxWrapped) {
                gWrapOverflow = true;
                continue;
            }
            Obj fn = eng::FindFunction(c, name);
            NativeFunction entry = nullptr;
            if (fn) std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
            if (!entry || !eng::InImage(reinterpret_cast<void*>(entry))) continue;
            gServerOriginal[gWrapped] = entry;
            gServerName[gWrapped] = name;
            const NativeFunction hooked = kServerWrappers[static_cast<size_t>(gWrapped)];
            std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
            ++gWrapped;
        }
    }
    hostlog::Info("together: " + std::to_string(gWrapped) + " of the editor's server functions record into each player's own history");
    if (gWrapOverflow) hostlog::Warn("together: the editor has more server functions than can be gated: sessions are closed");
}

// A joiner's undo or redo, run on the host against that joiner's own history. The handler's Undo / Redo need the
// world's gizmo subsystem in a native member (+0x3c0, read from their code: without it they return at once), which a
// handler only has when its player is local; the host's copy of a joiner's handler has null there. For the step it is
// given the host's own (checked: the host's handler holds that same SKGMLEGizmoWorldSubsystem there), and the world's
// history is the joiner's own, as while their changes are recorded; both are put back right after.
constexpr int kHandlerGizmoSubsystem = 0x3c0;
bool RunPlayerStep(Obj handler, bool redo) {
    Obj own = editor::OwnHandler(), world = WorldHistorySubsystem();
    Obj history = eng::ReadObj(handler, "HistoryManagerOverride");
    if (!own || !world || !history || handler == own) return false;
    Obj ownSubsystem = nullptr, theirs = nullptr;
    std::memcpy(&ownSubsystem, own + kHandlerGizmoSubsystem, sizeof ownSubsystem);
    std::memcpy(&theirs, handler + kHandlerGizmoSubsystem, sizeof theirs);
    if (ownSubsystem != world || (theirs && theirs != world)) {
        hostlog::Warn("together: the editor handler's layout is not as measured; a joiner's undo isn't run");
        return false;
    }
    Obj before = eng::ReadObj(world, "HistoryManager");
    std::memcpy(handler + kHandlerGizmoSubsystem, &world, sizeof world);
    eng::WriteBytes(world, "HistoryManager", &history, sizeof history);
    ++gInPlayerCall;
    const bool ran = eng::Call(handler, redo ? "Redo" : "Undo").Invoked();
    --gInPlayerCall;
    editor::WakePieces();
    eng::WriteBytes(world, "HistoryManager", &before, sizeof before);
    std::memcpy(handler + kHandlerGizmoSubsystem, &theirs, sizeof theirs);
    return ran;
}

// Who is in the editor. Anyone on Steam can reach the host's Steam id, so the host decides, every quarter second while
// it listens, which connected players may stay, and sends the rest home (as the engine's own GameSession kicks:
// ClientWasKicked, then ClientReturnToMainMenuWithTextReason, then their pawn and controller destroyed: even a client
// that ignored the message can't edit any more). Fail closed: a host listening without its own lobby keeps nobody.
//   * Who a player is: in a player's game, the Steam id of their PlayerState's UniqueId (SIK_SharedFile
//     GetSteamIdFromUniqueNetId: Steam authenticates it). In a test copy every copy is the same Steam account, so there
//     a lobby member says "hello <PlayerId>" over the lobby once connected (only members can send lobby messages).
//   * Who may stay: a member of the host's lobby, admitted by the session's kind (the lobby type the host made it with):
//     public, anyone in the lobby; friends and invite only, the host's Steam friends; code only, a member who proved the
//     code ("join <proof>", proof = a hash of the code and their own member id, so nobody who reads the lobby's chat can
//     reuse it). And no more than the lobby's size.
//   * Grace: kAdmitMs to show who they are and prove the code (a test copy's hello comes once its map has loaded).
constexpr ULONGLONG kHelloEveryMs = 4000, kAdmitMs = 20000, kTestAdmitMs = 90000, kKickEveryMs = 2000;
ULONGLONG gLastHello = 0, gLastGate = 0;
std::map<int, std::string> gClaims;     // test copies: PlayerId -> the lobby member who said it's them
std::map<int, ULONGLONG> gFirstSeen;    // PlayerId -> when the host first saw that player
std::map<int, ULONGLONG> gLastKick;     // PlayerId -> when they were last sent home
std::set<std::string> gProved;          // members who proved the session's code
std::map<std::string, std::string> gExpectedProof;
std::string gSessionCode, gJoinCode;    // the host's code (code-only sessions) and the one this joiner typed
int gKicked = 0;

// PBKDF2-HMAC-SHA256 of the code, salted with the lobby and the member (and what it's for): a proof read in the lobby's
// chat can't be reused by anyone else or in another lobby, and trying the 32^8 codes against it costs years. Made once
// per member and kept (a flood of proofs costs nothing).
constexpr ULONG kProofRounds = 60000;
std::string Proof(const std::string& code, const std::string& member) {
    static std::map<std::string, std::string> made;
    const std::string salt = "create-together|" + lobby::Id() + "|" + member;
    const std::string key = code + "\n" + salt;
    if (auto it = made.find(key); it != made.end()) return it->second;
    static BCRYPT_ALG_HANDLE alg = nullptr;
    if (!alg && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) alg = nullptr;
    uint8_t out[16] = {};
    if (!alg || code.empty() ||
        BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(const_cast<char*>(code.data())), static_cast<ULONG>(code.size()),
                              reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())), static_cast<ULONG>(salt.size()), kProofRounds,
                              out, sizeof out, 0) != 0)
        return "";
    std::string hex;
    char b[3];
    for (uint8_t x : out) {
        std::snprintf(b, sizeof b, "%02x", x);
        hex += b;
    }
    if (made.size() > 64) made.clear();
    made[key] = hex;
    return hex;
}

std::string SteamIdOf(Obj state) {
    static eng::Weak lib;
    if (!eng::Get(lib)) lib = eng::MakeWeak(eng::FindCdo("SIK_SharedFile"));
    Obj l = eng::Get(lib);
    const eng::Prop p = state ? eng::FindProp(eng::ClassOf(state), "UniqueID") : eng::Prop{};
    if (!l || !p || p.size != 48) return "";
    eng::Params call(eng::FunctionOn(l, "GetSteamIdFromUniqueNetId"));
    if (!call.SetArg(0, state + p.offset, 48) || !eng::Invoke(l, call)) return "";
    const int64_t id = call.ReturnAs<int64_t>(0);
    return id > 0 ? std::to_string(id) : "";
}

// The lobby member a connected player is ("" if not known yet).
std::string MemberOf(Obj state, int playerId) {
    if (!sandbox::On()) return SteamIdOf(state);
    auto it = gClaims.find(playerId);
    return it == gClaims.end() ? "" : it->second;
}

void Kick(Obj state, const std::string& why, const std::string& member) {
    const int id = PlayerIdOf(state);
    if (GetTickCount64() - gLastKick[id] < kKickEveryMs) return;
    gLastKick[id] = GetTickCount64();
    if (!member.empty()) lobby::Send("kick " + member + " " + why);     // the player's own plugin says why
    Obj controller = eng::ReadObj(state, "Owner");
    hostlog::Warn("together: player " + std::to_string(id) + " removed from the session: " + why);
    if (!controller) return;
    const eng::Params text = eng::MakeText("you're not in this session (" + why + ")");
    if (const uint8_t* t = text.Return()) {
        for (const char* fn : {"ClientWasKicked", "ClientReturnToMainMenuWithTextReason"}) {
            eng::Params p(eng::FunctionOn(controller, fn));
            p.SetArg(0, t, 16);
            eng::Invoke(controller, p);
        }
        eng::ReleaseText(t);
    }
    if (Obj pawn = eng::ReadObj(state, "PawnPrivate")) eng::Call(pawn, "K2_DestroyActor");
    eng::Call(controller, "K2_DestroyActor");
    ++gKicked;
}

bool Admitted(const std::string& member) {
    switch (lobby::Type()) {
        case lobby::kPublic: return true;
        case lobby::kFriends:
        case lobby::kInviteOnly: return lobby::IsFriend(member);
        case lobby::kCode: return gProved.count(member) > 0 || (!sandbox::On() && lobby::IsFriend(member));   // the host's invites too
        default: return false;
    }
}

std::map<std::string, ULONGLONG> gBanned;     // member -> when last sent home (sent home again on sight for kBanMs)
std::map<int, ULONGLONG> gMissingSince;       // PlayerId -> since when their member isn't in the lobby
ULONGLONG gNoMembersSince = 0;
constexpr ULONGLONG kBanMs = 60000, kLobbyLagMs = 4000, kMissingMs = 5000, kNoMembersMs = 3000;

void GateConnections() {
    if (GetTickCount64() - gLastGate < 250) return;
    gLastGate = GetTickCount64();
    gGateRan = true;                              // from now on, only those it let in may change anything
    Obj controller = game::PlayerController();
    Obj gameState = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameState", controller).ReturnObj() : nullptr;
    Obj own = LocalPlayerState();
    if (!gameState) return;
    const ULONGLONG now = GetTickCount64();
    const bool ownLobby = lobby::State() == "in lobby" && lobby::IsOwner();
    const std::vector<lobby::Member> members = ownLobby ? lobby::Members() : std::vector<lobby::Member>{};
    // Steam's list empty for a moment: decided next time; empty for longer: the lobby is as good as gone
    bool closed = !ownLobby;
    if (ownLobby && members.empty()) {
        if (!gNoMembersSince) gNoMembersSince = now;
        if (now - gNoMembersSince < kNoMembersMs) return;
        closed = true;
    } else {
        gNoMembersSince = 0;
    }
    const int most = std::max(1, lobby::MaxMembers());
    std::map<int, ULONGLONG> seen;
    std::set<std::pair<int32_t, Obj>> admitted;
    int kept = 1;                                 // the host
    // who each connected player is: in a player's game their PlayerState's Steam id (Steam authenticates it; nothing a
    // lobby member says can change it); in a test copy (one Steam account) the member who said hello for that PlayerId
    struct Seen {
        Obj ps;
        int id;
        std::string member;
    };
    std::vector<Seen> players;
    std::map<std::string, int> newest;            // member -> the PlayerId that came last (a rejoin replaces the old one)
    for (Obj ps : eng::ReadObjArray(gameState, "PlayerArray")) {
        if (ps == own) continue;
        const int id = PlayerIdOf(ps);
        seen[id] = gFirstSeen.count(id) ? gFirstSeen[id] : now;
        const std::string member = id < 0 ? "" : MemberOf(ps, id);
        players.push_back({ps, id, member});
        if (!member.empty() && (!newest.count(member) || seen[id] > seen[newest[member]] ||
                                (seen[id] == seen[newest[member]] && id > newest[member])))
            newest[member] = id;
    }
    for (const Seen& p : players) {
        Obj ps = p.ps;
        const int id = p.id;
        const std::string& member = p.member;
        const ULONGLONG age = now - seen[id];
        if (closed) {
            Kick(ps, "the session is closed", member);
            continue;
        }
        bool inLobby = false;
        for (const auto& m : members) inLobby = inLobby || (!member.empty() && m.id == member);
        if (!inLobby) gMissingSince.emplace(id, now);
        else gMissingSince.erase(id);
        const bool missingLong = !inLobby && now - gMissingSince[id] > kMissingMs;
        const ULONGLONG grace = sandbox::On() ? kTestAdmitMs : kAdmitMs;
        std::string why;
        bool ban = false;                         // only a refusal keeps them out for a while (not a rejoin or a leave)
        if (id < 0) why = "no player id";
        else if (!member.empty() && gBanned.count(member) && now - gBanned[member] < kBanMs) why = "sent home a moment ago";
        else if (!member.empty() && newest[member] != id) why = "they joined again";
        else if (!member.empty() && !inLobby && (age > kLobbyLagMs || missingLong)) why = "not in the session's lobby";
        else if (member.empty() || !Admitted(member)) {
            if (age > grace) {
                why = member.empty() ? "didn't say who they are"
                                     : std::string("not let in (") + (lobby::Type() == lobby::kCode ? "no code" : "not the host's friend") + ")";
                ban = !member.empty();
            } else {
                continue;                         // not decided yet: they can't change anything meanwhile
            }
        } else if (kept + 1 > most) {
            why = "the session is full";
        }
        if (!why.empty()) {
            if (ban) gBanned[member] = now;
            Kick(ps, why, member);
            continue;
        }
        ++kept;
        Obj pawn = eng::ReadObj(ps, "PawnPrivate");
        if (Obj handler = pawn && eng::FindProp(eng::ClassOf(pawn), "SKGMLEHandler") ? eng::ReadObj(pawn, "SKGMLEHandler") : nullptr)
            admitted.insert({eng::MakeWeak(handler).index, handler});
    }
    gFirstSeen.swap(seen);
    for (auto it = gClaims.begin(); it != gClaims.end();) it = gFirstSeen.count(it->first) || gClaims.size() < 32 ? std::next(it) : gClaims.erase(it);
    gAdmittedHandlers.swap(admitted);
    for (auto it = gMissingSince.begin(); it != gMissingSince.end();) it = gFirstSeen.count(it->first) ? std::next(it) : gMissingSince.erase(it);
}

// The world's lighting. The editor light's time of day and brightness replicate (SKGMapEditorDirectionalLight
// .DirectionalLightSettings, OnRep_DirectionalLightData), but measured 2026-11-05 a joiner keeps its own on joining, its
// light's yaw never follows the host's, and the sky lights' intensity and the sky sphere are each machine's own
// (W_WorldSettings sets them locally). So the host sends what it has ("world <time> <brightness> <pitch> <yaw> <roll>" then
// "|<sky light path>=<intensity>" for each, the same objects in every copy of the editor's level) when it changes and every
// few seconds (late joiners), and a joiner applies it, only from the lobby's owner.
std::string gLastWorld;
ULONGLONG gLastWorldSent = 0;
std::string WorldState() {
    char buf[256];
    std::string out;
    Obj lightCls = eng::FindClass("SKGMapEditorDirectionalLight"), skyCls = eng::FindClass("SkyLightComponent");
    std::string skies;
    eng::ForEachObject([&](Obj o) {
        if (eng::IsDefaultObject(o)) return true;
        if (lightCls && out.empty() && eng::IsA(o, lightCls) && eng::PathOf(o).find("/Engine/Transient") != 0) {
            float st[2] = {0, 0};
            eng::ReadBytes(o, "DirectionalLightSettings", st, sizeof st);
            struct { double p, y, r; } rot{};
            const eng::Params rp = eng::Call(o, "K2_GetActorRotation");
            if (const uint8_t* rr = rp.Return()) std::memcpy(&rot, rr, sizeof rot);
            std::snprintf(buf, sizeof buf, "world %.3f %.3f %.2f %.2f %.2f", st[0], st[1], rot.p, rot.y, rot.r);
            out = buf;
        } else if (skyCls && eng::IsA(o, skyCls) && eng::PathOf(o).find("/Engine/Transient") != 0) {
            float intensity = 0;
            eng::ReadBytes(o, "Intensity", &intensity, sizeof intensity);
            std::snprintf(buf, sizeof buf, "=%.3f", intensity);
            skies += "|" + eng::PathOf(o) + buf;
        }
        return true;
    });
    return out.empty() ? "" : out + skies;
}
void ShareHostWorld() {
    static ULONGLONG lastLook = 0;
    if (GetTickCount64() - lastLook < 1000) return;
    lastLook = GetTickCount64();
    if (Players() < 2) return;
    const std::string state = WorldState();
    if (state.empty() || state.size() > 1000) return;
    if (state != gLastWorld || GetTickCount64() - gLastWorldSent > 5000) {
        if (lobby::Send(state)) {
            gLastWorld = state;
            gLastWorldSent = GetTickCount64();
        }
    }
}
std::string gLastApplied;
ULONGLONG gLastAppliedAt = 0;
void ApplyHostWorld(const std::vector<lobby::Message>& messages) {
    const std::string owner = lobby::OwnerId();
    for (const auto& m : messages) {
        if (m.from != owner || m.text.rfind("world ", 0) != 0) continue;
        if (m.text == gLastApplied && GetTickCount64() - gLastAppliedAt < 20000) continue;     // nothing changed
        gLastApplied = m.text;
        gLastAppliedAt = GetTickCount64();
        float tod = 0, bri = 0;
        double pitch = 0, yaw = 0, roll = 0;
        if (std::sscanf(m.text.c_str() + 6, "%f %f %lf %lf %lf", &tod, &bri, &pitch, &yaw, &roll) != 5) continue;
        std::map<std::string, float> skies;
        size_t at = m.text.find('|');
        while (at != std::string::npos) {
            const size_t next = m.text.find('|', at + 1), eq = m.text.find('=', at);
            if (eq != std::string::npos && (next == std::string::npos || eq < next))
                skies[m.text.substr(at + 1, eq - at - 1)] = static_cast<float>(std::atof(m.text.c_str() + eq + 1));
            at = next;
        }
        Obj lightCls = eng::FindClass("SKGMapEditorDirectionalLight"), skyCls = eng::FindClass("SkyLightComponent"),
            sphereCls = eng::FindClass("BP_Sky_Sphere_Stylized_C");
        const float settings[2] = {tod, bri};
        std::vector<Obj> spheres;
        eng::ForEachObject([&](Obj o) {
            const bool light = lightCls && eng::IsA(o, lightCls), sky = !light && skyCls && eng::IsA(o, skyCls),
                       sphere = !light && !sky && sphereCls && eng::IsA(o, sphereCls);
            if (!(light || sky || sphere) || eng::IsDefaultObject(o) || eng::PathOf(o).find("/Engine/Transient") == 0) return true;
            if (light) {
                eng::WriteBytes(o, "DirectionalLightSettings", settings, sizeof settings);
                eng::Call(o, "OnRep_DirectionalLightData");
                const struct { double p, y, r; } rot{pitch, yaw, roll};
                eng::Call(o, "K2_SetActorRotation", rot, uint8_t{0});
            } else if (sky) {
                auto it = skies.find(eng::PathOf(o));
                if (it != skies.end()) eng::Call(o, "SetIntensity", it->second);
            } else {
                spheres.push_back(o);           // after the light has turned (the sphere reads it)
            }
            return true;
        });
        for (Obj o : spheres) {
            eng::Params sp(eng::FunctionOn(o, "EstablishEditorParams"));
            if (sp.SetArg(0, settings, sizeof settings)) eng::Invoke(o, sp);
        }
    }
}


// What the host says to this joiner over the lobby: that it knows the code too ("ok <me> <proof>"), or why it sent this
// player home ("kick <me> <why>").
bool gCodeAccepted = false;
void FromHost(const std::vector<lobby::Message>& messages) {
    const std::string owner = lobby::OwnerId(), me = lobby::OwnId();
    if (owner.empty()) return;
    for (const auto& m : messages) {
        if (m.from != owner) continue;
        if (m.text.rfind("ok " + me + " ", 0) == 0 && !gJoinCode.empty()) {
            const std::string expected = Proof(gJoinCode, me + "|host");
            if (!expected.empty() && m.text.substr(4 + me.size()) == expected) gCodeAccepted = true;
        } else if (m.text.rfind("kick " + me + " ", 0) == 0) {
            const std::string why = m.text.substr(6 + me.size(), 200);
            static ULONGLONG said = 0;
            if (GetTickCount64() - said > 5000) {
                said = GetTickCount64();
                Notice("the host removed you from the session: " + why);
            }
        }
    }
}

void ShareHistory() {
    const bool inLobby = lobby::State() == "in lobby";
    const bool redirect = IsClient();             // a joiner's own history is always empty: never their own
    if (redirect != gRedirecting) {
        gRedirecting = redirect;
        editor::SetHistoryRedirect(redirect ? std::function<bool(bool)>([](bool redo) {
            // the step marker (StepMarker): Server_ApplyHistoryProperties with no properties, the own pawn (undo) or
            // player state (redo) as its actor, on the same connection as this player's changes
            Obj handler = editor::OwnHandler();
            Obj pawn = handler ? eng::Call(handler, "GetOwner").ReturnObj() : nullptr;
            Obj actor = pawn && redo ? eng::ReadObj(pawn, "PlayerState") : pawn;
            eng::Params call(handler ? eng::FunctionOn(handler, "Server_ApplyHistoryProperties") : nullptr);
            if (actor && call.Set("Actor", &actor, sizeof actor) && eng::Invoke(handler, call)) ++gSentSteps;
            else Notice("undo and redo couldn't reach the host");
            return true;
        })
                                            : std::function<bool(bool)>());
    }
    if (!IsServer()) {
        gClaims.clear();
        gFirstSeen.clear();
        gLastKick.clear();
        gProved.clear();
        gExpectedProof.clear();
        gBanned.clear();
        gMissingSince.clear();
        gAdmittedHandlers.clear();
        gGateRan = false;
    }
    if (IsServer() && lobby::IsOwner()) ShareHostWorld();
    if (IsServer()) {
        WrapServerFunctions(editor::OwnHandler());
        GiveJoinersOwnHistories();
        KeepHostHistory();
        GateConnections();
    }
    if (!inLobby) return;
    // a joiner tells the host who it is: the code's proof as soon as it is in the lobby (before it connects), and
    // "hello <PlayerId>" once connected; again every few seconds (a message can be lost)
    if (!lobby::IsOwner() && GetTickCount64() - gLastHello > kHelloEveryMs) {
        const int id = IsClient() ? PlayerIdOf(LocalPlayerState()) : -1;
        bool sent = false;
        if (id >= 0 && sandbox::On()) sent = lobby::Send("hello " + std::to_string(id));      // a test copy's identity
        const std::string proof = gJoinCode.empty() || gCodeAccepted ? "" : Proof(gJoinCode, lobby::OwnId());
        if (!proof.empty()) sent = lobby::Send("join " + proof) || sent;
        if (sent) gLastHello = GetTickCount64();
    }
    const std::vector<lobby::Message> messages = lobby::Receive();
    if (!lobby::IsOwner()) FromHost(messages);
    if (IsClient()) ApplyHostWorld(messages);
    if (!IsServer() || !lobby::IsOwner() || messages.empty()) return;
    const std::string own = lobby::OwnId();
    const std::vector<lobby::Member> members = lobby::Members();
    for (const auto& m : messages) {
        if (m.from == own) continue;
        bool member = false;
        for (const auto& x : members) member = member || x.id == m.from;
        if (!member) continue;
        if (m.text.rfind("hello ", 0) == 0) {
            if (!sandbox::On()) continue;                       // a player's game knows who each player is from Steam
            const int id = std::atoi(m.text.c_str() + 6);
            if (!gClaims.count(id)) gClaims[id] = m.from;       // the first claim stands (a later one can't take it over)
            continue;
        }
        if (m.text.rfind("join ", 0) == 0) {
            if (gSessionCode.empty()) continue;
            if (!gProved.count(m.from)) {
                auto& expected = gExpectedProof[m.from];       // made once per member: a flood costs nothing
                if (expected.empty()) expected = Proof(gSessionCode, m.from);
                if (expected.empty() || m.text.substr(5) != expected) continue;
                gProved.insert(m.from);
            }
            // the host's own answer: proof that this session knows the code too (a look-alike lobby can't make it)
            lobby::Send("ok " + m.from + " " + Proof(gSessionCode, m.from + "|host"));
            continue;
        }
    }
}

// The lobby goes when the session does, from here rather than only from the plugin's script: a host that stops
// listening (back to the menu, another map, the plugin stopped) leaves its lobby; a joiner whose lobby's owner is no
// longer the host it joined (Steam hands a lobby to another member when its owner leaves) leaves the session.
ULONGLONG gNotServerSince = 0;
bool gWasHosting = false;
std::string gJoinedOwner;
void KeepLobbyWithSession() {
    if (IsServer()) {
        gWasHosting = true;
        gNotServerSince = 0;
    } else if (gWasHosting && lobby::IsOwner()) {
        if (!gNotServerSince) gNotServerSince = GetTickCount64();
        if (GetTickCount64() - gNotServerSince > 2000) {
            lobby::Leave();
            gWasHosting = false;
            gNotServerSince = 0;
            hostlog::Info("together: the editor stopped listening: its lobby is closed");
        }
    } else if (!lobby::IsOwner()) {
        gWasHosting = false;
    }
    static std::string joinedLobby;
    if (lobby::State() == "in lobby" && !lobby::IsOwner()) {
        const std::string owner = lobby::OwnerId();
        if (lobby::Id() != joinedLobby) {
            joinedLobby = lobby::Id();
            gJoinedOwner.clear();
        }
        if (owner.empty() || owner == "0") {
            // not known this moment: decided later
        } else if (gJoinedOwner.empty()) {
            gJoinedOwner = owner;
        } else if (owner != gJoinedOwner) {
            Notice("the host left: the session is over");
            gJoinedOwner.clear();
            joinedLobby.clear();
            lobby::Leave();
            if (IsClient() || gJoinPending) Leave();
        }
    } else {
        gJoinedOwner.clear();
        joinedLobby.clear();
    }
}

// A joiner never travels where the host says: the engine's ClientTravelInternal (a client RPC a host can send) is
// refused while this game is a client of a session (the host could otherwise send the joiner to any address).
NativeFunction gClientTravelOriginal = nullptr;
bool gClientTravelHookTried = false;
void ClientTravelHooked(Obj context, uint8_t* frame, void* result) {
    if (IsClient()) {
        hostlog::Warn("together: the host asked this game to travel elsewhere: refused");
        return;                         // an RPC from the network: called with its parameters, no bytecode to step
    }
    gClientTravelOriginal(context, frame, result);
}
void HookClientTravel() {
    if (gClientTravelOriginal || gClientTravelHookTried) return;
    Obj fn = eng::FindFunction(eng::FindClass("PlayerController"), "ClientTravelInternal");
    if (!fn) return;
    gClientTravelHookTried = true;
    NativeFunction entry = nullptr;
    std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
    const NativeFunction hooked = &ClientTravelHooked;
    if (!entry || !eng::InImage(reinterpret_cast<void*>(entry))) {
        hostlog::Warn("together: PlayerController.ClientTravelInternal is not as measured; joining is off");
        return;
    }
    gClientTravelOriginal = entry;
    std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
}

// The editor's document calls from a player (BallestEditorPlayerState's Server* RPCs: the map's name, description,
// difficulty and tags, loading a whole map in chunks, starting a test run, the save/publish export) run on the host for
// any player. Only the host's own are kept: a joiner's would rename, retag or replace the host's map, or start a test run
// in the shared editor. They come from the network, so a refused one has no bytecode to step.
constexpr int kEditorCalls = 13;
const char* const kEditorCallNames[kEditorCalls] = {"ServerLevelNames", "ServerLevelDescription", "ServerLevelDifficulty", "ServerLevelTags",
                                                    "ServerBeginLoad", "ServerLoadChunk", "ServerPlayHere", "ServerPlayFromStart",
                                                    "ServerRequestExport", "ServerExportAcknowledged", "ServerCancelEditorTransfer",
                                                    "ServerReturnToAuthoring", "ServerAuthoringCamera"};
NativeFunction gEditorCallOriginal[kEditorCalls] = {};
bool gEditorCallsTried = false;
int gEditorCallsRefused = 0;
template <int I>
void EditorCallHooked(Obj context, uint8_t* frame, void* result) {
    if (IsServer() && context && context != LocalPlayerState()) {
        if (++gEditorCallsRefused % 20 == 1) hostlog::Warn(std::string("together: a joiner's ") + kEditorCallNames[I] + " was refused (the map is the host's)");
        return;
    }
    gEditorCallOriginal[I](context, frame, result);
}
template <int... I>
constexpr std::array<NativeFunction, sizeof...(I)> MakeEditorCallHooks(std::integer_sequence<int, I...>) { return {&EditorCallHooked<I>...}; }
const std::array<NativeFunction, kEditorCalls> kEditorCallHooks = MakeEditorCallHooks(std::make_integer_sequence<int, kEditorCalls>{});
void RefuseJoinersEditorCalls() {
    if (gEditorCallsTried) return;
    Obj cls = eng::FindClass("BallestEditorPlayerState");
    if (!cls) return;
    gEditorCallsTried = true;
    int hooked = 0;
    for (int i = 0; i < kEditorCalls; ++i) {
        Obj fn = eng::FindFunction(cls, kEditorCallNames[i]);
        NativeFunction entry = nullptr;
        if (fn) std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
        if (!fn || !entry || !eng::InImage(reinterpret_cast<void*>(entry))) continue;
        gEditorCallOriginal[i] = entry;
        std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &kEditorCallHooks[static_cast<size_t>(i)], sizeof(NativeFunction));
        ++hooked;
    }
    hostlog::Info("together: " + std::to_string(hooked) + " of the editor's " + std::to_string(kEditorCalls) + " document calls are the host's own");
}

// Grouping, ungrouping (the editor keeps groups only on the machine that made them: no Server_ function, measured from
// the handler's functions) is off while editing together, for everyone: groups would otherwise differ between players.
NativeFunction gGroupOriginal[4] = {};
const char* const kGroupFunctions[4] = {"GroupSelection", "GroupSelectionShortcut", "UngroupSelection", "UngroupSelectionShortcut"};
bool gGroupHooksTried = false;
void SkipCall(uint8_t* frame) {         // a skipped native call without parameters: its own P_FINISH (see editor.cpp)
    uint8_t* code = nullptr;
    std::memcpy(&code, frame + 0x20, sizeof code);
    if (code) {
        ++code;
        std::memcpy(frame + 0x20, &code, sizeof code);
    }
}
template <int I>
void GroupHooked(Obj context, uint8_t* frame, void* result) {
    if (IsServer() || IsClient()) {
        Notice("groups are off while editing together (they'd only exist on your own screen)");
        return SkipCall(frame);
    }
    gGroupOriginal[I](context, frame, result);
}
const NativeFunction kGroupHooks[4] = {&GroupHooked<0>, &GroupHooked<1>, &GroupHooked<2>, &GroupHooked<3>};
void HookGroups(Obj handler) {
    if (gGroupHooksTried || !handler) return;
    gGroupHooksTried = true;
    for (int i = 0; i < 4; ++i) {
        Obj fn = eng::FindFunction(eng::ClassOf(handler), kGroupFunctions[i]);
        NativeFunction entry = nullptr;
        if (fn) std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
        if (!fn || !entry || !eng::InImage(reinterpret_cast<void*>(entry)) || !eng::ParamsOf(fn).empty()) continue;
        gGroupOriginal[i] = entry;
        std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &kGroupHooks[i], sizeof(NativeFunction));
    }
    std::string hooked;
    for (int i = 0; i < 4; ++i) hooked += std::string(gGroupOriginal[i] ? " " : " NOT ") + kGroupFunctions[i];
    hostlog::Info("together: groups off while editing together:" + hooked);
}

// A joiner never writes the host's map to their own disk. The editor saves from several places (its save button, its
// exit: W_MapEditor.HandleExit -> W_SaveLoad.SaveMapWithExitCallback, menu actions -> ManuallySaveNoCallback), all
// ending in SKGMLEStatics.SaveMapToFile, which writes the .balledit through the game's CreateFileW. So while this game
// is a client of a session, the game exe's CreateFileW refuses to open any file in the game's maps folder or its
// subfolders (a publish writes its upload copy, a .ballmap, into one) for writing (ERROR_ACCESS_DENIED: the game reports
// a failed save, and a publish has nothing to upload). The hook sits in the exe's import slot after anything
// already there (the sandbox's own file hook in a test copy) and passes every other call straight on. CreateFileW can
// be called from any thread, so it reads only a flag and a folder the game thread sets.
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn gNextCreateFile = nullptr;
std::atomic<bool> gRefuseMapWrites{false};
wchar_t gMapsFolder[MAX_PATH] = {};     // set once (game thread) before gRefuseMapWrites is ever true
std::atomic<long> gRefusedWrites{0};
HANDLE WINAPI GuardedCreateFile(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags,
                                HANDLE templ) {
    if (gRefuseMapWrites.load() && name && (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) && gMapsFolder[0]) {
        if (name[0] == L'\\' && name[1] == L'\\' && (name[2] == L'?' || name[2] == L'.') && name[3] == L'\\') name += 4;
        const size_t folder = wcslen(gMapsFolder), len = wcslen(name);
        // the maps folder, any slash style, and a .balledit (or its temp copy)
        bool inside = len > folder;
        for (size_t i = 0; inside && i < folder; ++i) {
            wchar_t a = towlower(name[i]), b = towlower(gMapsFolder[i]);
            if (a == L'/') a = L'\\';
            if (b == L'/') b = L'\\';
            inside = a == b;
        }
        if (inside) {                   // the maps folder and its subfolders (a publish's upload copy, a .ballmap): any file
            ++gRefusedWrites;
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
    }
    return gNextCreateFile(name, access, share, sa, disposition, flags, templ);
}
void** ExeImportSlot(const char* dll, const char* function) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), function) == 0) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}
bool gWriteGuardTried = false;
void GuardMapWrites() {
    const bool client = IsClient();
    // the folder from the game (its level manager), read again until the game answers (the fallback is a guess)
    static bool folderFromGame = false;
    if (client && !folderFromGame && !gRefuseMapWrites.load()) {
        folderFromGame = !tracks::LocalMapsFolder().empty();
        const std::wstring folder = SavedMapsFolder();
        if (folder.size() < MAX_PATH - 2) wcscpy(gMapsFolder, (folder + L"\\").c_str());
    }
    if (client && !gWriteGuardTried) {
        gWriteGuardTried = true;
        void** slot = ExeImportSlot("KERNEL32.dll", "CreateFileW");
        DWORD old = 0;
        if (slot && *slot != reinterpret_cast<void*>(&GuardedCreateFile) && VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            gNextCreateFile = reinterpret_cast<CreateFileWFn>(*slot);
            *slot = reinterpret_cast<void*>(&GuardedCreateFile);
            VirtualProtect(slot, sizeof(void*), old, &old);
            hostlog::Info("together: joiners' saves of the host's map are refused");
        } else {
            hostlog::Warn("together: the game's CreateFileW import wasn't found: a joiner's save isn't refused");
        }
    }
    static int clientWorld = -1;            // the world this game was a client in
    if (client) clientWorld = game::Generation();
    const bool stillThatWorld = clientWorld >= 0 && game::Generation() == clientWorld;
    if (!client && stillThatWorld && editor::Open()) {
        static int sentHome = -1;
        if (sentHome != clientWorld) {
            sentHome = clientWorld;
            hostlog::Info("together: the connection to the host is gone: back to the menu");
            Leave();
        }
    }
    if (!client && !stillThatWorld) clientWorld = -1;
    const bool refuse = (client || stillThatWorld) && gNextCreateFile && gMapsFolder[0];
    static long lastRefused = 0;
    if (gRefusedWrites.load() != lastRefused) {
        lastRefused = gRefusedWrites.load();
        Notice("only the host can save this map");
    }
    gRefuseMapWrites = refuse;
}

// In a player's game, P2P through Steam's relay only: ICE (Steam's direct peer-to-peer path, which tells each side the
// other's address) is switched off for this game (ISteamNetworkingUtils SetGlobalConfigValueInt32:
// k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable (104) = k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable (0)).
bool gRelayOnly = false;
bool RelayOnly() {
    if (gRelayOnly || sandbox::On()) return true;
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    auto accessor = dll ? reinterpret_cast<void* (*)()>(reinterpret_cast<void*>(GetProcAddress(dll, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004"))) : nullptr;
    auto set = dll ? reinterpret_cast<bool (*)(void*, int, int32_t)>(
                         reinterpret_cast<void*>(GetProcAddress(dll, "SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32"))) : nullptr;
    void* utils = accessor ? accessor() : nullptr;
    gRelayOnly = utils && set && set(utils, 104, 0) && sandbox::RefuseSteamIpSockets();
    hostlog::Info(std::string("together: Steam's direct peer-to-peer path (ICE) ") + (gRelayOnly ? "off and IP sockets refused: relay only" : "or the IP sockets COULD NOT be switched off: no sessions"));
    return gRelayOnly;
}

// After the editor starts listening: in a player's game its world's net driver must be Steam's; anything else (it
// can't be, with the fallback above, but it is checked where it matters) ends the session at once.
bool gListenChecked = false;
void CheckListenDriver() {
    if (!IsServer()) {
        gListenChecked = false;
        return;
    }
    if (gListenChecked) return;
    gListenChecked = true;
    Obj world = eng::Call(eng::FindCdo("KismetSystemLibrary"), "GetOuterObject", game::PlayerController()).ReturnObj();
    Obj controller = game::PlayerController();
    Obj level = controller ? eng::OuterOf(controller) : nullptr;     // PersistentLevel
    Obj w = level ? eng::OuterOf(level) : world;                    // the World
    Obj driver = w && eng::FindProp(eng::ClassOf(w), "NetDriver") ? eng::ReadObj(w, "NetDriver") : nullptr;
    const std::string cls = driver ? eng::ObjName(eng::ClassOf(driver)) : "none";
    hostlog::Info("together: listening with " + cls);
    if (!sandbox::On() && cls != "SteamSocketsNetDriver") {
        hostlog::Error("together: the editor listened with " + cls + ", not Steam's sockets: the session is ended");
        Leave();
    }
}

}  // namespace

void Frame() {
    HookOpenLevel();
    HookClientTravel();
    UseFallbackDriverInSandbox();
    CheckListenDriver();
    KeepLobbyWithSession();
    GuardMapWrites();
    editor::SetNetworked(IsServer() || IsClient());
    if (IsClient()) gJoinPending = false;   // joined: a later failure isn't this join's
    editor::SetJoiner(IsClient());
    static bool hooksSet = false;
    if (!hooksSet) {
        hooksSet = true;
        editor::SetJoinerRefused([](const std::string& what) { Notice(what); });
        editor::SetAfterHistoryStep([] {
            if (IsServer()) editor::WakePieces();
        });
    }
    if (IsServer() || IsClient()) HookGroups(editor::OwnHandler());
    if (IsServer()) RefuseJoinersEditorCalls();
    SetUpClientEditor();
    ShareHistory();
}

void ListenNextEditor(bool on) {
    if (on && gWrapOverflow) {
        Notice("this game's editor can't be shared safely (more server functions than the host can check)");
        on = false;
    }
    if (on && !(SteamOnlyInPlayersGame() && RelayOnly())) {
        Notice("this game can't host sessions safely (Steam networking isn't available)");
        on = false;
    }
    gArmed = on;
}
void SetReopenSuffix(const std::string& s) { gSuffix = s; }
void SetSandboxDriver(const std::string& d) {
    gSandboxDriver = d;
    gDriverChecked = false;
}
bool ListenArmed() { return gArmed; }

// What this game is, asked of the engine once a frame (every check here runs often).
long long gModeFrame = -1;
bool gIsServer = false, gIsClient = false;
void ReadMode() {
    const long long frame = game::Generation() * 1000000ll + static_cast<long long>(GetTickCount64() / 16);
    if (frame == gModeFrame) return;
    gModeFrame = frame;
    const bool controller = game::PlayerController() != nullptr;
    const bool server = controller && SystemBool("IsServer");
    gIsServer = server && !SystemBool("IsStandalone");
    gIsClient = controller && !server;
}
bool IsServer() {
    ReadMode();
    return gIsServer;
}
bool IsClient() {
    ReadMode();
    return gIsClient;
}

std::string NetMode() {
    if (!game::PlayerController()) return "none";
    if (SystemBool("IsStandalone")) return "standalone";
    return SystemBool("IsServer") ? "listen server" : "client";
}

bool Join(const std::string& address) {
    if (address.empty()) return false;
    // a player's game joins only through Steam ("steam.<Steam id>"); a test copy only on this computer (loopback)
    if (sandbox::On() ? address.rfind("127.0.0.1:", 0) != 0 : address.rfind("steam.", 0) != 0 ||
                            address.find_first_not_of("0123456789", 6) != std::string::npos || address.size() < 10) {
        hostlog::Warn("together: refused to join " + address + " (not a " + (sandbox::On() ? "loopback" : "Steam") + " address)");
        return false;
    }
    if (!(SteamOnlyInPlayersGame() && RelayOnly()) || !gClientTravelOriginal) {
        Notice("this game can't join sessions safely (Steam networking isn't available)");
        return false;
    }
    gJoinPending = true;
    hostlog::Info("together: joining " + address);
    return CallOpenLevel(address, "");
}

bool gJoinPending = false;
bool JoinPending() { return gJoinPending && !IsClient(); }
bool Leave() {
    gJoinPending = false;
    return CallOpenLevel("Map_MainMenu", "");
}

namespace {
// The game instance's BeginLevelTransition(LevelName, EGameModes, map text, UserLevelName, level data), as the Create
// page's "+new" (no text) and a files entry (WBP_TrackLibraryEntry.LoadMapForEdit: the file's text) call it.
bool BeginEditorTransition(const uint8_t* text, size_t textSize, const std::wstring& levelName) {
    Obj gi = GameInstance();
    if (!gi || !eng::FindFunction(eng::ClassOf(gi), "BeginLevelTransition")) return false;
    const eng::Params name = eng::Call(eng::FindCdo("KismetStringLibrary"), "Conv_StringToName", eng::FString{L"Map_LevelEditorMain", 20, 20});
    size_t nameSize = 0;
    const uint8_t* fname = name.Return(&nameSize);
    if (!fname) return false;
    const eng::FString empty{L"", 1, 1};
    const eng::FString level{levelName.c_str(), static_cast<int32_t>(levelName.size() + 1), static_cast<int32_t>(levelName.size() + 1)};
    eng::Params go(eng::FunctionOn(gi, "BeginLevelTransition"));
    go.SetArg(0, fname, nameSize);
    go.SetArg(1, uint8_t{2});                       // EGameModes 2: the editor
    if (text) go.SetArg(2, text, textSize);
    else go.SetArg(2, empty);
    go.SetArg(3, levelName.empty() ? empty : level);
    go.SetArg(4, static_cast<Obj>(nullptr));
    return eng::Invoke(gi, go);
}
}  // namespace

bool NewMap() { return BeginEditorTransition(nullptr, 0, L""); }

// The game's own folder of the player's maps (its level manager's GetCustomMapDirectory: a test copy's is under its
// -userdir), without a trailing slash, as a full path.
std::wstring SavedMapsFolder() {
    std::wstring dir = eng::Widen(tracks::LocalMapsFolder());
    if (dir.empty()) dir = hostlog::DataDir() + L"\\..\\UserSavedMaps";
    while (!dir.empty() && (dir.back() == L'/' || dir.back() == L'\\')) dir.pop_back();
    wchar_t full[MAX_PATH];
    const DWORD n = GetFullPathNameW(dir.c_str(), MAX_PATH, full, nullptr);
    return n > 0 && n < MAX_PATH ? std::wstring(full, n) : dir;
}

std::vector<std::string> SavedMaps() {
    std::vector<std::string> out;
    const std::wstring dir = SavedMapsFolder();
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((dir + L"\\Map_LevelEditorMain&*.balledit").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring path = dir + L"\\" + f.cFileName;
        out.push_back(eng::Narrow(path.c_str(), static_cast<int>(path.size())));
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return out;
}

bool OpenMap(const std::string& path) {
    // only the game's own saved maps: a file in its UserSavedMaps folder named Map_LevelEditorMain&<name>.balledit
    const std::wstring wpath = eng::Widen(path), dir = SavedMapsFolder() + L"\\";
    if (_wcsnicmp(wpath.c_str(), dir.c_str(), dir.size()) != 0 || wpath.find(L"..") != std::wstring::npos) return false;
    const size_t amp = wpath.find_last_of(L'&'), dot = wpath.rfind(L".balledit");
    if (amp == std::wstring::npos || dot == std::wstring::npos || dot < amp) return false;
    static std::vector<eng::Params> kept;           // the loaded text stays owned by these buffers (one per open)
    Obj statics = eng::FindCdo("SKGMLEStatics");
    if (!statics) return false;
    if (kept.size() > 2) kept.erase(kept.begin());         // the travel has long since copied the older ones
    kept.emplace_back(eng::FunctionOn(statics, "LoadMapFromPath"));
    eng::Params& load = kept.back();
    load.SetArg(0, eng::FString{wpath.c_str(), static_cast<int32_t>(wpath.size() + 1), static_cast<int32_t>(wpath.size() + 1)});
    if (!eng::Invoke(statics, load) || !load.ReturnBool()) return false;
    size_t size = 0;
    const uint8_t* text = load.Get("OutString", &size);
    if (!text) return false;
    hostlog::Info("together: opening " + path + " in the editor");
    return BeginEditorTransition(text, size, wpath.substr(amp + 1, dot - amp - 1));
}

int Players() {
    Obj controller = game::PlayerController();
    Obj state = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameState", controller).ReturnObj() : nullptr;
    return state ? static_cast<int>(eng::ReadObjArray(state, "PlayerArray").size()) : 0;
}

std::string OwnAddress() {
    if (sandbox::On()) return "127.0.0.1:" + std::to_string(ListenPort());
    const uint64_t id = steam::OwnSteamId();
    return id ? "steam." + std::to_string(id) : "";
}

int gOwnerPlugin = -1;
void SetOwner(int plugin) { gOwnerPlugin = plugin; }
void PluginStopped(int plugin) {
    if (plugin != gOwnerPlugin) return;
    gOwnerPlugin = -1;
    gArmed = false;
    gSessionCode.clear();
    gJoinCode.clear();
    const bool joiner = IsClient();
    lobby::Leave();                     // a host keeps its editor (and its unsaved work): with no lobby nobody stays in
    if (joiner) Leave();
    hostlog::Info("together: the plugin running the session stopped: the session is over");
}
std::string SteamIdsStatus() {
    std::string out;
    Obj controller = game::PlayerController();
    Obj gameState = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameState", controller).ReturnObj() : nullptr;
    if (gameState)
        for (Obj ps : eng::ReadObjArray(gameState, "PlayerArray")) out += " " + std::to_string(PlayerIdOf(ps)) + "=" + SteamIdOf(ps);
    return out + " own=" + lobby::OwnId();
}
// A session code: 8 of the 32 letters and digits that can't be mixed up, from the system's random source
// (BCryptGenRandom: nothing anyone can work out from the time or the lobby).
std::string NewSessionCode() {
    static const char* const kLetters = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint8_t bytes[8] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return "";
    std::string code;
    for (uint8_t b : bytes) code += kLetters[b % 32];
    return code;
}
void SetSessionCode(const std::string& code) {
    gSessionCode = code;
    gExpectedProof.clear();
    gProved.clear();
}
void SetJoinCode(const std::string& code) {
    gJoinCode = code;
    gCodeAccepted = false;
}
bool CodeAccepted() { return gCodeAccepted; }
std::string TakeNotice() {
    std::string n;
    n.swap(gNotice);
    return n;
}

int ListenPort() {
    const wchar_t* line = GetCommandLineW();
    const wchar_t* at = line ? wcsstr(line, L"-port=") : nullptr;
    if (!at) at = line ? wcsstr(line, L"-PORT=") : nullptr;
    return at ? _wtoi(at + 6) : 7777;
}

int ConnectedClients() {
    if (!IsServer()) return -1;
    Obj state = eng::Call(eng::FindCdo("GameplayStatics"), "GetGameState", game::PlayerController()).ReturnObj();
    if (!state) return -1;
    return static_cast<int>(eng::ReadObjArray(state, "PlayerArray").size()) - 1;
}

std::string Status() {
    return "net=" + NetMode() + " armed=" + (gArmed ? "1" : "0") + " port=" + std::to_string(ListenPort()) +
           " clients=" + std::to_string(ConnectedClients()) + " hook=" + (gOpenOriginal ? "1" : gHookFailed ? "failed" : "0") +
           " lastTravel=" + gLastTravel + " driver=" + gDriverNote + gSteamOnlyNote + " client=" + gClientNote + " recached=" + std::to_string(gRecached) + " savingHidden=" + std::to_string(gHiddenSaving) + " nameRestores=" + std::to_string(gNameRestores) + " stepsSent=" + std::to_string(gSentSteps) + " stepsRun=" + std::to_string(gRanSteps) + " ownHistories=" + std::to_string(gOwnHistories) + " wrapped=" + std::to_string(gWrapped) + " kicked=" + std::to_string(gKicked) + " claims=" + std::to_string(gClaims.size()) + " proved=" + std::to_string(gProved.size()) + " refusedWrites=" + std::to_string(gRefusedWrites.load());
}

}  // namespace together
