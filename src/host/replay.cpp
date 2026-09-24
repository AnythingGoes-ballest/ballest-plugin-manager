#include "replay.hpp"

#include <windows.h>

#include <cmath>

#include "engine.hpp"
#include "game.hpp"
#include "input.hpp"
#include "log.hpp"

using eng::Obj;
using eng::Params;

namespace replay {
namespace {

struct Vec3 { double x, y, z; };
struct Rot { double pitch, yaw, roll; };

// --- state ---------------------------------------------------------------------------------------------------------
int gGeneration = -1;
eng::Weak gReplayCam;               // the game's replay camera (BP_FreeCam_C) while a replay is watched
bool gActive = false;
double gLength = -1, gLengthCheckedAt = -100;

int gMode = CameraDefault;
eng::Weak gFreeCamera;              // our CameraActor while free or follow 3D mode is on
int gCameraFor = -1;                // the mode our camera was made for
Vec3 gBall{}, gDir{1, 0, 0};        // follow 3D: the ball last frame and the smoothed travel direction
bool gChaseFresh = true;            // the next follow 3D frame snaps instead of easing
double gYaw = 0, gPitch = 0;
Vec3 gPos{};
bool gLooking = false;
POINT gLookAnchor{};

bool gSimulated = false;
double gSimTime = 0, gSimLength = 30;

// --- game objects --------------------------------------------------------------------------------------------------
Obj ViewTarget() { return eng::Call(game::PlayerController(), "GetViewTarget").ReturnObj(); }
Obj FollowCamOf(Obj replayCam) { return replayCam ? eng::ReadObj(replayCam, "AC_GhostFollowCam") : nullptr; }
bool Following(Obj followCam) { return followCam && eng::Call(followCam, "HasNativeGhostSource").ReturnBool(); }

Obj GhostSubsystem() {
    Obj cls = eng::FindClass("BallestGhostWorldSubsystem");
    Obj controller = game::PlayerController();
    if (!cls || !controller) return nullptr;
    return eng::Call(eng::FindCdo("SubsystemBlueprintLibrary"), "GetWorldSubsystem", controller, cls).ReturnObj();
}

// --- our camera --------------------------------------------------------------------------------------------------
// A CameraActor of our own becomes the view target (free mode and follow 3D); the replay keeps playing behind it.
bool SetViewTarget(Obj target) { return eng::Call(game::PlayerController(), "SetViewTargetWithBlend", target).Invoked(); }

void DestroyFreeCamera() {
    if (Obj cam = eng::Get(gFreeCamera)) eng::Call(cam, "K2_DestroyActor");
    gFreeCamera = {};
    gLooking = false;
    gChaseFresh = true;
}

bool SpawnFreeCamera() {
    Obj controller = game::PlayerController();
    Obj manager = eng::ReadObj(controller, "PlayerCameraManager");
    Obj statics = eng::FindCdo("GameplayStatics");
    Obj cls = eng::FindClass("CameraActor");
    if (!manager || !statics || !cls) return false;
    // Start exactly where the replay camera is looking from.
    gPos = eng::Call(manager, "GetCameraLocation").ReturnAs<Vec3>();
    const Rot r = eng::Call(manager, "GetCameraRotation").ReturnAs<Rot>();
    const float fov = eng::Call(manager, "GetFOVAngle").ReturnAs<float>(90.0f);
    gPitch = r.pitch;
    gYaw = r.yaw;

    struct Transform {                                  // FTransform with doubles (96 bytes)
        double qx, qy, qz, qw;
        double tx, ty, tz, pad0;
        double sx, sy, sz, pad1;
    } t{0, 0, 0, 1, gPos.x, gPos.y, gPos.z, 0, 1, 1, 1, 0};
    Params begin(eng::FunctionOn(statics, "BeginDeferredActorSpawnFromClass"));
    begin.Set("WorldContextObject", controller);
    begin.Set("ActorClass", cls);
    begin.Set("SpawnTransform", t);
    begin.Set("CollisionHandlingOverride", uint8_t{1});    // AlwaysSpawn
    eng::Invoke(statics, begin);
    Obj actor = begin.ReturnObj();
    if (!actor) return false;
    Params finish(eng::FunctionOn(statics, "FinishSpawningActor"));
    finish.Set("Actor", actor);
    finish.Set("SpawnTransform", t);
    eng::Invoke(statics, finish);
    if (Obj component = eng::ReadObj(actor, "CameraComponent")) {
        eng::Call(component, "SetFieldOfView", fov);
        eng::WriteBool(component, "bConstrainAspectRatio", false);
    }
    gFreeCamera = eng::MakeWeak(actor);
    hostlog::Info("free camera at (" + std::to_string(static_cast<int>(gPos.x)) + ", " + std::to_string(static_cast<int>(gPos.y)) +
                  ", " + std::to_string(static_cast<int>(gPos.z)) + ")");
    return true;
}

// Hold the right mouse button to look (the cursor is held in place meanwhile); W/S forward and back, A/D
// sideways, E/Q up and down, Shift for speed.
void MoveFreeCamera(float dt) {
    Obj cam = eng::Get(gFreeCamera);
    if (!cam) return;
    if (input::Down(VK_RBUTTON)) {
        POINT now;
        GetCursorPos(&now);
        if (gLooking) {
            gYaw += (now.x - gLookAnchor.x) * 0.12;
            gPitch = std::fmax(-89.0, std::fmin(89.0, gPitch - (now.y - gLookAnchor.y) * 0.12));
            SetCursorPos(gLookAnchor.x, gLookAnchor.y);
        } else {
            gLooking = true;
            gLookAnchor = now;
        }
    } else {
        gLooking = false;
    }
    constexpr double kRadians = 3.14159265358979 / 180;
    const double p = gPitch * kRadians, y = gYaw * kRadians;
    const Vec3 forward{std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
    const Vec3 right{-std::sin(y), std::cos(y), 0};
    const double f = input::Down('W') - input::Down('S');
    const double r = input::Down('D') - input::Down('A');
    const double u = input::Down('E') - input::Down('Q');
    const double step = (input::Down(VK_SHIFT) ? 5000.0 : 1200.0) * dt;
    gPos.x += (forward.x * f + right.x * r) * step;
    gPos.y += (forward.y * f + right.y * r) * step;
    gPos.z += (forward.z * f + u) * step;
    Params move(eng::FunctionOn(cam, "K2_SetActorLocationAndRotation"));
    move.Set("NewLocation", gPos);
    move.Set("NewRotation", Rot{gPitch, gYaw, 0});
    move.Set("bTeleport", uint8_t{1});
    eng::Invoke(cam, move);
}

// --- follow 3D: a chase camera of our own ----------------------------------------------------------------------
// The game's own follow settings were measured not to give a chase view on this build (2026-09-24: its spring-arm
// reference is empty, and with every setting forced the view's yaw moved 0.3 degrees in 6 s of replay), so follow
// 3D views the replay through the host's camera instead: behind the ball along its direction of travel, turning
// and moving smoothly. The ball is where the game's follow cam puts its rig (GetCurrentSmoothedBaseTransform).
constexpr double kChaseDistance = 450, kChaseHeight = 150, kChaseLookAbove = 40;
constexpr double kTurnRate = 3.0;           // how fast the direction eases to the new one (1/s)
constexpr double kFollowRate = 10.0;        // how fast the camera eases to its place behind the ball (1/s)
constexpr double kJump = 1500;              // a move this long in one frame is a seek or restart: snap, no easing

bool BallPosition(Obj followCam, Vec3* out) {
    const Params p = eng::Call(followCam, "GetCurrentSmoothedBaseTransform");
    size_t size = 0;
    const uint8_t* t = p.Return(&size);
    if (!p.Invoked() || !t || size != 96) return false;
    std::memcpy(out, t + 32, sizeof *out);          // FTransform (doubles): rotation quat, then translation
    return !(out->x == 0 && out->y == 0 && out->z == 0);
}

Vec3 Normalized(Vec3 v) {
    const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return len > 1e-6 ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{1, 0, 0};
}

void MoveChaseCamera(float dt, Obj followCam) {
    Obj cam = eng::Get(gFreeCamera);
    Vec3 ball;
    if (!cam || !followCam || !BallPosition(followCam, &ball) || dt <= 0) return;
    const Vec3 step{ball.x - gBall.x, ball.y - gBall.y, ball.z - gBall.z};
    const double moved = std::sqrt(step.x * step.x + step.y * step.y + step.z * step.z);
    const bool snap = gChaseFresh || moved > kJump;
    // Travel direction; steep climbs and drops are flattened so the camera never ends up straight above or below.
    if (!snap && moved / dt > 50) {
        Vec3 want = Normalized(step);
        want.z = std::fmax(-0.6, std::fmin(0.6, want.z));
        want = Normalized(want);
        const double k = 1 - std::exp(-kTurnRate * dt);
        gDir = Normalized({gDir.x + (want.x - gDir.x) * k, gDir.y + (want.y - gDir.y) * k, gDir.z + (want.z - gDir.z) * k});
    }
    gBall = ball;
    const Vec3 place{ball.x - gDir.x * kChaseDistance, ball.y - gDir.y * kChaseDistance, ball.z - gDir.z * kChaseDistance + kChaseHeight};
    if (snap) {
        gPos = place;
    } else {
        const double k = 1 - std::exp(-kFollowRate * dt);
        gPos = {gPos.x + (place.x - gPos.x) * k, gPos.y + (place.y - gPos.y) * k, gPos.z + (place.z - gPos.z) * k};
    }
    gChaseFresh = false;
    const Vec3 look{ball.x - gPos.x, ball.y - gPos.y, ball.z + kChaseLookAbove - gPos.z};
    constexpr double kDegrees = 180 / 3.14159265358979;
    gYaw = std::atan2(look.y, look.x) * kDegrees;
    gPitch = std::atan2(look.z, std::sqrt(look.x * look.x + look.y * look.y)) * kDegrees;
    Params move(eng::FunctionOn(cam, "K2_SetActorLocationAndRotation"));
    move.Set("NewLocation", gPos);
    move.Set("NewRotation", Rot{gPitch, gYaw, 0});
    move.Set("bTeleport", uint8_t{1});
    eng::Invoke(cam, move);
}

void UpdateCamera(float dt, Obj viewTarget) {
    Obj replayCam = eng::Get(gReplayCam);
    if (!gActive || !replayCam) {
        if (eng::Get(gFreeCamera)) DestroyFreeCamera();
        return;
    }
    if (gMode == CameraFree || gMode == CameraFollow3D) {
        if (gMode != gCameraFor) {                  // switching between free and follow: start the new one afresh
            DestroyFreeCamera();
            gCameraFor = gMode;
            gChaseFresh = true;
        }
        if (!eng::Get(gFreeCamera)) SpawnFreeCamera();
        Obj cam = eng::Get(gFreeCamera);
        if (cam && viewTarget != cam) SetViewTarget(cam);
        if (gMode == CameraFree) MoveFreeCamera(dt);
        else MoveChaseCamera(dt, FollowCamOf(replayCam));
    } else if (eng::Get(gFreeCamera)) {
        SetViewTarget(replayCam);
        DestroyFreeCamera();
    }
}

// The followed ghost's key (AC_GhostFollowCam.GetNativeGhostSourceKey, a 96-byte BallestGhostKey), then its
// record's Duration (BallestGhostWorldSubsystem.GetGhostRecord -> BallestGhostRecordState.Duration).
double ReadRecordedLength() {
    Obj followCam = FollowCamOf(eng::Get(gReplayCam));
    Obj ghosts = GhostSubsystem();
    if (!followCam || !ghosts) return -1;
    Params keyCall = eng::Call(followCam, "GetNativeGhostSourceKey");
    const uint8_t* key = nullptr;
    for (const auto& p : eng::ParamsOf(keyCall.Fn()))
        if (p.size == 96) key = keyCall.Data() + p.offset;
    Params record(eng::FunctionOn(ghosts, "GetGhostRecord"));
    if (!keyCall.Invoked() || !key || !record.Set("Key", key, 96) || !eng::Invoke(ghosts, record)) return -1;
    const eng::Prop duration = eng::FindProp(eng::StructOf(eng::FindProp(record.Fn(), "StateOut")), "Duration");
    const uint8_t* state = record.Get("StateOut");
    if (!state || !duration || duration.size != 8) return -1;
    double seconds = -1;
    std::memcpy(&seconds, state + duration.offset, 8);
    return seconds;
}

}  // namespace

void Frame(float dt) {
    if (gSimulated) {
        gSimTime += dt;
        if (gSimTime >= gSimLength) gSimTime = 0;   // at 1x the game restarts a finished replay itself
    }
    if (game::Generation() != gGeneration) {
        // A different map: its objects are gone, so they are forgotten without being touched.
        gGeneration = game::Generation();
        gReplayCam = gFreeCamera = {};
        gActive = false;
        gCameraFor = -1;
        gLength = -1;
    }
    if (!game::PlayerController()) return;

    // A replay is on while the game's replay camera follows a ghost; our free camera may be the view target.
    Obj viewTarget = ViewTarget();
    Obj replayCam = nullptr;
    if (viewTarget && eng::ObjName(eng::ClassOf(viewTarget)) == "BP_FreeCam_C") replayCam = viewTarget;
    else if (viewTarget && viewTarget == eng::Get(gFreeCamera)) replayCam = eng::Get(gReplayCam);
    const bool active = replayCam && Following(FollowCamOf(replayCam));
    if (active != gActive) hostlog::Info(active ? "replay started" : "replay ended");
    gActive = active;
    gReplayCam = active ? eng::MakeWeak(replayCam) : eng::Weak{};
    if (!active) gLength = -1;
    UpdateCamera(dt, viewTarget);
}

bool Active() { return gActive || gSimulated; }

double Time() {
    if (gSimulated) return gSimTime;
    Obj ghosts = gActive ? GhostSubsystem() : nullptr;
    if (!ghosts) return 0;
    const Params p = eng::Call(ghosts, "GetPlaybackTime");
    size_t size = 0;
    p.Return(&size);
    return size == 4 ? p.ReturnAs<float>() : p.ReturnAs<double>();
}

void Seek(double seconds) {
    if (gSimulated) {
        gSimTime = std::fmax(0.0, std::fmin(gSimLength, seconds));
        return;
    }
    if (Obj ghosts = gActive ? GhostSubsystem() : nullptr) eng::Call(ghosts, "SeekPlayback", seconds);
}

void Restart() {
    if (gSimulated) gSimTime = 0;
    else if (Obj ghosts = gActive ? GhostSubsystem() : nullptr) eng::Call(ghosts, "RestartPlayback");
}

// Unknown lengths are asked for every second (a downloaded replay's record fills in after it loads); known ones
// every ten seconds, which also catches a different replay starting without the view changing.
double Length() {
    if (gSimulated) return gSimLength;
    if (!gActive) return -1;
    const double now = game::Seconds();
    if (now - gLengthCheckedAt < (gLength > 0 ? 10.0 : 1.0)) return gLength;
    gLengthCheckedAt = now;
    const double seconds = ReadRecordedLength();
    if (seconds != gLength) hostlog::Info("replay length " + std::to_string(seconds) + " s from the ghost record");
    gLength = seconds;
    return gLength;
}

int CameraMode() { return gMode; }

void SetCameraMode(int mode) {
    if (mode < CameraDefault || mode > CameraFree || mode == gMode) return;
    gMode = mode;
    hostlog::Info("camera mode " + std::to_string(mode));
}

void Simulate(bool on, double length) {
    gSimulated = on;
    gSimTime = 0;
    gSimLength = length > 0 ? length : 30;
    hostlog::Info(on ? "simulated replay on, length " + std::to_string(gSimLength) : std::string("simulated replay off"));
}

}  // namespace replay
