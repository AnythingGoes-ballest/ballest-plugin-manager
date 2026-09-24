#include "replay.hpp"

#include <windows.h>

#include <algorithm>
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
double gDistance = 0;               // the camera's distance from the ball; 0: the game's own
bool gSeeThrough = false;           // pieces between the camera and the ball turn to glass
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
constexpr double kChaseDistance = 450, kChaseLookAbove = 40;
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
    const double distance = gDistance > 0 ? gDistance : kChaseDistance, height = distance / 3;
    const Vec3 place{ball.x - gDir.x * distance, ball.y - gDir.y * distance, ball.z - gDir.z * distance + height};
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

// --- distance and see-through -----------------------------------------------------------------------------------
// The replay camera (BP_FreeCam) hangs off its SpringArm, measured: TargetArmLength 375, bDoCollisionTest on (which
// pulls the camera in when something is between it and the ball), ProbeSize 22. A chosen distance is written to the
// arm every frame (the game may set it again); see-through turns the collision test off so the camera stays out,
// and turns whatever is between the camera and the ball into glass (the game's own M_Glass material), putting each
// piece's own materials back a moment after it no longer blocks the view.
constexpr double kClearAfter = 0.3;         // seconds a glassed piece must be out of the way before it is restored
constexpr double kShortOfBall = 80;         // the traces stop this far before the ball, so they never hit it
constexpr int kMaxBlockers = 4;

struct Glassed {
    eng::Weak component;
    std::vector<eng::Weak> materials;       // its own, in slot order
    double lastBlocking = 0;
};
std::vector<Glassed> gGlassed;
eng::Weak gArm;                             // the arm whose length or collision this host changed
float gArmLength = -1;                      // its length before that
eng::Weak gGlass;
bool gGlassMissingLogged = false;

void RestoreArm() {
    Obj arm = eng::Get(gArm);
    if (arm) {
        if (gArmLength > 0) eng::WriteBytes(arm, "TargetArmLength", &gArmLength, sizeof gArmLength);
        eng::WriteBool(arm, "bDoCollisionTest", true);
    }
    gArm = {};
    gArmLength = -1;
}

void UpdateArm(Obj replayCam) {
    Obj arm = eng::ReadObj(replayCam, "SpringArm");
    if (!arm) return;
    if (arm != eng::Get(gArm)) {
        RestoreArm();
        gArm = eng::MakeWeak(arm);
        eng::ReadBytes(arm, "TargetArmLength", &gArmLength, sizeof gArmLength);
    }
    const float length = gDistance > 0 ? static_cast<float>(gDistance) : gArmLength;
    if (length > 0) eng::WriteBytes(arm, "TargetArmLength", &length, sizeof length);
    eng::WriteBool(arm, "bDoCollisionTest", !gSeeThrough);
}

void Unglass(Glassed& g) {
    Obj component = eng::Get(g.component);
    for (size_t i = 0; component && i < g.materials.size(); ++i)
        if (Obj material = eng::Get(g.materials[i])) eng::Call(component, "SetMaterial", static_cast<int32_t>(i), material);
}

void UnglassAll() {
    for (auto& g : gGlassed) Unglass(g);
    gGlassed.clear();
}

Obj Glass() {
    if (Obj glass = eng::Get(gGlass)) return glass;
    static double lastLook = -100;                  // looking scans every object: at most every two seconds
    if (game::Seconds() - lastLook < 2) return nullptr;
    lastLook = game::Seconds();
    Obj found = eng::FindObjectByName("M_Glass");
    gGlass = eng::MakeWeak(found);
    if (!found && !gGlassMissingLogged) {
        hostlog::Warn("see-through: the glass material is not loaded in this map; pieces in the way stay as they are");
        gGlassMissingLogged = true;
    }
    return found;
}

// The component a line trace hit (FHitResult.Component, a weak pointer: object index then serial number).
Obj HitComponent(const Params& trace) {
    static eng::Prop component;
    if (!component) component = eng::FindProp(eng::StructOf(eng::FindProp(trace.Fn(), "OutHit")), "Component");
    const uint8_t* hit = trace.Get("OutHit");
    if (!hit || !component) return nullptr;
    int32_t index = -1;
    std::memcpy(&index, hit + component.offset, sizeof index);
    return index >= 0 && index < eng::NumObjects() ? eng::ObjectAt(index) : nullptr;
}

void UpdateSeeThrough(Obj followCam) {
    Obj controller = game::PlayerController();
    Obj manager = eng::ReadObj(controller, "PlayerCameraManager");
    Vec3 ball;
    Obj glass = Glass();
    if (!manager || !followCam || !glass || !BallPosition(followCam, &ball)) return;
    const Vec3 from = eng::Call(manager, "GetCameraLocation").ReturnAs<Vec3>();
    const Vec3 dir = Normalized({ball.x - from.x, ball.y - from.y, ball.z - from.z});
    const Vec3 to{ball.x - dir.x * kShortOfBall, ball.y - dir.y * kShortOfBall, ball.z - dir.z * kShortOfBall};
    const double now = game::Seconds();
    static Obj meshClass = nullptr;
    if (!meshClass) meshClass = eng::FindClass("MeshComponent");

    // One trace per blocker: each one found is ignored by the next, so everything in the way is found in turn.
    std::vector<Obj> ignore;
    Obj library = eng::FindCdo("KismetSystemLibrary");
    for (int n = 0; n < kMaxBlockers; ++n) {
        struct {
            Obj* data;
            int32_t num, max;
        } actors{ignore.data(), static_cast<int32_t>(ignore.size()), static_cast<int32_t>(ignore.size())};
        Params trace(eng::FunctionOn(library, "LineTraceSingle"));
        trace.Set("WorldContextObject", controller);
        trace.Set("Start", from);
        trace.Set("End", to);
        trace.Set("ActorsToIgnore", actors);
        trace.Set("bIgnoreSelf", uint8_t{1});
        if (!eng::Invoke(library, trace) || !trace.ReturnBool()) break;
        Obj component = HitComponent(trace);
        Obj actor = component ? eng::Call(component, "GetOwner").ReturnObj() : nullptr;
        if (!component || !actor) break;
        ignore.push_back(actor);
        if (!eng::IsA(component, meshClass)) continue;
        auto it = std::find_if(gGlassed.begin(), gGlassed.end(), [&](const Glassed& g) { return eng::Get(g.component) == component; });
        if (it == gGlassed.end()) {
            Glassed g;
            g.component = eng::MakeWeak(component);
            const int32_t slots = eng::Call(component, "GetNumMaterials").ReturnAs<int32_t>();
            for (int32_t i = 0; i < slots && i < 16; ++i) {
                g.materials.push_back(eng::MakeWeak(eng::Call(component, "GetMaterial", i).ReturnObj()));
                eng::Call(component, "SetMaterial", i, glass);
            }
            gGlassed.push_back(std::move(g));
            it = gGlassed.end() - 1;
        }
        it->lastBlocking = now;
    }
    for (auto it = gGlassed.begin(); it != gGlassed.end();) {
        if (now - it->lastBlocking > kClearAfter || !eng::Get(it->component)) {
            Unglass(*it);
            it = gGlassed.erase(it);
        } else {
            ++it;
        }
    }
}

void UpdateCamera(float dt, Obj viewTarget) {
    Obj replayCam = eng::Get(gReplayCam);
    if (!gActive || !replayCam) {
        if (eng::Get(gFreeCamera)) DestroyFreeCamera();
        if (!gGlassed.empty()) UnglassAll();
        if (eng::Get(gArm)) RestoreArm();
        return;
    }
    UpdateArm(replayCam);
    if (gSeeThrough && gMode != CameraFree) UpdateSeeThrough(FollowCamOf(replayCam));
    else if (!gGlassed.empty()) UnglassAll();
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
        gReplayCam = gFreeCamera = gArm = gGlass = {};
        gGlassed.clear();
        gArmLength = -1;
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

double CameraDistance() { return gDistance; }

void SetCameraDistance(double units) {
    const double d = units > 0 ? std::floor(units + 0.5) : 0;
    if (d == gDistance) return;
    gDistance = d;
    hostlog::Info("replay camera distance " + std::to_string(static_cast<int>(d)) + (d > 0 ? "" : " (the game's own)"));
}

bool SeeThrough() { return gSeeThrough; }

void SetSeeThrough(bool on) {
    if (on == gSeeThrough) return;
    gSeeThrough = on;
    hostlog::Info(on ? "replay see-through on" : "replay see-through off");
}

void Simulate(bool on, double length) {
    gSimulated = on;
    gSimTime = 0;
    gSimLength = length > 0 ? length : 30;
    hostlog::Info(on ? "simulated replay on, length " + std::to_string(gSimLength) : std::string("simulated replay off"));
}

}  // namespace replay
