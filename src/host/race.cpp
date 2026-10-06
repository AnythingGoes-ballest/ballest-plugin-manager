#include "race.hpp"
#if __has_include("sandbox.hpp")
#include "sandbox.hpp"
#define HOST_HAS_SANDBOX 1
#endif

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "widgets.hpp"

using eng::Obj;
namespace w = ui::widgets;

namespace race {
namespace {

bool gOnTrack = false, gActive = false, gComplete = false;
int gRestarts = 0, gRunId = -1;
eng::Weak gBall;                // the ball whose run was read last
int32_t gLastRun = -1;          // that ball's last raced run id (kept between runs, on the results screen)

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
struct Rot {
    double pitch = 0, yaw = 0, roll = 0;
};
constexpr uint64_t kNoBone = 0;             // FName None, for the physics functions' BoneName

Obj Ball() {
    Obj controller = game::PlayerController();
    Obj pawn = controller ? eng::Call(controller, "K2_GetPawn").ReturnObj() : nullptr;
    return pawn && eng::FindProp(eng::ClassOf(pawn), "RaceId") ? pawn : nullptr;
}

// The ball being played: the controlled pawn when it's a ball, else the view target when that is one (in the track
// editor's test runs the camera follows the ball).
Obj PlayedBall() {
    if (Obj b = Ball()) return b;
    Obj controller = game::PlayerController();
    Obj target = controller ? eng::Call(controller, "GetViewTarget").ReturnObj() : nullptr;
    return target && eng::FindProp(eng::ClassOf(target), "RaceId") ? target : nullptr;
}

Obj Library(const char* name) { return eng::FindCdo(name); }

// An FText property as a string.
std::string TextProperty(Obj o, const char* name) {
    const eng::Prop p = o ? eng::FindProp(eng::ClassOf(o), name) : eng::Prop{};
    if (!p || eng::KindOf(p) != "TextProperty") return "";
    Obj lib = Library("KismetTextLibrary");
    eng::Params convert(eng::FunctionOn(lib, "Conv_TextToString"));
    convert.SetArg(0, o + p.offset, static_cast<size_t>(p.size));
    const uint8_t* s = eng::Invoke(lib, convert) ? convert.Return() : nullptr;
    return s ? eng::ReadFString(s) : "";
}

// "7.035", "1:05.123" -> seconds; 0 if it isn't a time.
double ParseTime(const std::string& text) {
    double minutes = 0, seconds = 0;
    const size_t colon = text.find(':');
    char* end = nullptr;
    if (colon != std::string::npos) {
        minutes = std::strtod(text.substr(0, colon).c_str(), &end);
        seconds = std::strtod(text.c_str() + colon + 1, &end);
    } else {
        seconds = std::strtod(text.c_str(), &end);
    }
    const double t = minutes * 60 + seconds;
    return t > 0 && t < 360000 ? t : 0;
}

// --- the track --------------------------------------------------------------------------------------------------------
Track gTrack;
bool gTrackDone = false;
double gNextTrackRead = 0;
int gTaintedRun = -2;                   // the run the host touched (-1: before a run started, on this map)
int gTaintedGeneration = -1;
std::string gTaintWhy;
int gTrackGeneration = -1;

Obj RaceUi() {
    Obj controller = game::PlayerController();
    Obj ui = eng::ReadObj(controller, "DONOTACCESS_UseGetter_CachedRaceUIManager");
    if (ui && eng::IsLive(ui)) return ui;
    static Obj cls = nullptr;
    if (!cls) cls = eng::FindClass("WBP_RaceUIManager_C");
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o)) found = o;
        return found == nullptr;
    });
    return found;
}

void ReadTrack() {
    Obj controller = game::PlayerController();
    if (!controller) return;
    Obj world = eng::OuterOf(eng::OuterOf(controller));                  // controller -> level -> world
    const std::string worldName = eng::ObjName(world);
    Track t = gTrack;
    t.custom = worldName == "Map_TrackUGCPlayback";
    Obj ui = RaceUi();
    if (Obj header = eng::ReadObj(ui, "WBP_Header")) {
        const std::string title = w::ReadText(eng::ReadObj(header, "Title"));
        const std::string author = w::ReadText(eng::ReadObj(header, "authorText"));
        if (!title.empty()) t.name = title;
        t.author = author == "playername" ? "" : author;
    }
    if (t.name.empty()) t.name = TextProperty(controller, "NameOfMap");
    double rowTime = 0, controllerTime = 0;
    if (Obj goals = eng::ReadObj(ui, "WBP_IngameGoalsGroup"))
        rowTime = ParseTime(w::ReadText(eng::ReadObj(eng::ReadObj(goals, "WBP_AuthorGoal"), "UserGoal_Numeric")));
    eng::ReadBytes(controller, "AuthorTime", &controllerTime, sizeof controllerTime);
    // The controller's AuthorTime is a default (17.243, measured on the game's own tracks too): only the row counts.
    t.authorTime = rowTime;
    if (t.custom) {
        Obj instance = eng::Call(Library("GameplayStatics"), "GetGameInstance", controller).ReturnObj();
        const eng::Prop p = instance ? eng::FindProp(eng::ClassOf(instance), "TargetLevelName") : eng::Prop{};
        std::string path = p ? eng::ReadFString(instance + p.offset) : "";
        for (char& c : path)
            if (c == '\\') c = '/';
        const size_t slash = path.rfind('/');
        std::string file = slash == std::string::npos ? path : path.substr(slash + 1), id = "local";
        if (slash != std::string::npos) {
            const size_t parent = path.rfind('/', slash - 1);
            const std::string folder = path.substr(parent == std::string::npos ? 0 : parent + 1, slash - (parent == std::string::npos ? 0 : parent + 1));
            if (!folder.empty() && folder.find_first_not_of("0123456789") == std::string::npos) id = folder;
        }
        if (const size_t amp = file.find('&'); amp != std::string::npos) file = file.substr(amp + 1);
        if (const size_t dot = file.rfind('.'); dot != std::string::npos) file = file.substr(0, dot);
        t.key = file.empty() ? "" : "custom:" + id + ":" + file;
        // The picture the game keeps beside the map ("<track>_<author>.jpg", measured in workshop folders; a local map
        // has one once it has been published).
        t.image = "";
        if (slash != std::string::npos && !file.empty()) {
            const std::string jpg = path.substr(0, slash + 1) + file + ".jpg";
            if (GetFileAttributesW(eng::Widen(jpg).c_str()) != INVALID_FILE_ATTRIBUTES) t.image = jpg;
        }
    } else {
        t.key = "map:" + worldName;
    }
    const bool changed = t.key != gTrack.key || t.name != gTrack.name || t.author != gTrack.author || t.authorTime != gTrack.authorTime;
    gTrack = t;
    // Read until the race has started: before that, the race UI can still show placeholders.
    gTrackDone = gActive && !t.key.empty() && !t.name.empty() && t.authorTime > 0;
    if (changed || gTrackDone) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%.3f (medal row %.3f, controller %.3f)%s", t.authorTime, rowTime, controllerTime, gTrackDone ? "" : ", still reading");
        hostlog::Info("race: track " + t.key + " \"" + t.name + "\" by \"" + t.author + "\", author time " + buf);
    }
}

// --- the ball's state ---------------------------------------------------------------------------------------------------
std::vector<Obj> Components(Obj actor, const char* className) {
    std::vector<Obj> out;
    Obj cls = eng::FindClass(className);
    if (!cls) return out;
    eng::Params p(eng::FunctionOn(actor, "K2_GetComponentsByClass"));
    p.Set("ComponentClass", cls);
    eng::Invoke(actor, p);
    struct {
        Obj* data;
        int32_t num, max;
    } array{};
    size_t size = 0;
    if (const uint8_t* r = p.Return(&size); r && size == sizeof array) std::memcpy(&array, r, sizeof array);
    for (int32_t i = 0; array.data && i < array.num && i < 64; ++i) out.push_back(array.data[i]);
    return out;
}

// Spring arms whose lag was switched off by a load, to switch back on a few frames later (the camera snaps first).
struct LagRestore {
    eng::Weak arm;
    bool lag, rotationLag;
};
std::vector<LagRestore> gLagRestores;
double gLagRestoreAt = 0;

// --- practice ------------------------------------------------------------------------------------------------------------
struct Hidden {
    eng::Weak object;
    int kind;                       // 0 widget (visibility byte), 1 actor (hidden in game), 2 trigger (collision off)
    uint8_t saved;                  // what it was: the widget's visibility, or the trigger's ECollisionEnabled
};
bool gPractice = false;
int gPracticeRun = -1, gPracticeGeneration = -1;
eng::Weak gPracticeBall;
std::vector<Hidden> gHidden;
double gNextPracticeApply = 0;
bool gTimerShifted = false;
constexpr double kPracticeTime = 999.999;

bool AlreadyHidden(Obj o, int kind) {
    for (const auto& h : gHidden)
        if (h.kind == kind && eng::Get(h.object) == o) return true;
    return false;
}

bool ApplyPractice() {
    Obj controller = game::PlayerController();
    if (!controller) return false;
    int timelines = 0, triggers = 0, liveTriggers = 0;
    // The race timer (BP_MyPlayerController): its two timelines stopped, the times it keeps at 999.999, the text the
    // timer shows (RaceTimeText, which the HUD reads) set once. The text block itself is left alone: setting it would
    // replace its binding.
    bool stopped = false;
    for (const char* timeline : {"GameTime2", "LapTime"})
        if (Obj t = eng::ReadObj(controller, timeline)) {
            ++timelines;
            if (eng::Call(t, "IsPlaying").ReturnBool()) {
                eng::Call(t, "Stop");
                stopped = true;
            }
        }
    for (const char* name : {"ActualRaceTime", "ActualLapTime"}) eng::WriteBytes(controller, name, &kPracticeTime, sizeof kPracticeTime);
    if (!gTimerShifted) {
        double start = 0;
        if (eng::ReadBytes(controller, "RealTimeStart", &start, sizeof start)) {
            start -= kPracticeTime;
            eng::WriteBytes(controller, "RealTimeStart", &start, sizeof start);
        }
        gTimerShifted = true;
        stopped = true;
    }
    if (stopped) {
        const eng::Params text = eng::MakeText("999:999.999");
        size_t size = 0;
        const uint8_t* ftext = text.Return(&size);
        if (ftext && size == 16) eng::WriteBytes(controller, "RaceTimeText", ftext, size);   // its one reference moves in
    }
    // Checkpoints and the finish (both BP_Checkpoint_C), the leaderboard, personal standing and ghosts.
    // A checkpoint's triggers are its query-only components (GoalHitbox1 and 2, with the begin-overlap handler; its
    // sprites are too, with none): only they lose their collision. The goal ring (Main, query and physics) stays solid.
    Obj checkpoint = eng::FindClass("BP_Checkpoint_C"), primitiveClass = eng::FindClass("PrimitiveComponent");
    Obj actorClass = eng::FindClass("Actor"), widgetClass = eng::FindClass("Widget");
    std::vector<Obj> hideClasses;
    for (const char* name : {"WBP_Leaderboard_C", "WBP_PersonalStanding_C", "BallestGhostRenderHost", "BallestGhostLabelCanvas"})
        if (Obj c = eng::FindClass(name)) hideClasses.push_back(c);
    constexpr uint8_t kNoCollision = 0, kQueryOnly = 1;
    eng::ForEachObject([&](Obj o) {
        if (eng::IsDefaultObject(o)) return true;
        if (checkpoint && primitiveClass && eng::IsA(o, primitiveClass) && eng::ClassOf(eng::OuterOf(o)) == checkpoint) {
            if (!AlreadyHidden(o, 2) && eng::Call(o, "GetCollisionEnabled").ReturnAs<uint8_t>(kNoCollision) == kQueryOnly) {
                eng::Call(o, "SetCollisionEnabled", kNoCollision);
                gHidden.push_back({eng::MakeWeak(o), 2, kQueryOnly});
            }
            if (AlreadyHidden(o, 2)) {
                ++triggers;
                if (eng::Call(o, "GetCollisionEnabled").ReturnAs<uint8_t>(kQueryOnly) != kNoCollision) ++liveTriggers;
            }
            return true;
        }
        for (Obj c : hideClasses)
            if (eng::IsA(o, c)) {
                if (eng::IsA(o, actorClass) && !AlreadyHidden(o, 1)) {
                    bool hidden = false;
                    eng::ReadBool(o, "bHidden", &hidden);
                    if (!hidden) {
                        eng::Call(o, "SetActorHiddenInGame", uint8_t{1});
                        gHidden.push_back({eng::MakeWeak(o), 1, 0});
                    }
                } else if (eng::IsA(o, widgetClass) && !AlreadyHidden(o, 0)) {
                    const uint8_t visibility = eng::Call(o, "GetVisibility").ReturnAs<uint8_t>(w::kCollapsed);
                    if (visibility != w::kCollapsed) {
                        w::SetVisibility(o, w::kCollapsed);
                        gHidden.push_back({eng::MakeWeak(o), 0, visibility});
                    }
                }
                break;
            }
        return true;
    });
    // Verified, or practice isn't on: the timer's two timelines found, and checkpoint triggers found, all off.
    return timelines == 2 && triggers > 0 && liveTriggers == 0;
}

void ReleasePractice(const std::string& why) {
    for (const auto& h : gHidden) {
        Obj o = eng::Get(h.object);
        if (!o) continue;
        if (h.kind == 0) w::SetVisibility(o, h.saved);
        else if (h.kind == 1) eng::Call(o, "SetActorHiddenInGame", uint8_t{0});
        else eng::Call(o, "SetCollisionEnabled", h.saved);
    }
    gHidden.clear();
    gPractice = false;
    gTimerShifted = false;
    hostlog::Info("race: practice off (" + why + ")");
}

// --- checkpoint respawns and falls ----------------------------------------------------------------------------------
// Read from the game's Blueprints (BP_MyPlayerController, MP_ActualCheckpoint_Strip, BP_KillZone):
//   * a respawn at a checkpoint (R once a checkpoint is cleared, or a fall after one) runs the controller's
//     EventResetBall with bRespawnOnly, the one caller of IncrementPlayerFaults: ActualRaceFaults (a double) goes up
//     by one. Before the first checkpoint both go back to the start instead, and the faults don't move.
//   * a fall is the kill zone calling the native BallestRestartBlueprintLibrary.BeginKillzoneRestart, which respawns
//     the ball a moment later (its delay is 1 s). That function's entry is wrapped (as the cosmetics wrap theirs) to
//     tell falls from R.
//   * the checkpoint a respawn goes to is the one whose bCurrent is on: a strip (or disc) the ball touches moves the
//     level's player start to its arrow and turns itself current and every other one off. Strips start out current
//     before any is touched (measured), so it has to be activated (bActivated) as well.
int gRespawns = 0, gFalls = 0;
eng::Weak gFaultsController;
double gLastFaults = 0;
double gFallAt = -100;                      // game::Seconds() of the last fall
bool gFallRespawnOpen = false;              // that fall's respawn and restart not yet seen (a fall before the first
bool gFallRestartOpen = false;              // checkpoint moves both the faults and the restart counter, measured)
constexpr double kFallToRespawn = 4;        // seconds: the game's delay is 1 s

using NativeFunction = void (*)(Obj context, uint8_t* frame, void* result);
NativeFunction gKillzoneOriginal = nullptr;

// Runs inside the game's own call: counts, then lets the game go on as usual.
void HookedKillzone(Obj context, uint8_t* frame, void* result) {
    ++gFalls;
    gFallAt = game::Seconds();
    gFallRespawnOpen = gFallRestartOpen = true;
    gKillzoneOriginal(context, frame, result);
}

void WatchKillzone() {
    static bool done = false;
    if (done) return;
    Obj fn = eng::FindFunction(eng::FindClass("BallestRestartBlueprintLibrary"), "BeginKillzoneRestart");
    if (!fn) return;                        // not loaded yet: tried again next frame
    done = true;
    NativeFunction entry = nullptr;
    std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
    if (!entry || !eng::InImage(reinterpret_cast<void*>(entry))) {
        hostlog::Warn("race: BeginKillzoneRestart is not as measured; falls are not counted");
        return;
    }
    gKillzoneOriginal = entry;
    NativeFunction hooked = &HookedKillzone;
    std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
    hostlog::Info("race: counting falls");
}

void CountRespawns(Obj controller) {
    double faults = 0;
    if (!eng::FindProp(eng::ClassOf(controller), "ActualRaceFaults") ||
        !eng::ReadBytes(controller, "ActualRaceFaults", &faults, sizeof faults))
        return;
    // Only during a race: loading a map moves the faults too (measured on Chaos2), without any respawn.
    if (eng::Get(gFaultsController) == controller && faults > gLastFaults && gActive) {
        const int added = static_cast<int>(std::lround(faults - gLastFaults));
        const bool fall = gFallRespawnOpen && game::Seconds() - gFallAt < kFallToRespawn;
        if (fall) gFallRespawnOpen = false; // this respawn was the fall's
        else gRespawns += added;
        hostlog::Info(std::string("race: respawned at a checkpoint (") + (fall ? "fall" : "R") + ", " +
                      std::to_string(gRespawns) + " R and " + std::to_string(gFalls) + " falls this session)");
    }
    gFaultsController = eng::MakeWeak(controller);
    gLastFaults = faults;                   // a new controller, or a new run's reset: a new baseline
}

struct CheckpointActor {
    eng::Weak actor;
    Vec3 at;
};
std::vector<CheckpointActor> gCheckpoints;
int gCheckpointGeneration = -1;
double gNextCheckpointScan = 0;

// Every checkpoint of the map on screen (BP_ActualCheckpointBase_C: strips and discs), in order of position so the
// order is the same every time the map is played. Scanned again every few seconds while on a track (the editor can
// add and remove them) and whenever one has gone.
void ScanCheckpoints(Obj controller) {
    bool gone = false;
    for (const auto& c : gCheckpoints) gone |= !eng::Get(c.actor);
    if (gCheckpointGeneration == game::Generation() && !gone && game::Seconds() < gNextCheckpointScan) return;
    gCheckpointGeneration = game::Generation();
    gNextCheckpointScan = game::Seconds() + 5;
    gCheckpoints.clear();
    static Obj cls = nullptr;
    if (!cls) cls = eng::FindClass("BP_ActualCheckpointBase_C");
    if (!cls) return;
    Obj level = eng::OuterOf(controller);
    eng::ForEachObject([&](Obj o) {
        if (eng::IsA(o, cls) && !eng::IsDefaultObject(o) && eng::OuterOf(o) == level)
            gCheckpoints.push_back({eng::MakeWeak(o), eng::Call(o, "K2_GetActorLocation").ReturnAs<Vec3>()});
        return true;
    });
    std::sort(gCheckpoints.begin(), gCheckpoints.end(), [](const CheckpointActor& a, const CheckpointActor& b) {
        if (a.at.x != b.at.x) return a.at.x < b.at.x;
        if (a.at.y != b.at.y) return a.at.y < b.at.y;
        return a.at.z < b.at.z;
    });
}

}  // namespace

// --- bounces ---------------------------------------------------------------------------------------------------------
// The ball's radius (its Sphere: 50 x 0.95, read from BP_RollingBall's component template).
constexpr double kBallRadius = 47.5;
// Measured with the ball thrown at the ground and walls (host log "race: hit", S2 Sampler, 2026-09-30): the ball's hit
// event comes about 250 times a second while it rests or rolls, at impulses of 200-420; rolling over bumps 2500-2800;
// settling after a teleport 10000; landings 52000 (a gentle throw), 74000-109000, 158000 and 216000 (hard); a fast
// wall hit 185000. Each landing's rebounds follow at about a third of it (21000, 6200). So a bounce is a hit of at least
// 20000, no sooner than 0.12 s after the last, and its strength runs from 0 there to 1 at 250000, by the logarithm (so
// soft, medium and hard landings spread over 0..1: 52000 0.38, 74000 0.52, 109000 0.67, 158000 0.82, 216000 0.94).
constexpr double kBounceFloor = 20000, kBounceFull = 250000, kBounceGap = 0.12;

struct RawHit {
    eng::Weak ball;
    double impulse[3] = {0, 0, 0};
    double when = 0;
};
std::vector<RawHit> gRawHits;                       // seen inside the game's call, resolved in Frame
std::vector<Bounce> gBounces;
int gBounceSerial = 0;
NativeFunction gCollisionOriginal = nullptr;
Obj gCollisionFunction = nullptr;
int gImpulseOffset = -1;

void HookedCollision(Obj context, uint8_t* frame, void* result) {
    Obj node = nullptr, self = nullptr;
    uint8_t* params = nullptr;
    if (frame) {
        std::memcpy(&node, frame + layout::kFFrameFunctionOffset, sizeof node);
        std::memcpy(&self, frame + layout::kFFrameObjectOffset, sizeof self);
        std::memcpy(&params, frame + layout::kFFrameParametersOffset, sizeof params);
    }
    if (node == gCollisionFunction && self == context && params && gRawHits.size() < 32) {
        RawHit hit;
        hit.ball = eng::MakeWeak(context);
        std::memcpy(hit.impulse, params + gImpulseOffset, sizeof hit.impulse);
        hit.when = game::Seconds();
        gRawHits.push_back(hit);
    }
    gCollisionOriginal(context, frame, result);
}

void WatchCollisions() {
    static bool done = false;
    if (done) return;
    Obj ballClass = eng::FindClass("BP_RollingBall_C");
    Obj fn = eng::FindFunction(ballClass, "BndEvt__BP_RollingBall_Sphere_K2Node_ComponentBoundEvent_1_ComponentHitSignature__DelegateSignature");
    Obj other = eng::FindFunction(ballClass, "ReceiveHit");
    if (!fn || !other) return;                      // no track loaded yet: tried again next frame
    done = true;
    NativeFunction entry = nullptr, otherEntry = nullptr;
    std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
    std::memcpy(&otherEntry, other + layout::kUFunctionNativeFunctionOffset, sizeof otherEntry);
    for (const auto& param : eng::ParamsOf(fn))
        if (param.name == "NormalImpulse") gImpulseOffset = param.offset;
    // Both are Blueprint functions of the ball, entering the same interpreter entry in the game exe.
    if (!entry || entry != otherEntry || !eng::InImage(reinterpret_cast<void*>(entry)) || gImpulseOffset < 0) {
        hostlog::Warn("race: the ball's hit event is not as measured; bounces are not seen");
        return;
    }
    gCollisionFunction = fn;
    gCollisionOriginal = entry;
    NativeFunction hooked = &HookedCollision;
    std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
    hostlog::Info("race: watching bounces");
}

void ResolveBounces() {
    for (const RawHit& hit : gRawHits) {
        Obj ball = eng::Get(hit.ball);
        if (!ball) continue;
        struct V {
            double x, y, z;
        };
        const V at = eng::Call(ball, "K2_GetActorLocation").ReturnAs<V>();
        const double size = std::sqrt(hit.impulse[0] * hit.impulse[0] + hit.impulse[1] * hit.impulse[1] + hit.impulse[2] * hit.impulse[2]);
        static double last = -100;
        if (size < kBounceFloor || hit.when - last < kBounceGap) continue;
        last = hit.when;
        Bounce b;
        b.impulse = size;
        b.ground = hit.impulse[2] / size > 0.5;
        b.nx = hit.impulse[0] / size, b.ny = hit.impulse[1] / size, b.nz = hit.impulse[2] / size;
        b.x = at.x - b.nx * kBallRadius, b.y = at.y - b.ny * kBallRadius, b.z = at.z - b.nz * kBallRadius;
        b.strength = std::clamp(std::log(size / kBounceFloor) / std::log(kBounceFull / kBounceFloor), 0.0, 1.0);
        b.serial = ++gBounceSerial;
        if (gBounces.size() >= 64) gBounces.erase(gBounces.begin());
        gBounces.push_back(b);
        char line[160];
        std::snprintf(line, sizeof line, "race: bounce %s impulse %.0f strength %.2f", b.ground ? "ground" : "wall", size, b.strength);
        hostlog::Info(line);
    }
    gRawHits.clear();
}

void Frame() {
    WatchCollisions();
    ResolveBounces();
    WatchKillzone();
    Obj controller = game::PlayerController();
    gOnTrack = controller && eng::FindProp(eng::ClassOf(controller), "bRaceActive");
    gActive = gComplete = false;
    if (gTrackGeneration != game::Generation()) {
        gTrackGeneration = game::Generation();
        gTrack = Track{};
        gTrackDone = false;
        gNextTrackRead = 0;
        gLagRestores.clear();
    }
    if (gPractice && gPracticeGeneration != game::Generation()) {
        gHidden.clear();                                        // gone with the old map: never touched
        gPractice = false;
        gTimerShifted = false;
        hostlog::Info("race: practice off (new map)");
    }
    if (!gOnTrack) {
        gRunId = -1;
        gCheckpoints.clear();
        return;
    }
    ScanCheckpoints(controller);
    eng::ReadBool(controller, "bRaceActive", &gActive);
    CountRespawns(controller);
    eng::ReadBool(controller, "bRaceComplete", &gComplete);
    if (!gTrackDone && game::Seconds() >= gNextTrackRead) {
        gNextTrackRead = game::Seconds() + 1;
        ReadTrack();
    }

    Obj pawn = eng::Call(controller, "K2_GetPawn").ReturnObj();
    int32_t runId = -1;
    if (pawn && eng::FindProp(eng::ClassOf(pawn), "RaceId")) eng::ReadBytes(pawn, "RaceId", &runId, sizeof runId);
    gRunId = runId;
    // The touched run's mark: carried into the run that starts after a touch made before it, cleared when a later run
    // starts (not when this one ends: the upload comes after the end).
    if (gTaintedRun == -1 && runId >= 0) gTaintedRun = runId;
    else if (gTaintedRun >= 0 && runId >= 0 && runId != gTaintedRun) gTaintedRun = -2;
#ifdef HOST_HAS_SANDBOX
    // A sandboxed test copy races live only on the user's own test map, the workshop map "stasis" (project rule, after
    // a teleported finish reached the real leaderboard from a test copy): on any other live track every run is practice
    // (timer stopped, finish off), also before the track has been read. Test runs in the editor (the user's own
    // unreleased maps, the rule's other half) keep their timer and checkpoints.
    static const char kTestMap[] = "custom:3805348161:";
    if (sandbox::On() && gActive && runId >= 0 && !gPractice && gTrack.key.rfind(kTestMap, 0) != 0 &&
        gTrack.key.find("LevelEditor") == std::string::npos) {
        hostlog::Info("sandbox: not the test map (" + (gTrack.key.empty() ? std::string("not read yet") : gTrack.key) +
                      "): this run is practice");
        StartPractice();
    }
#endif
    if (gPractice) {
        if (pawn != eng::Get(gPracticeBall)) ReleasePractice("a new ball");
        else if (runId != gPracticeRun) ReleasePractice("a new run");
        else if (game::Seconds() >= gNextPracticeApply) {
            gNextPracticeApply = game::Seconds() + 0.5;
            ApplyPractice();
        }
    }
    if (!gLagRestores.empty() && game::Seconds() >= gLagRestoreAt) {
        for (const auto& r : gLagRestores)
            if (Obj arm = eng::Get(r.arm)) {
                eng::WriteBool(arm, "bEnableCameraLag", r.lag);
                eng::WriteBool(arm, "bEnableCameraRotationLag", r.rotationLag);
            }
        gLagRestores.clear();
    }

    // A restart is a new run on the same ball. Since the game's 2026-10-06 update a restart (measured in a run; the
    // results screen restarts with the same action) keeps the ball and gives it a new RaceId, but no longer raises its
    // RestartCounterThisSession (measured on stasis: run 93 -> 26, counter stays 1). Only runs that were raced count:
    // before the first Play the ball reads run 0 with no race active (measured), and the first raced run on a new ball
    // (a new map) is not a restart either.
    if (!pawn || !eng::FindProp(eng::ClassOf(pawn), "RaceId")) return;
    if (eng::Get(gBall) != pawn) {
        gBall = eng::MakeWeak(pawn);
        gLastRun = -1;
    }
    if (!gActive || runId < 0 || runId == gLastRun) return;
    if (gLastRun >= 0) {
        // A fall before the first checkpoint goes back to the start (measured on Chaos2, before the update, as a
        // restart); it's a fall, not a restart.
        if (gFallRestartOpen && game::Seconds() - gFallAt < kFallToRespawn) {
            gFallRestartOpen = false;
            hostlog::Info("race: back to the start after a fall (not counted as a restart)");
        } else {
            ++gRestarts;
            hostlog::Info("race restarted from the beginning (" + std::to_string(gRestarts) + " this session)");
        }
    }
    gLastRun = runId;
}

bool OnTrack() { return gOnTrack; }
bool Active() { return gActive; }

void TaintRun(const std::string& why) {
    if (!gOnTrack) return;
    if (gTaintedRun != gRunId || gTaintWhy != why) hostlog::Info("race: this run won't go to a leaderboard (" + why + ")");
    gTaintedRun = gRunId;
    gTaintedGeneration = game::Generation();
    gTaintWhy = why;
}

bool RunTainted(std::string* why) {
    if (gPractice) {
        *why = "practice";
        return true;
    }
    // Touched in this run (or before it started, on this map). It lasts past the finish (measured: the run id reads
    // -1 once a run is complete, which is when the game uploads) until another run starts.
    if (gTaintedGeneration == game::Generation() && gTaintedRun != -2) {
        *why = gTaintWhy;
        return true;
    }
    if (Obj controller = game::PlayerController()) {
        const float dilation = eng::Call(Library("GameplayStatics"), "GetGlobalTimeDilation", controller).ReturnAs<float>(1.0f);
        if (std::fabs(dilation - 1.0f) > 0.0001f) {
            *why = "game time not at normal speed";
            return true;
        }
    }
    return false;
}

int Restarts() { return gRestarts; }
int Respawns() { return gRespawns; }
int Falls() { return gFalls; }
int CheckpointCount() { return gOnTrack ? static_cast<int>(gCheckpoints.size()) : 0; }

bool CheckpointPosition(int index, double* x, double* y, double* z) {
    if (index < 0 || index >= CheckpointCount()) return false;
    *x = gCheckpoints[index].at.x;
    *y = gCheckpoints[index].at.y;
    *z = gCheckpoints[index].at.z;
    return true;
}

int CurrentCheckpoint() {
    for (int i = 0; i < CheckpointCount(); ++i) {
        bool current = false, activated = false;         // strips start out current (measured): only touched ones count
        if (Obj o = eng::Get(gCheckpoints[i].actor))
            if (eng::ReadBool(o, "bCurrent", &current) && current && eng::ReadBool(o, "bActivated", &activated) && activated)
                return i;
    }
    return -1;
}

bool CheckpointTrigger(int index, double* x, double* y, double* z) {
    Obj o = index >= 0 && index < CheckpointCount() ? eng::Get(gCheckpoints[index].actor) : nullptr;
    Obj aura = o ? eng::ReadObj(o, "PreCheckpointAura1") : nullptr;     // what the ball touches (MP_ActualCheckpoint_Strip)
    if (!aura) return false;
    const Vec3 at = eng::Call(aura, "K2_GetComponentLocation").ReturnAs<Vec3>();
    *x = at.x;
    *y = at.y;
    *z = at.z;
    return true;
}

bool MoveBall(double x, double y, double z) {
    TaintRun("the ball was moved by the host");
    Obj ball = PlayedBall();
    if (!ball) return false;
    eng::Params p(eng::FunctionOn(ball, "K2_SetActorLocation"));
    p.Set("NewLocation", Vec3{x, y, z});
    p.Set("bSweep", uint8_t{0});
    p.Set("bTeleport", uint8_t{1});
    return eng::Invoke(ball, p);
}

Obj PlayedBallActor() { return PlayedBall(); }

bool BallPosition(double* x, double* y, double* z) {
    Obj ball = PlayedBall();
    if (!ball) return false;
    const Vec3 at = eng::Call(ball, "K2_GetActorLocation").ReturnAs<Vec3>();
    *x = at.x;
    *y = at.y;
    *z = at.z;
    return true;
}

bool EditorTesting() {
    // The track editor's map with the camera on a ball: a test run (while editing it's on P_LevelEditorPawn_C, and the
    // editor's ball sits unused, measured).
    static bool was = false;
    const bool now = CurrentTrack().key.find("LevelEditor") != std::string::npos && PlayedBall() != nullptr;
    if (now != was) {
        was = now;
        hostlog::Info(std::string("race: editor test run ") + (now ? "started" : "ended"));
    }
    return now;
}
int RunId() { return gRunId; }
bool Complete() { return gComplete; }
const Track& CurrentTrack() { return gTrack; }

bool BounceAfter(int after, Bounce* out) {
    for (const Bounce& b : gBounces)
        if (b.serial > after) {
            *out = b;
            return true;
        }
    return false;
}

int LatestBounce() { return gBounceSerial; }

bool Input(double* x, double* y, bool* jump) {
    Obj ball = Ball();
    struct {
        double x, y;
    } axes{};
    *x = *y = 0;
    *jump = false;
    if (!ball || !eng::ReadBytes(ball, "InputAxes", &axes, sizeof axes)) return false;
    *x = axes.x;
    *y = axes.y;
    eng::ReadBool(ball, "InputJump", jump);
    return true;
}

bool SetPaused(bool paused) {
    if (paused && gActive && !gPractice && !EditorTesting()) {
        hostlog::Warn("race: not pausing a run that counts");
        return false;
    }
    Obj controller = game::PlayerController();
    return controller && eng::Call(Library("GameplayStatics"), "SetGamePaused", controller, static_cast<uint8_t>(paused ? 1 : 0)).ReturnBool();
}

bool Paused() {
    Obj controller = game::PlayerController();
    return controller && eng::Call(Library("GameplayStatics"), "IsGamePaused", controller).ReturnBool();
}

std::string SaveBall() {
    Obj ball = Ball(), controller = game::PlayerController();
    if (!ball || !controller) return "";
    Obj root = eng::Call(ball, "K2_GetRootComponent").ReturnObj();
    const Vec3 at = eng::Call(ball, "K2_GetActorLocation").ReturnAs<Vec3>();
    const Rot turn = eng::Call(ball, "K2_GetActorRotation").ReturnAs<Rot>();
    const Vec3 linear = eng::Call(root, "GetPhysicsLinearVelocity", kNoBone).ReturnAs<Vec3>();
    const Vec3 angular = eng::Call(root, "GetPhysicsAngularVelocityInDegrees", kNoBone).ReturnAs<Vec3>();
    const Rot control = eng::Call(controller, "GetControlRotation").ReturnAs<Rot>();
    std::ostringstream s;
    s.precision(17);
    s << "L " << at.x << ' ' << at.y << ' ' << at.z << '\n'
      << "R " << turn.pitch << ' ' << turn.yaw << ' ' << turn.roll << '\n'
      << "V " << linear.x << ' ' << linear.y << ' ' << linear.z << '\n'
      << "A " << angular.x << ' ' << angular.y << ' ' << angular.z << '\n'
      << "C " << control.pitch << ' ' << control.yaw << ' ' << control.roll << '\n';
    for (Obj arm : Components(ball, "SpringArmComponent")) {
        float length = 0;
        eng::ReadBytes(arm, "TargetArmLength", &length, sizeof length);
        const Rot r = eng::Call(arm, "K2_GetComponentRotation").ReturnAs<Rot>();
        s << "S " << eng::ObjName(arm) << ' ' << length << ' ' << r.pitch << ' ' << r.yaw << ' ' << r.roll << '\n';
    }
    for (Obj camera : Components(ball, "CameraComponent")) {
        Vec3 l;
        Rot r;
        eng::ReadBytes(camera, "RelativeLocation", &l, sizeof l);
        eng::ReadBytes(camera, "RelativeRotation", &r, sizeof r);
        s << "K " << eng::ObjName(camera) << ' ' << l.x << ' ' << l.y << ' ' << l.z << ' ' << r.pitch << ' ' << r.yaw << ' ' << r.roll << '\n';
    }
    return s.str();
}

bool LoadBall(const std::string& state, bool momentum) {
    Obj ball = Ball(), controller = game::PlayerController();
    if (!ball || !controller || state.empty()) return false;
    StartPractice();
    if (!gPractice || !ApplyPractice()) return false;          // never move a ball whose run could still finish
    Vec3 at, linear, angular;
    Rot turn, control;
    struct Arm {
        std::string name;
        float length;
        Rot r;
    };
    struct Camera {
        std::string name;
        Vec3 l;
        Rot r;
    };
    std::vector<Arm> arms;
    std::vector<Camera> cameras;
    std::istringstream in(state);
    for (std::string line; std::getline(in, line);) {
        std::istringstream f(line);
        std::string tag;
        f >> tag;
        if (tag == "L") f >> at.x >> at.y >> at.z;
        else if (tag == "R") f >> turn.pitch >> turn.yaw >> turn.roll;
        else if (tag == "V") f >> linear.x >> linear.y >> linear.z;
        else if (tag == "A") f >> angular.x >> angular.y >> angular.z;
        else if (tag == "C") f >> control.pitch >> control.yaw >> control.roll;
        else if (tag == "S") {
            Arm a;
            f >> a.name >> a.length >> a.r.pitch >> a.r.yaw >> a.r.roll;
            arms.push_back(a);
        } else if (tag == "K") {
            Camera c;
            f >> c.name >> c.l.x >> c.l.y >> c.l.z >> c.r.pitch >> c.r.yaw >> c.r.roll;
            cameras.push_back(c);
        }
    }
    const auto armComponents = Components(ball, "SpringArmComponent");
    gLagRestores.clear();
    for (Obj arm : armComponents) {                             // the camera jumps with the ball instead of trailing it
        bool lag = false, rotationLag = false;
        eng::ReadBool(arm, "bEnableCameraLag", &lag);
        eng::ReadBool(arm, "bEnableCameraRotationLag", &rotationLag);
        gLagRestores.push_back({eng::MakeWeak(arm), lag, rotationLag});
        eng::WriteBool(arm, "bEnableCameraLag", false);
        eng::WriteBool(arm, "bEnableCameraRotationLag", false);
    }
    gLagRestoreAt = game::Seconds() + 0.1;
    {
        eng::Params p(eng::FunctionOn(ball, "K2_SetActorLocationAndRotation"));
        p.Set("NewLocation", at);
        p.Set("NewRotation", turn);
        p.Set("bTeleport", uint8_t{1});
        eng::Invoke(ball, p);
    }
    Obj root = eng::Call(ball, "K2_GetRootComponent").ReturnObj();
    const Vec3 zero{};
    eng::Call(root, "SetPhysicsLinearVelocity", momentum ? linear : zero, uint8_t{0}, kNoBone);
    eng::Call(root, "SetPhysicsAngularVelocityInDegrees", momentum ? angular : zero, uint8_t{0}, kNoBone);
    eng::Call(controller, "SetControlRotation", control);
    for (Obj arm : armComponents)
        for (const auto& a : arms)
            if (a.name == eng::ObjName(arm)) {
                eng::WriteBytes(arm, "TargetArmLength", &a.length, sizeof a.length);
                eng::Params p(eng::FunctionOn(arm, "K2_SetWorldRotation"));
                p.Set("NewRotation", a.r);
                p.Set("bTeleport", uint8_t{1});
                eng::Invoke(arm, p);
            }
    for (Obj camera : Components(ball, "CameraComponent"))
        for (const auto& c : cameras)
            if (c.name == eng::ObjName(camera)) {
                eng::Params p(eng::FunctionOn(camera, "K2_SetRelativeLocationAndRotation"));
                p.Set("NewLocation", c.l);
                p.Set("NewRotation", c.r);
                p.Set("bTeleport", uint8_t{1});
                eng::Invoke(camera, p);
            }
    hostlog::Info(std::string("race: ball loaded") + (momentum ? "" : " (momentum off)"));
    return true;
}

void StartPractice() {
    if (gPractice) return;
    Obj ball = Ball();
    if (!ball || gRunId < 0) return;
    gPractice = true;
    gPracticeRun = gRunId;
    gPracticeBall = eng::MakeWeak(ball);
    gPracticeGeneration = game::Generation();
    if (!ApplyPractice()) {             // fails closed: if the game changed and practice can't be verified, no practice
        ReleasePractice("practice could not be verified (timer or checkpoints not found)");
        hostlog::Warn("race: practice could not be verified; not turned on");
        return;
    }
    gNextPracticeApply = game::Seconds() + 0.5;
    hostlog::Info("race: practice on (timer stopped, checkpoints and finish off, leaderboard and ghosts hidden)");
}

bool Practice() { return gPractice; }

}  // namespace race
