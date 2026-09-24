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
int gAppliedFollow = -1;            // follow cam last set to: -1 untouched, 0 game default, 1 follow 3D
eng::Weak gFreeCamera;              // our CameraActor while free mode is on
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

// --- follow settings ---------------------------------------------------------------------------------------------
// Measured values: the game's replay camera uses RotationSource 3 (KeepCurrent) with the mouse steering its
// spring arm; following the ball's travel in 3D is RotationSource 1 with mouse control off.
void ApplyFollowSettings(Obj followCam, bool follow3d) {
    Obj arm = eng::ReadObj(followCam, "RuntimeManagedSpringArm");
    const uint8_t source = follow3d ? 1 : 3;
    eng::WriteBytes(followCam, "RotationSource", &source, 1);
    eng::WriteBool(followCam, "bApplyPlayerControlRotationToManagedSpringArm", !follow3d);
    if (!arm) return;
    eng::WriteBool(arm, "bUsePawnControlRotation", !follow3d);
    if (follow3d) {
        // The arm keeps whatever angle the mouse left it at; zeroed so following points along the travel.
        Params p(eng::FunctionOn(arm, "K2_SetRelativeRotation"));
        p.Set("NewRotation", Rot{0, 0, 0});
        eng::Invoke(arm, p);
    }
}

bool FollowSettingsHeld(Obj followCam) {
    uint8_t source = 0;
    bool mouse = true, armControl = true;
    eng::ReadBytes(followCam, "RotationSource", &source, 1);
    eng::ReadBool(followCam, "bApplyPlayerControlRotationToManagedSpringArm", &mouse);
    if (Obj arm = eng::ReadObj(followCam, "RuntimeManagedSpringArm")) eng::ReadBool(arm, "bUsePawnControlRotation", &armControl);
    return source == 1 && !mouse && !armControl;
}

// The game configures the camera's spring arm again every time following re-engages (each replay restart, which
// at high speed is every few seconds), putting the mouse back in control. Follow 3D is therefore checked every
// frame and re-applied whenever the game has reset it. "Default" only undoes what this host set.
void UpdateFollow(Obj followCam) {
    if (!followCam) return;
    const int want = gMode == CameraFollow3D ? 1 : 0;
    if (want == 1 && !FollowSettingsHeld(followCam)) {
        ApplyFollowSettings(followCam, true);
        hostlog::Info(gAppliedFollow == 1 ? "replay camera: follow 3d re-applied (the game had reset it)" : "replay camera: follow 3d");
    } else if (want == 0 && gAppliedFollow == 1) {
        ApplyFollowSettings(followCam, false);
        hostlog::Info("replay camera: game default");
    }
    gAppliedFollow = want;
}

// --- free camera -------------------------------------------------------------------------------------------------
// A CameraActor of our own becomes the view target; the replay keeps playing behind it.
bool SetViewTarget(Obj target) { return eng::Call(game::PlayerController(), "SetViewTargetWithBlend", target).Invoked(); }

void DestroyFreeCamera() {
    if (Obj cam = eng::Get(gFreeCamera)) eng::Call(cam, "K2_DestroyActor");
    gFreeCamera = {};
    gLooking = false;
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

void UpdateCamera(float dt, Obj viewTarget) {
    Obj replayCam = eng::Get(gReplayCam);
    if (!gActive || !replayCam) {
        if (eng::Get(gFreeCamera)) DestroyFreeCamera();
        return;
    }
    UpdateFollow(FollowCamOf(replayCam));
    if (gMode == CameraFree) {
        if (!eng::Get(gFreeCamera)) SpawnFreeCamera();
        Obj cam = eng::Get(gFreeCamera);
        if (cam && viewTarget != cam) SetViewTarget(cam);
        MoveFreeCamera(dt);
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
        gAppliedFollow = -1;
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
