#include "race.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "engine.hpp"
#include "game.hpp"
#include "log.hpp"
#include "widgets.hpp"

using eng::Obj;
namespace w = ui::widgets;

namespace race {
namespace {

bool gOnTrack = false, gActive = false, gComplete = false;
int gRestarts = 0, gRunId = -1;
eng::Weak gBall;                // the ball whose counter was read last
int32_t gLastCounter = 0;

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
struct Rot {
    double pitch = 0, yaw = 0, roll = 0;
};
constexpr uint64_t kNoBone = 0;             // FName None, for the physics functions' BoneName

// The ball's restart counter, or false if this pawn is not a ball (a replay camera, the menu ball, ...).
bool ReadCounter(Obj pawn, int32_t* counter) {
    return pawn && eng::FindProp(eng::ClassOf(pawn), "RestartCounterThisSession") &&
           eng::ReadBytes(pawn, "RestartCounterThisSession", counter, sizeof *counter);
}

Obj Ball() {
    Obj controller = game::PlayerController();
    Obj pawn = controller ? eng::Call(controller, "K2_GetPawn").ReturnObj() : nullptr;
    return pawn && eng::FindProp(eng::ClassOf(pawn), "RaceId") ? pawn : nullptr;
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
    int kind;                       // 0 widget (visibility byte), 1 actor (hidden in game), 2 actor collision
    uint8_t visibility;
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

void ApplyPractice() {
    Obj controller = game::PlayerController();
    if (!controller) return;
    // The race timer (BP_MyPlayerController): its two timelines stopped, the times it keeps at 999.999, the text the
    // timer shows (RaceTimeText, which the HUD reads) set once. The text block itself is left alone: setting it would
    // replace its binding.
    bool stopped = false;
    for (const char* timeline : {"GameTime2", "LapTime"})
        if (Obj t = eng::ReadObj(controller, timeline))
            if (eng::Call(t, "IsPlaying").ReturnBool()) {
                eng::Call(t, "Stop");
                stopped = true;
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
    Obj checkpoint = eng::FindClass("BP_Checkpoint_C"), actorClass = eng::FindClass("Actor"), widgetClass = eng::FindClass("Widget");
    std::vector<Obj> hideClasses;
    for (const char* name : {"WBP_Leaderboard_C", "WBP_PersonalStanding_C", "BallestGhostRenderHost", "BallestGhostLabelCanvas"})
        if (Obj c = eng::FindClass(name)) hideClasses.push_back(c);
    eng::ForEachObject([&](Obj o) {
        Obj cls = eng::ClassOf(o);
        if (eng::IsDefaultObject(o)) return true;
        if (checkpoint && cls == checkpoint) {
            if (!AlreadyHidden(o, 2) && eng::Call(o, "GetActorEnableCollision").ReturnBool()) {
                eng::Call(o, "SetActorEnableCollision", uint8_t{0});
                gHidden.push_back({eng::MakeWeak(o), 2, 0});
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
}

void ReleasePractice(const std::string& why) {
    for (const auto& h : gHidden) {
        Obj o = eng::Get(h.object);
        if (!o) continue;
        if (h.kind == 0) w::SetVisibility(o, h.visibility);
        else if (h.kind == 1) eng::Call(o, "SetActorHiddenInGame", uint8_t{0});
        else eng::Call(o, "SetActorEnableCollision", uint8_t{1});
    }
    gHidden.clear();
    gPractice = false;
    gTimerShifted = false;
    hostlog::Info("race: practice off (" + why + ")");
}

}  // namespace

void Frame() {
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
        return;
    }
    eng::ReadBool(controller, "bRaceActive", &gActive);
    eng::ReadBool(controller, "bRaceComplete", &gComplete);
    if (!gTrackDone && game::Seconds() >= gNextTrackRead) {
        gNextTrackRead = game::Seconds() + 1;
        ReadTrack();
    }

    Obj pawn = eng::Call(controller, "K2_GetPawn").ReturnObj();
    int32_t runId = -1;
    if (pawn && eng::FindProp(eng::ClassOf(pawn), "RaceId")) eng::ReadBytes(pawn, "RaceId", &runId, sizeof runId);
    gRunId = runId;
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

    int32_t counter = 0;
    if (!ReadCounter(pawn, &counter)) return;
    if (eng::Get(gBall) == pawn && counter > gLastCounter) {
        gRestarts += counter - gLastCounter;
        hostlog::Info("race restarted from the beginning (" + std::to_string(gRestarts) + " this session)");
    }
    gBall = eng::MakeWeak(pawn);
    gLastCounter = counter;
}

bool OnTrack() { return gOnTrack; }
bool Active() { return gActive; }
int Restarts() { return gRestarts; }
int RunId() { return gRunId; }
bool Complete() { return gComplete; }
const Track& CurrentTrack() { return gTrack; }

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
    if (!gPractice) return false;                               // never move a ball whose run could still finish
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
    ApplyPractice();
    gNextPracticeApply = game::Seconds() + 0.5;
    hostlog::Info("race: practice on (timer stopped, checkpoints and finish off, leaderboard and ghosts hidden)");
}

bool Practice() { return gPractice; }

}  // namespace race
