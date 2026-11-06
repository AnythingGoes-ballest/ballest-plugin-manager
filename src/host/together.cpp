#include "together.hpp"

#include <windows.h>

#include <cstdint>
#include <utility>
#include <array>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <map>
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
    if (url.empty()) return;
    gArmed = false;
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
void SteamOnlyInPlayersGame() {
    if (gSteamOnlyChecked || sandbox::On()) return;
    Obj gi = GameInstance();
    Obj engine = gi ? eng::OuterOf(gi) : nullptr;
    if (!engine) return;
    gSteamOnlyChecked = true;
    const eng::Prop defs = eng::FindProp(eng::ClassOf(engine), "NetDriverDefinitions");
    if (!defs) return;
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
            return;
        }
        std::memcpy(def + 16, def + 8, 8);         // fallback := SteamSockets
        gSteamOnlyNote = "Steam networking only (" + cls + ", no fallback)";
        return;
    }
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
    gGiVtable.assign(vtable, vtable + 400);
    gGiProcessEvent = reinterpret_cast<ProcessEventFn>(vtable[layout::kProcessEventVtableSlot]);
    gGiVtable[layout::kProcessEventVtableSlot] = reinterpret_cast<uintptr_t>(&GiProcessEvent);
    uintptr_t* copy = gGiVtable.data();
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
ULONGLONG gLastClientCheck = 0;
std::string gClientNote;
Obj LiveInstance(const char* className) {
    Obj cls = eng::FindClass(className), found = nullptr;
    if (!cls) return nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o)) found = o;
        return found == nullptr;
    });
    return found;
}
// A client's handler keeps its own list of pieces (AllActors), filled once by Init: pieces the host (or another
// player) adds later arrive by replication but not in that list, so the client's select-all, outliner and its own save
// would miss them. The handler's GetAllActors(bReCache) builds the list again from the world; on a client the host
// asks for that once a second.
int gRecached = 0;
void RecacheClientPieces(Obj handler) {
    eng::Params p(eng::FunctionOn(handler, "GetAllActors"));
    p.SetArg(0, uint8_t{1});
    p.SetArg(1, uint8_t{0});
    if (eng::Invoke(handler, p)) ++gRecached;
}

// The map is the host's alone (user decision 2026-11-05): a client can't save or publish it. Its editor's save button
// (WBP_SaveButton), publish tab (WBP_PublishButton), publisher tools (WBP_PublisherToolsButton, which the game shows
// and hides itself) and publish window are kept hidden, checked again each second.
int gHiddenSaving = 0;
void HideClientSaving() {
    Obj editorUi = LiveInstance("W_MapEditor_C");
    if (!editorUi) return;
    for (const char* name : {"WBP_SaveButton", "WBP_PublishButton", "WBP_PublisherToolsButton", "WBP_PublishWindow"}) {
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
int gLastPlayers = -1;
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
        return;
    }
    const int players = Players();
    if (players != gLastPlayers) {
        gLastPlayers = players;
        gPlayersChangedAt = GetTickCount64();
    }
    // a moment after the change (the joiner's arrival runs over a few frames), once
    if (gPlayersChangedAt && GetTickCount64() - gPlayersChangedAt > 1500) {
        gPlayersChangedAt = 0;
        if (PassMapName(GameInstanceMapName())) ++gNameRestores;
    }
}

void SetUpClientEditor() {
    if (GetTickCount64() - gLastClientCheck < 1000) return;
    gLastClientCheck = GetTickCount64();
    KeepHostMapName();
    // the host's own list misses pieces another player's handler brings back (a joiner's undo of a delete): rebuilt too
    if (IsServer())
        if (Obj own = editor::OwnHandler()) RecacheClientPieces(own);
    if (!IsClient()) return;
    Obj controller = game::PlayerController();
    Obj pawn = controller ? eng::Call(controller, "K2_GetPawn").ReturnObj() : nullptr;
    if (!pawn || !eng::FindProp(eng::ClassOf(pawn), "SKGMLEHandler")) return;
    if (eng::Get(gSetUpPawn) == pawn) {
        if (Obj handler = eng::ReadObj(pawn, "SKGMLEHandler")) RecacheClientPieces(handler);
        HideClientSaving();
        return;
    }
    Obj handler = eng::ReadObj(pawn, "SKGMLEHandler");
    if (!handler) return;
    gSetUpPawn = eng::MakeWeak(pawn);
    eng::WriteBytes(pawn, "SKGLevelEditorHandler", &handler, sizeof handler);
    const bool init = eng::Call(handler, "Init").Invoked();
    const bool hud = eng::Call(pawn, "HandleHUD").Invoked();
    eng::Call(handler, "SetGizmoType", uint8_t{20});
    gClientNote = std::string("client editor set up (init ") + (init ? "ok" : "failed") + ", hud " + (hud ? "ok" : "failed") +
                  ", ui " + (LiveInstance("W_MapEditor_C") ? "made" : "missing") + ")";
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
Obj HandlerOfPlayer(int playerId) {
    Obj controller = game::PlayerController();
    Obj gameState = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameState", controller).ReturnObj() : nullptr;
    if (!gameState) return nullptr;
    for (Obj ps : eng::ReadObjArray(gameState, "PlayerArray")) {
        if (PlayerIdOf(ps) != playerId) continue;
        Obj pawn = eng::ReadObj(ps, "PawnPrivate");
        return pawn && eng::FindProp(eng::ClassOf(pawn), "SKGMLEHandler") ? eng::ReadObj(pawn, "SKGMLEHandler") : nullptr;
    }
    return nullptr;
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
constexpr int kMaxWrapped = 48;
NativeFunction gServerOriginal[kMaxWrapped] = {};
int gWrapped = 0;
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
void RunAsPlayer(int i, Obj context, uint8_t* frame, void* result) {
    Obj own = context && IsServer() && eng::FindProp(eng::ClassOf(context), "HistoryManagerOverride")
                  ? eng::ReadObj(context, "HistoryManagerOverride") : nullptr;
    Obj world = own ? WorldHistorySubsystem() : nullptr;
    Obj before = world ? eng::ReadObj(world, "HistoryManager") : nullptr;
    if (world && before && before != own) eng::WriteBytes(world, "HistoryManager", &own, sizeof own);
    gServerOriginal[i](context, frame, result);
    if (world && before && before != own) eng::WriteBytes(world, "HistoryManager", &before, sizeof before);
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
            if (name.rfind("Server_", 0) != 0 || gWrapped >= kMaxWrapped) continue;
            Obj fn = eng::FindFunction(c, name);
            NativeFunction entry = nullptr;
            if (fn) std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
            if (!entry || !eng::InImage(reinterpret_cast<void*>(entry))) continue;
            gServerOriginal[gWrapped] = entry;
            const NativeFunction hooked = kServerWrappers[static_cast<size_t>(gWrapped)];
            std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
            ++gWrapped;
        }
    }
    hostlog::Info("together: " + std::to_string(gWrapped) + " of the editor's server functions record into each player's own history");
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
    const bool ran = eng::Call(handler, redo ? "Redo" : "Undo").Invoked();
    eng::WriteBytes(world, "HistoryManager", &before, sizeof before);
    std::memcpy(handler + kHandlerGizmoSubsystem, &theirs, sizeof theirs);
    return ran;
}

// Only the lobby's members are in the editor. Anyone on Steam can reach the host's Steam id, so a connection that
// isn't one of the lobby's members is dropped: a joiner says "hello <PlayerId>" over the lobby once connected (and
// every few seconds: only members can send lobby messages), and a player the host has no member's claim for within
// kClaimMs, or whose member has left the lobby, is kicked as the engine's own GameSession kicks (ClientWasKicked, then
// the player's controller destroyed, which closes the connection).
constexpr ULONGLONG kHelloEveryMs = 4000, kClaimMs = 15000;
ULONGLONG gLastHello = 0;
std::map<int, std::string> gClaims;     // PlayerId -> the lobby member who said it's them
std::map<int, ULONGLONG> gFirstSeen;    // PlayerId -> when the host first saw that player
int gKicked = 0;
void Kick(Obj state, const std::string& why) {
    Obj controller = eng::ReadObj(state, "Owner");
    if (!controller) return;
    hostlog::Warn("together: player " + std::to_string(PlayerIdOf(state)) + " removed from the session: " + why);
    // the client is sent home (APlayerController.ClientReturnToMainMenuWithTextReason, which the game's own code runs
    // on the client), and its pawn and controller go: even a client that ignored the message can't edit any more
    const eng::Params text = eng::MakeText("you're not a member of this session");
    if (const uint8_t* t = text.Return())
        for (const char* fn : {"ClientWasKicked", "ClientReturnToMainMenuWithTextReason"}) {
            eng::Params p(eng::FunctionOn(controller, fn));
            p.SetArg(0, t, 16);
            eng::Invoke(controller, p);
        }
    if (Obj pawn = eng::ReadObj(state, "PawnPrivate")) eng::Call(pawn, "K2_DestroyActor");
    eng::Call(controller, "K2_DestroyActor");
    ++gKicked;
}
void GateConnections(const std::vector<lobby::Member>& members) {
    Obj controller = game::PlayerController();
    Obj gameState = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameState", controller).ReturnObj() : nullptr;
    Obj own = LocalPlayerState();
    if (!gameState) return;
    const ULONGLONG now = GetTickCount64();
    std::map<int, ULONGLONG> seen;
    for (Obj ps : eng::ReadObjArray(gameState, "PlayerArray")) {
        if (ps == own) continue;
        const int id = PlayerIdOf(ps);
        if (id < 0) continue;
        seen[id] = gFirstSeen.count(id) ? gFirstSeen[id] : now;
        auto claim = gClaims.find(id);
        bool stillMember = false;
        if (claim != gClaims.end())
            for (const auto& m : members) stillMember = stillMember || m.id == claim->second;
        if (claim == gClaims.end() ? now - seen[id] > kClaimMs : !stillMember) {
            Kick(ps, claim == gClaims.end() ? "no lobby member claimed this connection" : "their lobby member left");
            gClaims.erase(id);
            seen.erase(id);
        }
    }
    gFirstSeen.swap(seen);
}

void ShareHistory() {
    const bool inLobby = lobby::State() == "in lobby";
    const bool redirect = inLobby && IsClient();
    if (redirect != gRedirecting) {
        gRedirecting = redirect;
        editor::SetHistoryRedirect(redirect ? std::function<bool(bool)>([](bool redo) {
            const int id = PlayerIdOf(LocalPlayerState());
            if (id >= 0 && lobby::Send(std::string(redo ? "redo " : "undo ") + std::to_string(id))) ++gSentSteps;
            return true;                // never the joiner's own (empty) history
        })
                                            : std::function<bool(bool)>());
    }
    if (!IsServer()) {
        gClaims.clear();
        gFirstSeen.clear();
    }
    if (IsServer()) {
        WrapServerFunctions(editor::OwnHandler());
        GiveJoinersOwnHistories();
    }
    if (!inLobby) return;
    if (IsClient() && GetTickCount64() - gLastHello > kHelloEveryMs) {
        const int id = PlayerIdOf(LocalPlayerState());
        if (id >= 0 && lobby::Send("hello " + std::to_string(id))) gLastHello = GetTickCount64();
    }
    const std::vector<lobby::Message> messages = lobby::Receive();
    if (!IsServer() || !lobby::IsOwner()) return;
    const std::string own = lobby::OwnId();
    const std::vector<lobby::Member> members = lobby::Members();
    for (const auto& m : messages) {
        if (m.from == own) continue;
        bool member = false;
        for (const auto& x : members) member = member || x.id == m.from;
        if (!member) continue;
        if (m.text.rfind("hello ", 0) == 0) {
            gClaims[std::atoi(m.text.c_str() + 6)] = m.from;
            continue;
        }
        const bool undo = m.text.rfind("undo ", 0) == 0, redo = m.text.rfind("redo ", 0) == 0;
        if (!undo && !redo) continue;
        Obj handler = HandlerOfPlayer(std::atoi(m.text.c_str() + 5));
        if (!handler || handler == editor::OwnHandler()) continue;       // never the host's own history
        if (RunPlayerStep(handler, redo)) ++gRanSteps;
    }
    GateConnections(members);
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
    UseFallbackDriverInSandbox();
    SteamOnlyInPlayersGame();
    CheckListenDriver();
    SetUpClientEditor();
    ShareHistory();
}

void ListenNextEditor(bool on) { gArmed = on; }
void SetReopenSuffix(const std::string& s) { gSuffix = s; }
void SetSandboxDriver(const std::string& d) {
    gSandboxDriver = d;
    gDriverChecked = false;
}
bool ListenArmed() { return gArmed; }

bool IsServer() { return SystemBool("IsServer") && !SystemBool("IsStandalone"); }
bool IsClient() { return game::PlayerController() && !SystemBool("IsServer"); }

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
    hostlog::Info("together: joining " + address);
    return CallOpenLevel(address, "");
}

bool Leave() { return CallOpenLevel("Map_MainMenu", ""); }

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
           " lastTravel=" + gLastTravel + " driver=" + gDriverNote + gSteamOnlyNote + " client=" + gClientNote + " recached=" + std::to_string(gRecached) + " savingHidden=" + std::to_string(gHiddenSaving) + " nameRestores=" + std::to_string(gNameRestores) + " stepsSent=" + std::to_string(gSentSteps) + " stepsRun=" + std::to_string(gRanSteps) + " ownHistories=" + std::to_string(gOwnHistories) + " wrapped=" + std::to_string(gWrapped) + " kicked=" + std::to_string(gKicked) + " claims=" + std::to_string(gClaims.size());
}

}  // namespace together
