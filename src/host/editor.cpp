#include "editor.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>

#include "cosmetics.hpp"
#include "game.hpp"
#include "input.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "widgets.hpp"

using eng::Obj;
namespace w = ui::widgets;

namespace editor {
namespace {

struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

eng::Weak gHandler, gDetails;
std::set<int32_t> gKnown;               // AllActors slots seen last frame
std::map<int32_t, int> gFresh;          // new pieces still being watched: frames left to reach the spawn point
eng::Weak gKnownHandler;
std::vector<int> gPlaced;
bool gTabCycling = false, gForceRotateContext = false;
int gRotateMode = kRotateDefault;
ULONGLONG gLastDetailsSearch = 0;
constexpr double kPlacementDistance = 150, kPlacementDrop = -300, kPlacementTolerance = 2;
constexpr int kTabKey = 0x09, kShiftKey = 0x10, kLeftMouse = 0x01;

// Rotating about the centre: the selection and its transforms when the rotation began.
struct Snapshot {
    std::vector<int> ids;
    std::vector<Vec3> locations;
    std::vector<Quat> rotations;
    Vec3 center;
    bool active = false, turned = false;
    Quat lastTurn{0, 0, 0, 1};          // the turn at the last frame of the drag
    int settleFrames = 0;               // after the release: frames left to keep the pieces about the centre
    bool reportedRelease = false;
};
Snapshot gRotation;
int gRotationFocusFrames = 0;           // a rotation box had focus this recently (typed rotations)
int gFocusTarget = -1, gFocusFrames = 0; // Tab: the transform box being given focus, and frames left to insist

// --- engine math: the engine's own functions, so conventions match exactly ----------------------------------------
Obj Math() { return eng::FindCdo("KismetMathLibrary"); }
Quat ToQuat(const Rot& r) { return eng::Call(Math(), "Conv_RotatorToQuaternion", r).ReturnAs<Quat>(); }
Rot ToRot(const Quat& q) { return eng::Call(Math(), "Quat_Rotator", q).ReturnAs<Rot>(); }
Quat Multiply(const Quat& a, const Quat& b) { return eng::Call(Math(), "Multiply_QuatQuat", a, b).ReturnAs<Quat>(); }
Quat Inverse(const Quat& q) { return eng::Call(Math(), "Quat_Inversed", q).ReturnAs<Quat>(); }
Vec3 RotateVector(const Quat& q, const Vec3& v) { return eng::Call(Math(), "Quat_RotateVector", q, v).ReturnAs<Vec3>(); }

// A turn of `degrees` about a unit axis, as FQuat(Axis, AngleRad) builds it.
Quat AxisAngle(double ax, double ay, double az, double degrees) {
    const double half = degrees * 3.14159265358979323846 / 360.0, s = std::sin(half);
    return {ax * s, ay * s, az * s, std::cos(half)};
}

// --- the editor ------------------------------------------------------------------------------------------------------
Obj Pawn() {
    Obj controller = game::PlayerController();
    Obj pawn = controller ? eng::Call(controller, "K2_GetPawn").ReturnObj() : nullptr;
    return pawn && eng::FindProp(eng::ClassOf(pawn), "SKGMLEHandler") ? pawn : nullptr;
}

Obj Handler() { return eng::Get(gHandler); }

Obj Resolve(int id) {
    static eng::Weak actorClass;
    if (!eng::Get(actorClass)) actorClass = eng::MakeWeak(eng::FindClass("Actor"));
    Obj o = id >= 0 && id < eng::NumObjects() ? eng::ObjectAt(id) : nullptr;
    return o && eng::IsA(o, eng::Get(actorClass)) ? o : nullptr;
}

int IdOf(Obj actor) { return eng::MakeWeak(actor).index; }

// A TArray<AActor*> returned by value: { data, num, max }.
std::vector<Obj> ObjArray(const uint8_t* tarray) {
    std::vector<Obj> out;
    if (!tarray) return out;
    Obj* data = nullptr;
    int32_t num = 0;
    std::memcpy(&data, tarray, sizeof data);
    std::memcpy(&num, tarray + 8, sizeof num);
    for (int32_t i = 0; data && i < num && i < 100000; ++i) out.push_back(data[i]);
    return out;
}

std::vector<Obj> AllActors() { return Handler() ? eng::ReadObjArray(Handler(), "AllActors") : std::vector<Obj>{}; }

bool Near(const Vec3& a, const Vec3& b, double tolerance) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

// Palette placements. A tile click (W_MapItem's blueprint, read from the asset) spawns its piece with the level editor
// handler's SpawnActor and then PlacePrePlacedActor moves it (measured: about 600 units ahead of the spawn point), so
// the pieces come from SpawnActor's return value, watched below (SpawnHooked). Without that hook, new entries of
// AllActors near the spawn point are taken, as before (this misses pieces the placing moved).
std::vector<Obj> gSpawned;              // this frame's SpawnActor results, fresh pointers
void SpawnWatched();
bool SpawnHookInstalled();

void FindPlacements() {
    gPlaced.clear();
    SpawnWatched();
    if (SpawnHookInstalled()) {
        for (Obj a : gSpawned)
            if (a && eng::IsLive(a)) {
                gPlaced.push_back(IdOf(a));
                hostlog::Info("editor: placed " + eng::ObjName(a));
            }
        gSpawned.clear();
        return;
    }
    const auto actors = AllActors();
    std::set<int32_t> now;
    for (Obj a : actors) now.insert(IdOf(a));
    if (eng::Get(gKnownHandler) != Handler()) {
        gFresh.clear();
    } else {
        for (int32_t id : now)
            if (!gKnown.count(id)) gFresh[id] = 10;
        Obj pawn = Pawn();
        const Vec3 pawnLocation = eng::Call(pawn, "K2_GetActorLocation").ReturnAs<Vec3>();
        const Vec3 f = ViewForward();
        const Vec3 spawn{pawnLocation.x + kPlacementDistance * f.x, pawnLocation.y + kPlacementDistance * f.y,
                         pawnLocation.z + kPlacementDistance * f.z + kPlacementDrop};
        // With location snapping on, the placed piece lands on the grid (measured: a checkpoint at -3400,-1840,8810
        // with a 10 unit grid), up to half a grid step from the spawn point on each axis.
        double tolerance = kPlacementTolerance;
        struct { float location; bool enabled; } snapping{};
        const eng::Params config = eng::Call(Handler(), "GetSnappingConfiguration");
        size_t configSize = 0;
        if (const uint8_t* r = config.Return(&configSize); r && configSize >= 5) {
            std::memcpy(&snapping.location, r, 4);
            snapping.enabled = r[4] != 0;
            if (snapping.enabled && snapping.location > 0 && snapping.location < 100000) tolerance += snapping.location / 2;
        }
        for (auto it = gFresh.begin(); it != gFresh.end();) {
            Vec3 at;
            if (Location(it->first, &at) && Near(at, spawn, tolerance)) {
                gPlaced.push_back(it->first);
                hostlog::Info("editor: placed " + eng::ObjName(Resolve(it->first)));
                it = gFresh.erase(it);
            } else if (--it->second <= 0 || !now.count(it->first)) {
                it = gFresh.erase(it);
            } else {
                ++it;
            }
        }
    }
    gKnown = now;
    gKnownHandler = gHandler;
}

Obj TransformWidget() { return eng::ReadObj(eng::Get(gDetails), "W_Transform"); }

// The nine EditableTexts of the transform section, location X..Z, rotation X..Z, scale X..Z.
std::vector<Obj> TransformBoxes() {
    std::vector<Obj> out;
    Obj transform = TransformWidget(), textClass = eng::FindClass("EditableText");
    for (const char* property : {"W_Location", "W_Rotation", "W_Scale"}) {
        Obj row = eng::ReadObj(transform, property);
        for (const char* axis : {"VTE_Axis1", "VTE_Axis2", "VTE_Axis3"}) out.push_back(w::FindFirst(eng::ReadObj(row, axis), textClass));
    }
    return out;
}

// Leaving a box commits it, and the editor then refreshes the transform section, which drops keyboard focus; so the
// next box is given focus again for a few frames until it keeps it.
void CycleTransformBoxes() {
    if (!gTabCycling || !eng::Get(gDetails)) return;
    const auto boxes = TransformBoxes();
    if (gFocusFrames > 0) {
        --gFocusFrames;
        Obj target = gFocusTarget >= 0 && gFocusTarget < static_cast<int>(boxes.size()) ? boxes[static_cast<size_t>(gFocusTarget)] : nullptr;
        if (target && !eng::Call(target, "HasKeyboardFocus").ReturnBool()) eng::Call(target, "SetKeyboardFocus");
    }
    if (!input::Pressed(kTabKey)) return;
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (!boxes[i] || !eng::Call(boxes[i], "HasKeyboardFocus").ReturnBool()) continue;
        // The next box the editor lets you type in: it disables boxes that do not apply (scale for pieces that cannot
        // be scaled, for example).
        const size_t count = boxes.size(), step = input::Down(kShiftKey) ? count - 1 : 1;
        size_t next = (i + step) % count;
        while (next != i && !(boxes[next] && eng::Call(boxes[next], "GetIsEnabled").ReturnBool() && eng::Call(boxes[next], "IsVisible").ReturnBool()))
            next = (next + step) % count;
        gFocusTarget = static_cast<int>(next);
        gFocusFrames = 5;
        if (boxes[next]) eng::Call(boxes[next], "SetKeyboardFocus");
        hostlog::Info("editor: Tab from transform box " + std::to_string(i) + " to " + std::to_string(next));
        return;
    }
}

bool RotationBoxFocused() {
    const auto boxes = TransformBoxes();
    for (size_t i = 3; i < 6 && i < boxes.size(); ++i)
        if (boxes[i] && eng::Call(boxes[i], "HasKeyboardFocus").ReturnBool()) return true;
    return false;
}

// Mirrored: which way each piece turns. The pieces are split by a plane through the selection's centre that holds the
// turn's axis and lies across the direction the pieces are spread in (the main axis of their offsets, seen along the
// turn's axis). The side of the piece the editor turns with the gizmo (the last selected) turns with the drag, the
// other side turns the opposite way, so the two sides turn as mirror images; pieces on the plane keep their rotation.
// The turn is read from that piece each frame, so it always gets the drag's own turn.
std::vector<int> MirrorSides(const Quat& turn) {
    const size_t n = gRotation.ids.size();
    std::vector<int> sides(n, 1);
    const double axisLength = std::sqrt(turn.x * turn.x + turn.y * turn.y + turn.z * turn.z);
    if (axisLength < 1e-12 || n == 0) return sides;
    const Vec3 a{turn.x / axisLength, turn.y / axisLength, turn.z / axisLength};
    std::vector<Vec3> flat(n);
    double longest = 0;
    Vec3 spread{0, 0, 0};
    for (size_t i = 0; i < n; ++i) {
        const Vec3& l = gRotation.locations[i];
        const Vec3 d{l.x - gRotation.center.x, l.y - gRotation.center.y, l.z - gRotation.center.z};
        const double along = d.x * a.x + d.y * a.y + d.z * a.z;
        flat[i] = {d.x - along * a.x, d.y - along * a.y, d.z - along * a.z};
        const double length = std::sqrt(flat[i].x * flat[i].x + flat[i].y * flat[i].y + flat[i].z * flat[i].z);
        if (length > longest) longest = length, spread = flat[i];
    }
    if (longest < 1e-6) return sides;
    for (int iteration = 0; iteration < 32; ++iteration) {     // main axis: power iteration on sum(p p^T)
        Vec3 next{0, 0, 0};
        for (const Vec3& p : flat) {
            const double dot = p.x * spread.x + p.y * spread.y + p.z * spread.z;
            next.x += p.x * dot, next.y += p.y * dot, next.z += p.z * dot;
        }
        const double length = std::sqrt(next.x * next.x + next.y * next.y + next.z * next.z);
        if (length < 1e-12) break;
        spread = {next.x / length, next.y / length, next.z / length};
    }
    const size_t pivot = n - 1;
    auto side = [&](size_t i) {
        const double s = flat[i].x * spread.x + flat[i].y * spread.y + flat[i].z * spread.z;
        return std::fabs(s) < longest * 1e-3 ? 0 : (s > 0 ? 1 : -1);
    };
    const int pivotSide = side(pivot) == 0 ? 1 : side(pivot);
    for (size_t i = 0; i < n; ++i) sides[i] = i == pivot ? 1 : side(i) * pivotSide;
    return sides;
}

// The snapshot's pieces placed for `turn` by the rotate mode: about the selection's centre, or turned in place as
// mirror images. Returns how far the first piece was from that place.
double PlaceAboutCenter(const Quat& turn) {
    double moved = 0;
    const bool mirrored = gRotateMode == kRotateMirrored;
    const std::vector<int> sides = mirrored ? MirrorSides(turn) : std::vector<int>{};
    const Quat back = mirrored ? Inverse(turn) : turn;
    for (size_t i = 0; i < gRotation.ids.size(); ++i) {
        const Vec3& from = gRotation.locations[i];
        if (mirrored) {
            SetLocation(gRotation.ids[i], from);
            const Quat own = sides[i] > 0 ? turn : sides[i] < 0 ? back : Quat{0, 0, 0, 1};
            SetRotation(gRotation.ids[i], ToRot(Multiply(own, gRotation.rotations[i])));
            continue;
        }
        const Vec3 offset = RotateVector(turn, {from.x - gRotation.center.x, from.y - gRotation.center.y, from.z - gRotation.center.z});
        const Vec3 place{gRotation.center.x + offset.x, gRotation.center.y + offset.y, gRotation.center.z + offset.z};
        if (i == 0) {
            Vec3 now;
            if (Location(gRotation.ids[i], &now)) moved = std::sqrt((now.x - place.x) * (now.x - place.x) + (now.y - place.y) * (now.y - place.y) + (now.z - place.z) * (now.z - place.z));
        }
        SetLocation(gRotation.ids[i], place);
        SetRotation(gRotation.ids[i], ToRot(Multiply(turn, gRotation.rotations[i])));
    }
    return moved;
}

// While the player rotates two or more pieces, the editor turns them about the last selected one. From the moment a
// rotation starts, each frame's turn (read from the first piece's rotation) is re-applied about the centre instead.
// When the mouse is let go the editor applies its own result (the turn about its pivot) once more, so for a few
// frames after the release the pieces are put back about the centre with the drag's last turn, then selected again
// so the editor's pivot follows.
void RotateAboutCenter() {
    if (gRotation.settleFrames > 0) {
        const double moved = PlaceAboutCenter(gRotation.lastTurn);
        if (moved > 0.01 && !gRotation.reportedRelease) {
            hostlog::Info("editor: after the release the editor moved the pieces " + std::to_string(moved) + " units; put back about the centre");
            gRotation.reportedRelease = true;
        }
        if (--gRotation.settleFrames == 0) {
            Select(gRotation.ids);
            gRotation.active = false;
        }
        return;
    }
    if (gRotateMode == kRotateDefault) {
        gRotation.active = false;
        return;
    }
    if (gRotationFocusFrames > 0) --gRotationFocusFrames;
    if (RotationBoxFocused()) gRotationFocusFrames = 3;
    const bool rotating = input::Down(kLeftMouse) || gRotationFocusFrames > 0 || gForceRotateContext;
    const auto ids = Selection();
    if (!rotating || ids.size() < 2) {
        if (gRotation.active && gRotation.turned) {
            gRotation.settleFrames = 10;
            gRotation.reportedRelease = false;
            PlaceAboutCenter(gRotation.lastTurn);
            return;
        }
        gRotation.active = false;
        return;
    }
    if (!gRotation.active || gRotation.ids != ids) {
        gRotation = Snapshot{};
        gRotation.ids = ids;
        for (int id : ids) {
            Vec3 l;
            Rot r;
            Location(id, &l);
            Rotation(id, &r);
            gRotation.locations.push_back(l);
            gRotation.rotations.push_back(ToQuat(r));
            gRotation.center.x += l.x / static_cast<double>(ids.size());
            gRotation.center.y += l.y / static_cast<double>(ids.size());
            gRotation.center.z += l.z / static_cast<double>(ids.size());
        }
        gRotation.active = true;
        return;
    }
    Rot now;
    const size_t reference = ids.size() - 1;
    if (!Rotation(ids[reference], &now)) return;
    const Quat turn = Multiply(ToQuat(now), Inverse(gRotation.rotations[reference]));
    if (std::fabs(std::fabs(turn.w) - 1.0) < 1e-9) return;           // not turned (a move, or nothing)
    gRotation.turned = true;
    gRotation.lastTurn = turn;
    PlaceAboutCenter(turn);
}

}  // namespace

namespace {
void WatchClicks();          // clicks, toolbar choices and key rows: further down
void WatchSaving();
void KeepToolbarChoices();
void KeepHotkeys();
void KeepRestores();
}  // namespace

void Frame() {
    Obj pawn = Pawn();
    gHandler = eng::MakeWeak(pawn ? eng::ReadObj(pawn, "SKGMLEHandler") : nullptr);
    if (!Handler()) {
        gPlaced.clear();
        gKnown.clear();
        gRotation.active = false;
        return;
    }
    if (!eng::Get(gDetails) && GetTickCount64() - gLastDetailsSearch > 1000) {
        gLastDetailsSearch = GetTickCount64();
        Obj cls = eng::FindClass("W_Details_C"), found = nullptr;
        eng::ForEachObject([&](Obj o) {
            if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o)) found = o;
            return found == nullptr;
        });
        gDetails = eng::MakeWeak(found);
    }
    FindPlacements();
    CycleTransformBoxes();
    RotateAboutCenter();
    WatchClicks();
    WatchSaving();
    KeepToolbarChoices();
    KeepHotkeys();
    KeepRestores();
}

bool Open() { return Handler() != nullptr; }

std::vector<int> Selection() {
    std::vector<int> ids;
    if (!Handler()) return ids;
    const eng::Params p = eng::Call(Handler(), "GetSelection");
    for (Obj a : ObjArray(p.Return())) ids.push_back(IdOf(a));
    return ids;
}

std::vector<int> Placed() { return gPlaced; }

bool Location(int id, Vec3* out) {
    Obj a = Resolve(id);
    if (a) *out = eng::Call(a, "K2_GetActorLocation").ReturnAs<Vec3>();
    return a != nullptr;
}

bool Rotation(int id, Rot* out) {
    Obj a = Resolve(id);
    if (a) *out = eng::Call(a, "K2_GetActorRotation").ReturnAs<Rot>();
    return a != nullptr;
}

bool SetLocation(int id, const Vec3& location) {
    Obj a = Resolve(id);
    return a && eng::Call(a, "K2_SetActorLocation", location, uint8_t{0}).Invoked();
}

bool SetRotation(int id, const Rot& rotation) {
    Obj a = Resolve(id);
    return a && eng::Call(a, "K2_SetActorRotation", rotation, uint8_t{0}).Invoked();
}

Vec3 ViewForward() {
    Obj controller = game::PlayerController();
    const Rot r = controller ? eng::Call(controller, "GetControlRotation").ReturnAs<Rot>() : Rot{};
    return eng::Call(Math(), "GetForwardVector", r).ReturnAs<Vec3>();
}

void Select(const std::vector<int>& ids) {
    if (!Handler()) return;
    eng::Call(Handler(), "Deselect");
    bool append = false;
    for (int id : ids)
        if (Obj a = Resolve(id)) {
            eng::Call(Handler(), "GrabWithNotifications", a, static_cast<uint8_t>(append), uint8_t{0});
            append = true;
        }
}

std::vector<int> DuplicateSelection() {
    std::vector<int> copies;
    if (!Handler() || Selection().empty()) return copies;
    std::set<int32_t> before;
    for (Obj a : AllActors()) before.insert(IdOf(a));
    eng::Call(Handler(), "CopySelection");
    const eng::FString none{L"", 1, 1};
    eng::Call(Handler(), "Paste", uint8_t{1}, uint8_t{1}, none);         // at the copied location, with properties
    for (Obj a : AllActors())                                           // the paste leaves the copies selected
        if (!before.count(IdOf(a))) copies.push_back(IdOf(a));
    for (int id : copies) {                                             // not placements
        gKnown.insert(id);
        gFresh.erase(id);
    }
    return copies;
}

void RotatePieces(const std::vector<int>& ids, const Vec3& center, double degreesX, double degreesY, double degreesZ) {
    const Quat turn = Multiply(AxisAngle(0, 0, 1, degreesZ), Multiply(AxisAngle(0, 1, 0, degreesY), AxisAngle(1, 0, 0, degreesX)));
    for (int id : ids) {
        Vec3 l;
        Rot r;
        if (!Location(id, &l) || !Rotation(id, &r)) continue;
        const Vec3 offset = RotateVector(turn, {l.x - center.x, l.y - center.y, l.z - center.z});
        SetLocation(id, {center.x + offset.x, center.y + offset.y, center.z + offset.z});
        SetRotation(id, ToRot(Multiply(turn, ToQuat(r))));
    }
}

void SetTabCycling(bool on) { gTabCycling = on; }
void SetRotateAroundCenter(bool on) { gRotateMode = on ? kRotateAroundCenter : kRotateDefault; }
void SetRotateMode(int mode) {
    if (mode >= kRotateDefault && mode <= kRotateMirrored) gRotateMode = mode;
}
int RotateMode() { return gRotateMode; }
void ForceRotateContext(bool on) { gForceRotateContext = on; }

eng::Obj DetailsContainer() {
    Obj switcher = eng::ReadObj(eng::Get(gDetails), "WS_Details");
    const auto slots = switcher ? eng::ReadObjArray(switcher, "Slots") : std::vector<Obj>{};
    return slots.size() > 1 ? eng::ReadObj(slots[1], "Content") : nullptr;
}

// --- clicks ----------------------------------------------------------------------------------------------------------
// The editor pawn's blueprint (P_LevelEditorPawn, read from the cooked asset) handles a click on the world in four
// input events, one per modifier, and each press ends in the level editor handler's FindAndGrab, which traces under the
// cursor: plain click FindAndGrab(false) (replaces the selection), Shift and Ctrl FindAndGrab(true) (adds; neither
// removes), Alt HandleAltPrimaryPress (duplicate-drag). These only run for clicks the UI did not take. Each is watched
// by swapping its UFunction's entry (all four are blueprint functions sharing the interpreter's entry): the piece
// under the cursor and whether it was selected are read before the game's own handling runs.
namespace {

using NativeFunction = void (*)(Obj context, uint8_t* frame, void* result);

struct ClickEvent {
    const char* function;
    int modifiers;
};
const ClickEvent kClickEvents[] = {
    {"InpActEvt_LeftMouseButton_K2Node_InputKeyEvent_15", 0},
    {"InpActEvt_Shift_LeftMouseButton_K2Node_InputKeyEvent_13", kClickShift},
    {"InpActEvt_Ctrl_LeftMouseButton_K2Node_InputKeyEvent_7", kClickCtrl},
    {"InpActEvt_Alt_LeftMouseButton_K2Node_InputKeyEvent_10", kClickAlt},
};
constexpr int kClickEventCount = static_cast<int>(sizeof kClickEvents / sizeof kClickEvents[0]);
Obj gClickFunctions[kClickEventCount] = {};
NativeFunction gClickOriginal = nullptr;
bool gClickHookFailed = false;
bool gWatchingClicks = true;            // off (test switch): every click is left entirely to the game
std::deque<Click> gClicks;
constexpr size_t kMaxClicks = 16;
constexpr int kControlKey = 0x11, kAltKey = 0x12;

// The editor's save path (W_MapEditor's save button -> W_SaveLoad.ManuallySaveNoCallback -> if the file exists, an
// "Overwrite Save?" popup answered through UserAccepted_Event / UserDeclined_Event; read from the blueprints), logged
// with whether a map file was written, since the game writes no log of its own. Only the steps the game enters through
// ProcessEvent (delegates) pass through a function's entry; a blueprint calling a blueprint function does not.
struct SaveStep {
    const char* className;
    const char* function;
    const char* label;
    bool checkFile;
};
const SaveStep kSaveSteps[] = {
    {"W_MapEditor_C", "BndEvt__W_MapEditor_WBP_SaveButton_K2Node_ComponentBoundEvent_11_ButtonPressed__DelegateSignature", "save button pressed", true},
    {"W_SaveLoad_C", "UserAccepted_Event", "overwrite accepted", true},
    {"W_SaveLoad_C", "UserDeclined_Event", "overwrite declined", false},
};
constexpr int kSaveStepCount = static_cast<int>(sizeof kSaveSteps / sizeof kSaveSteps[0]);
Obj gSaveFunctions[kSaveStepCount] = {};

// The map file written in the last few seconds, or "".
std::string JustWrittenMap() {
    const std::wstring dir = hostlog::DataDir() + L"\\..\\UserSavedMaps\\";
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW((dir + L"*.balledit").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return "";
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG nowTicks = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    std::string newest;
    do {
        const ULONGLONG written = (static_cast<ULONGLONG>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime;
        if (nowTicks - written < 3ull * 10000000) newest = eng::Narrow(found.cFileName, static_cast<int>(wcslen(found.cFileName)));
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return newest;
}

const char* ClickName(int modifiers) {
    return (modifiers & kClickAlt) ? "alt" : (modifiers & kClickShift) ? "shift" : (modifiers & kClickCtrl) ? "ctrl" : "plain";
}

void ClickHooked(Obj context, uint8_t* frame, void* result) {
    Obj node = nullptr;
    if (frame) std::memcpy(&node, frame + layout::kFFrameFunctionOffset, sizeof node);
    for (int i = 0; i < kSaveStepCount; ++i)
        if (node && node == gSaveFunctions[i]) {
            hostlog::Info(std::string("editor: ") + kSaveSteps[i].label);
            gClickOriginal(context, frame, result);
            if (kSaveSteps[i].checkFile) {
                const std::string written = JustWrittenMap();
                hostlog::Info(written.empty() ? "editor: no map file written (yet)" : "editor: wrote " + written);
            }
            return;
        }
    int event = -1;
    for (int i = 0; i < kClickEventCount; ++i)
        if (node && node == gClickFunctions[i]) event = i;
    if (event < 0 || !Handler() || !gWatchingClicks) return gClickOriginal(context, frame, result);
    Click c;
    c.modifiers = kClickEvents[event].modifiers | (input::Down(kShiftKey) ? kClickShift : 0) |
                  (input::Down(kControlKey) ? kClickCtrl : 0) | (input::Down(kAltKey) ? kClickAlt : 0);
    const bool gizmoBefore = eng::Call(Handler(), "IsHoveringGizmo").ReturnBool();
    const auto before = Selection();
    gClickOriginal(context, frame, result);
    const auto after = Selection();
    const bool gizmoAfter = eng::Call(Handler(), "IsHoveringGizmo").ReturnBool();
    if (gizmoBefore || gizmoAfter) c.modifiers |= kClickOnGizmo;
    // The piece clicked: the one the game's handling added to the selection. A plain click leaves the clicked piece as
    // the whole selection. Otherwise (Alt, or Shift/Ctrl on a piece that was selected already) the game's own pick
    // (FindAndGrab, which traces under the cursor) is asked for it and the selection the game left is put back. That
    // is only done off the gizmo: any change of selection while the button is down re-binds the gizmo and ends the
    // drag the press began (reported after the 2026-09-25 update: pieces could not be moved).
    for (int id : after)
        if (std::find(before.begin(), before.end(), id) == before.end()) c.piece = id;
    const bool plain = !(c.modifiers & (kClickShift | kClickCtrl | kClickAlt));
    if (c.piece < 0 && plain && after.size() == 1) c.piece = after[0];
    else if (c.piece < 0 && !plain && !(c.modifiers & kClickOnGizmo)) {
        eng::Call(Handler(), "FindAndGrab", uint8_t{0});
        const auto picked = Selection();
        if (picked.size() == 1) c.piece = picked[0];
        Select(after);
    }
    c.wasSelected = c.piece >= 0 && std::find(before.begin(), before.end(), c.piece) != before.end();
    hostlog::Info(std::string("editor: ") + ClickName(c.modifiers) + " click on " + (c.piece >= 0 ? eng::ObjName(Resolve(c.piece)) : std::string("nothing")) +
                  (c.wasSelected ? " (selected)" : "") + ((c.modifiers & kClickOnGizmo) ? " on the gizmo" : "") + "; the game left " +
                  std::to_string(after.size()) + " selected (gizmo hovered before " + (gizmoBefore ? "yes" : "no") + ", after " +
                  (gizmoAfter ? "yes" : "no") + ")");
    if (gClicks.size() >= kMaxClicks) gClicks.pop_front();
    gClicks.push_back(c);
}

// The handler's SpawnActor is native (the SKG plugin's C++), with an entry of its own; its return value is the piece.
NativeFunction gSpawnOriginal = nullptr;
bool gSpawnHookFailed = false;

void SpawnHooked(Obj context, uint8_t* frame, void* result) {
    gSpawnOriginal(context, frame, result);
    Obj spawned = nullptr;
    if (result) std::memcpy(&spawned, result, sizeof spawned);
    if (spawned && gSpawned.size() < 64) gSpawned.push_back(spawned);
}

bool SpawnHookInstalled() { return gSpawnOriginal != nullptr; }

void SpawnWatched() {
    if (gSpawnOriginal || gSpawnHookFailed || !Handler()) return;
    Obj fn = eng::FindFunction(eng::ClassOf(Handler()), "SpawnActor");
    NativeFunction entry = nullptr;
    if (fn) std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
    const NativeFunction hooked = &SpawnHooked;
    if (!fn || !entry || !eng::InImage(reinterpret_cast<void*>(entry)) || entry == gClickOriginal) {
        gSpawnHookFailed = true;
        hostlog::Warn("editor: the handler's SpawnActor is not as measured; placements are found by position");
        return;
    }
    if (entry == hooked) return;
    gSpawnOriginal = entry;
    std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
    hostlog::Info("editor: watching palette placements");
}

// Installed on the pawn class's functions whenever the editor's pawn class is (re)loaded.
void WatchClicks() {
    if (gClickHookFailed) return;
    Obj pawn = Pawn();
    if (!pawn) return;
    Obj cls = eng::ClassOf(pawn);
    Obj fns[kClickEventCount];
    for (int i = 0; i < kClickEventCount; ++i) {
        fns[i] = eng::FindFunction(cls, kClickEvents[i].function);
        if (!fns[i]) {
            gClickHookFailed = true;
            hostlog::Warn(std::string("editor: the pawn has no ") + kClickEvents[i].function + "; clicks are not watched");
            return;
        }
    }
    if (std::equal(fns, fns + kClickEventCount, gClickFunctions)) return;
    NativeFunction entries[kClickEventCount];
    for (int i = 0; i < kClickEventCount; ++i) std::memcpy(&entries[i], fns[i] + layout::kUFunctionNativeFunctionOffset, sizeof entries[i]);
    const NativeFunction hooked = &ClickHooked;
    for (int i = 0; i < kClickEventCount; ++i) {
        if (entries[i] == hooked) continue;             // already ours (the same class seen again)
        if (!eng::InImage(reinterpret_cast<void*>(entries[i])) || (gClickOriginal && entries[i] != gClickOriginal) ||
            (!gClickOriginal && i > 0 && entries[i] != entries[0])) {
            gClickHookFailed = true;
            hostlog::Warn("editor: the pawn's click events are not blueprint functions as measured; clicks are not watched");
            return;
        }
        gClickOriginal = entries[i];
    }
    for (int i = 0; i < kClickEventCount; ++i) {
        std::memcpy(fns[i] + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
        gClickFunctions[i] = fns[i];
    }
    hostlog::Info("editor: watching clicks on pieces");
}

void WatchSaving() {
    if (!gClickOriginal) return;
    const NativeFunction hooked = &ClickHooked;
    for (int i = 0; i < kSaveStepCount; ++i) {
        Obj fn = eng::FindFunction(eng::FindClass(kSaveSteps[i].className), kSaveSteps[i].function);
        if (!fn || fn == gSaveFunctions[i]) continue;
        NativeFunction entry = nullptr;
        std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
        if (entry == hooked) {
            gSaveFunctions[i] = fn;
            continue;
        }
        if (entry != gClickOriginal) continue;          // not the blueprint entry the click events use: left alone
        std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
        gSaveFunctions[i] = fn;
    }
}

// --- editor UI: panels without an insert -------------------------------------------------------------------------------
struct SlotLayout {
    uint8_t size[8]{}, padding[16]{};
    uint8_t alignX = 0, alignY = 0;
};

SlotLayout LayoutOf(Obj widget) {
    SlotLayout l;
    Obj slot = eng::ReadObj(widget, "Slot");
    eng::ReadBytes(slot, "Size", l.size, sizeof l.size);
    eng::ReadBytes(slot, "Padding", l.padding, sizeof l.padding);
    eng::ReadBytes(slot, "HorizontalAlignment", &l.alignX, 1);
    eng::ReadBytes(slot, "VerticalAlignment", &l.alignY, 1);
    return l;
}

void ApplyLayout(Obj slot, const SlotLayout& l) {
    if (!slot) return;
    eng::Params size(eng::FunctionOn(slot, "SetSize")), padding(eng::FunctionOn(slot, "SetPadding"));
    size.SetArg(0, l.size, sizeof l.size);
    padding.SetArg(0, l.padding, sizeof l.padding);
    eng::Invoke(slot, size);
    eng::Invoke(slot, padding);
    eng::Call(slot, "SetHorizontalAlignment", l.alignX);
    eng::Call(slot, "SetVerticalAlignment", l.alignY);
}

// A toolbar button taken out and added back is rebuilt, and its blueprint's Construct refills its dropdown and selects
// the first value, which the toolbar applies (the snapping sizes, the camera speed), and draws it unhighlighted. So
// the value it showed is selected again (the toolbar applies that too) and its highlight redrawn, for a few frames in
// case the rebuild comes a frame late.
struct Restore {
    eng::Weak widget;
    std::string option;
    bool active = false;
    int frames = 0;
};
std::vector<Restore> gRestores;

Obj ComboOf(Obj toolbarButton) { return eng::ReadObj(eng::ReadObj(toolbarButton, "WBP_TransformDropdown"), "ComboBox"); }

std::string SelectedOption(Obj combo) {
    const eng::Params p = eng::Call(combo, "GetSelectedOption");
    const uint8_t* r = p.Return();
    return r ? eng::ReadFString(r) : "";
}

void Remember(Obj widget) {
    static Obj buttonClass = nullptr;
    if (!buttonClass) buttonClass = eng::FindClass("WBP_TransformSettings_C");
    if (!widget || eng::ClassOf(widget) != buttonClass) return;
    Restore r;
    r.widget = eng::MakeWeak(widget);
    if (Obj combo = ComboOf(widget)) r.option = SelectedOption(combo);
    eng::ReadBool(widget, "Active", &r.active);
    r.frames = 30;
    gRestores.push_back(r);
}

void KeepRestores() {
    for (auto it = gRestores.begin(); it != gRestores.end();) {
        Obj w = eng::Get(it->widget);
        if (!w || --it->frames < 0) {
            it = gRestores.erase(it);
            continue;
        }
        Obj combo = ComboOf(w);
        if (combo && !it->option.empty() && SelectedOption(combo) != it->option) {
            const std::wstring wide = eng::Widen(it->option);
            const eng::FString option{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
            eng::Call(combo, "SetSelectedOption", option);
            hostlog::Info("editor: " + eng::ObjName(w) + " set back to " + it->option);
        }
        eng::Call(w, "SetIsActive", static_cast<uint8_t>(it->active ? 1 : 0));
        ++it;
    }
}

// Puts `child` into `panel` (a horizontal or vertical box) right after `after`, laid out like `after`. Boxes have no
// insert at runtime, so the children after it are taken out and added back with their own slot settings (and toolbar
// buttons with what they showed, above).
bool InsertAfter(Obj panel, Obj after, Obj child) {
    const int32_t n = eng::Call(panel, "GetChildrenCount").ReturnAs<int32_t>(-1);
    const int32_t at = eng::Call(panel, "GetChildIndex", after).ReturnAs<int32_t>(-1);
    if (n < 0 || at < 0) return false;
    std::vector<std::pair<Obj, SlotLayout>> moved;
    for (int32_t i = at + 1; i < n; ++i) {
        Obj c = eng::Call(panel, "GetChildAt", i).ReturnObj();
        if (c) moved.push_back({c, LayoutOf(c)});
    }
    for (auto& [c, layout] : moved) {
        Remember(c);
        eng::Call(panel, "RemoveChild", c);
    }
    ApplyLayout(ui::widgets::AddChild(panel, child), LayoutOf(after));
    for (auto& [c, layout] : moved) ApplyLayout(ui::widgets::AddChild(panel, c), layout);
    return true;
}

Obj Live(const char* className) {
    Obj cls = eng::FindClass(className), found = nullptr;
    if (!cls) return nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::PathOf(o).find("/Engine/Transient") == 0) found = o;
        return found == nullptr;
    });
    return found;
}

Obj NewUserWidget(const char* className) {
    Obj controller = game::PlayerController(), cls = eng::FindClass(className);
    if (!controller || !cls) return nullptr;
    return eng::Call(eng::FindCdo("WidgetBlueprintLibrary"), "Create", controller, cls, controller).ReturnObj();
}

// A texture: a game asset ("/Game/...") or a PNG file (kept for the session, one per file).
Obj IconTexture(const std::string& icon) {
    static std::map<std::string, eng::Weak> loaded;
    if (Obj t = eng::Get(loaded[icon])) return t;
    Obj t = nullptr;
    if (!icon.empty() && icon[0] == '/') {
        t = cosmetics::LoadAsset(eng::Widen(icon));
    } else if (!icon.empty()) {
        const std::wstring wide = eng::Widen(icon);
        const eng::FString file{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
        t = eng::Call(eng::FindCdo("KismetRenderingLibrary"), "ImportFileAsTexture2D", game::PlayerController(), file).ReturnObj();
        if (t) cosmetics::KeepAlive(t);
    }
    loaded[icon] = eng::MakeWeak(t);
    return t;
}

void SetComboOptions(Obj combo, const std::vector<std::string>& options, int selected) {
    eng::Call(combo, "ClearOptions");
    for (const auto& option : options) {
        const std::wstring wide = eng::Widen(option);
        const eng::FString s{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
        eng::Call(combo, "AddOption", s);
    }
    eng::Call(combo, "SetSelectedIndex", static_cast<int32_t>(selected));
}

// --- toolbar choices ---------------------------------------------------------------------------------------------------
// A button of the editor's own toolbar kind (WBP_TransformSettings, read from its blueprint): with EPlacement 4 it is
// an icon with a dropdown on its right, as the snapping buttons are; its ComboBoxString is filled with the choices.
// Placed after the world/local toggle (the globe).
struct ToolbarChoice {
    int owner = -1;
    std::string icon;
    std::vector<std::string> options;
    int selected = 0;
    eng::Weak widget, combo;
    int shownSelected = -1;
    bool removed = false;
};
std::vector<ToolbarChoice> gChoices;
constexpr uint8_t kPlacementWithDropdown = 4;           // EEditorQuickButtonType: the snapping buttons' kind

void KeepToolbarChoices() {
    Obj snapping = nullptr;
    for (auto& c : gChoices) {
        if (c.removed) continue;
        Obj widget = eng::Get(c.widget);
        if (!widget) {
            if (!snapping) snapping = Live("W_Snapping_C");
            Obj globe = eng::ReadObj(snapping, "WBP_RelativeOrWorldToggle");
            Obj row = eng::ReadObj(eng::ReadObj(globe, "Slot"), "Parent");
            if (!row) continue;
            widget = NewUserWidget("WBP_TransformSettings_C");
            if (!widget) continue;
            if (Obj icon = IconTexture(c.icon)) eng::WriteBytes(widget, "DisplayIcon", &icon, sizeof icon);
            eng::WriteBytes(widget, "EPlacement", &kPlacementWithDropdown, 1);
            if (!InsertAfter(row, globe, widget)) continue;
            c.widget = eng::MakeWeak(widget);
            c.combo = {};
            c.shownSelected = -1;
            hostlog::Info("editor: toolbar choice added after the world/local toggle");
        }
        Obj combo = eng::Get(c.combo);
        if (!combo) {
            combo = eng::ReadObj(eng::ReadObj(widget, "WBP_TransformDropdown"), "ComboBox");
            if (!combo) continue;
            SetComboOptions(combo, c.options, c.selected);
            eng::Call(combo, "InvalidateLayoutAndVolatility");     // its size was worked out while it was empty
            eng::Call(widget, "InvalidateLayoutAndVolatility");
            c.combo = eng::MakeWeak(combo);
            c.shownSelected = c.selected;
            continue;
        }
        const int now = eng::Call(combo, "GetSelectedIndex").ReturnAs<int32_t>(-1);
        if (c.shownSelected != c.selected) {            // the plugin changed it
            eng::Call(combo, "SetSelectedIndex", static_cast<int32_t>(c.selected));
            c.shownSelected = c.selected;
        } else if (now >= 0 && now != c.selected) {     // the player did
            c.selected = c.shownSelected = now;
        }
    }
}

// --- hotkey legend -----------------------------------------------------------------------------------------------------
// The editor's key list (WBP_KeyKey) is a column of WBP_KeyRow widgets; a row shows PrimaryIcon and DesiredText, and a
// second icon only when bHasModifier (from its blueprint). Plugin rows go after the game's single-key rows (the last
// is T, test track), in the order they were added.
struct Hotkey {
    int owner = -1;
    std::string icon, label, secondIcon;        // with a second icon: "icon + second icon"
    eng::Weak row;
    bool removed = false;
};
std::vector<Hotkey> gHotkeys;

// The key list's column of rows sits in an overlay with nothing limiting its height (WBP_KeyKey's tree), so extra rows
// would make the panel taller, up into the toolbar. Before the first plugin row goes in, the column is moved into a
// ScrollBox inside a SizeBox that is at most as tall as the column was, in the column's place in the overlay.
bool KeepListSize(Obj column) {
    Obj holder = eng::ReadObj(eng::ReadObj(column, "Slot"), "Parent");
    static Obj scrollClass = nullptr;
    if (!scrollClass) scrollClass = eng::FindClass("ScrollBox");
    if (!holder) return false;
    if (eng::ClassOf(holder) == scrollClass) return true;          // done already
    struct { double x, y; } size{};
    size = eng::Call(column, "GetDesiredSize").ReturnAs<decltype(size)>();
    if (size.y < 50) return false;                                  // not laid out yet (hidden): later
    Obj oldSlot = eng::ReadObj(column, "Slot");
    uint8_t padding[16]{}, alignX = 0, alignY = 0;
    eng::ReadBytes(oldSlot, "Padding", padding, sizeof padding);
    eng::ReadBytes(oldSlot, "HorizontalAlignment", &alignX, 1);
    eng::ReadBytes(oldSlot, "VerticalAlignment", &alignY, 1);
    Obj tree = eng::OuterOf(column);
    Obj box = ui::widgets::Spawn("SizeBox", tree), scroll = ui::widgets::Spawn("ScrollBox", tree);
    if (!box || !scroll) return false;
    eng::Call(box, "SetMaxDesiredHeight", static_cast<float>(size.y));
    eng::Call(holder, "RemoveChild", column);
    ui::widgets::AddChild(box, scroll);
    ui::widgets::AddChild(scroll, column);
    Obj slot = ui::widgets::AddChild(holder, box);
    eng::Params setPadding(eng::FunctionOn(slot, "SetPadding"));
    setPadding.SetArg(0, padding, sizeof padding);
    eng::Invoke(slot, setPadding);
    eng::Call(slot, "SetHorizontalAlignment", alignX);
    eng::Call(slot, "SetVerticalAlignment", alignY);
    hostlog::Info("editor: the key list scrolls past " + std::to_string(static_cast<int>(size.y)) + " px, its height before");
    return true;
}

void KeepHotkeys() {
    Obj legend = nullptr, anchor = nullptr, column = nullptr;
    for (auto& h : gHotkeys) {
        if (h.removed || eng::Get(h.row)) {
            if (!h.removed) anchor = eng::Get(h.row);   // the next one goes after this
            continue;
        }
        if (!legend) {
            legend = Live("WBP_KeyKey_C");
            if (!legend) return;
            Obj keyClass = eng::FindClass("WBP_KeyRow_C");
            eng::ForEachObject([&](Obj o) {             // the game's T row
                if (eng::ClassOf(o) != keyClass || eng::OuterOf(eng::OuterOf(o)) != legend) return true;
                Obj icon = eng::ReadObj(o, "PrimaryIcon");
                if (icon && eng::ObjName(icon) == "keyboard_t" && !anchor) anchor = o;
                return true;
            });
        }
        if (!anchor) return;
        if (!column) column = eng::ReadObj(eng::ReadObj(anchor, "Slot"), "Parent");
        if (!column || !KeepListSize(column)) return;
        Obj row = NewUserWidget("WBP_KeyRow_C");
        if (!row || !column) return;
        if (Obj icon = IconTexture(h.icon)) eng::WriteBytes(row, "PrimaryIcon", &icon, sizeof icon);
        eng::WriteBool(row, "bHasModifier", !h.secondIcon.empty());
        if (Obj second = h.secondIcon.empty() ? nullptr : IconTexture(h.secondIcon)) eng::WriteBytes(row, "OptionalIcon", &second, sizeof second);
        // The text's one reference moves into the row's DesiredText (read by the row's PreConstruct).
        const eng::Params text = eng::MakeText(h.label);
        size_t size = 0;
        const uint8_t* ftext = text.Return(&size);
        if (ftext && size == 16) eng::WriteBytes(row, "DesiredText", ftext, size);
        if (!InsertAfter(column, anchor, row)) return;
        if (Obj actionText = eng::ReadObj(row, "ActionText")) ui::widgets::SetText(actionText, h.label);
        h.row = eng::MakeWeak(row);
        anchor = row;
        hostlog::Info("editor: hotkey row '" + h.label + "' added to the key list");
    }
}

}  // namespace

bool NextClick(Click* out) {
    if (gClicks.empty()) return false;
    *out = gClicks.front();
    gClicks.pop_front();
    return true;
}

std::vector<int> Pieces() {
    std::vector<int> ids;
    for (Obj a : AllActors()) ids.push_back(IdOf(a));
    return ids;
}

std::string PieceClass(int id) {
    Obj a = Resolve(id);
    return a ? eng::ObjName(eng::ClassOf(a)) : "";
}

// The name of the map open in the editor: the game instance's CurrentUGCLevelName (the name in the editor's header).
std::string MapName() {
    Obj controller = game::PlayerController();
    Obj instance = controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameInstance", controller).ReturnObj() : nullptr;
    const eng::Prop p = instance ? eng::FindProp(eng::ClassOf(instance), "CurrentUGCLevelName") : eng::Prop{};
    return p && eng::KindOf(p) == "StrProperty" ? eng::ReadFString(instance + p.offset) : "";
}

// A text box of the game has keyboard focus (the map name, a transform box, a search): keys are being typed.
bool Typing() {
    bool typing = false;
    for (const char* className : {"EditableText", "EditableTextBox", "MultiLineEditableText", "MultiLineEditableTextBox"}) {
        Obj cls = eng::FindClass(className);
        if (!cls) continue;
        eng::ForEachObject([&](Obj o) {
            if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::Call(o, "HasKeyboardFocus").ReturnBool()) typing = true;
            return !typing;
        });
        if (typing) break;
    }
    return typing;
}

int AddToolbarChoice(int owner, const std::string& icon, const std::vector<std::string>& options, int selected) {
    ToolbarChoice c;
    c.owner = owner;
    c.icon = icon;
    c.options = options;
    c.selected = selected >= 0 && selected < static_cast<int>(options.size()) ? selected : 0;
    gChoices.push_back(c);
    return static_cast<int>(gChoices.size() - 1);
}

int ToolbarChoiceSelected(int choice) {
    return choice >= 0 && choice < static_cast<int>(gChoices.size()) && !gChoices[choice].removed ? gChoices[choice].selected : -1;
}

void SetToolbarChoiceSelected(int choice, int selected) {
    if (choice < 0 || choice >= static_cast<int>(gChoices.size()) || gChoices[choice].removed) return;
    ToolbarChoice& c = gChoices[choice];
    if (selected >= 0 && selected < static_cast<int>(c.options.size())) c.selected = selected;
}

void AddHotkey(int owner, const std::string& icon, const std::string& label, const std::string& secondIcon) {
    Hotkey h;
    h.owner = owner;
    h.icon = icon;
    h.secondIcon = secondIcon;
    h.label = label;
    gHotkeys.push_back(h);
}

void RemoveOwner(int owner) {
    for (auto& c : gChoices)
        if (c.owner == owner && !c.removed) {
            if (Obj w = eng::Get(c.widget)) eng::Call(w, "RemoveFromParent");
            c.removed = true;
        }
    for (auto& h : gHotkeys)
        if (h.owner == owner && !h.removed) {
            if (Obj r = eng::Get(h.row)) eng::Call(r, "RemoveFromParent");
            h.removed = true;
        }
}

std::string PiecesStatus() {
    std::string s;
    for (Obj a : AllActors()) {
        Vec3 l = eng::Call(a, "K2_GetActorLocation").ReturnAs<Vec3>();
        Obj parent = eng::Call(a, "GetAttachParentActor").ReturnObj();
        char buf[200];
        std::snprintf(buf, sizeof buf, " [%d %s at %.1f,%.1f,%.1f parent %s]", IdOf(a), eng::ObjName(a).c_str(), l.x, l.y, l.z,
                      parent ? (std::to_string(IdOf(parent)) + " " + eng::ObjName(parent)).c_str() : "none");
        s += buf;
    }
    return "pieces:" + s;
}

bool CallHandler(const std::string& function) { return Handler() && eng::Call(Handler(), function.c_str()).Invoked(); }

std::string Status() {
    std::string s = std::string("editor=") + (Open() ? "open" : "closed");
    if (!Open()) return s;
    const auto ids = Selection();
    s += " pieces=" + std::to_string(AllActors().size()) + " selected=" + std::to_string(ids.size());
    {
        const Vec3 cam = eng::Call(Pawn(), "K2_GetActorLocation").ReturnAs<Vec3>(), f = ViewForward();
        char where[160];
        std::snprintf(where, sizeof where, " camera=%.1f,%.1f,%.1f forward=%.3f,%.3f,%.3f spawn=%.1f,%.1f,%.1f", cam.x, cam.y, cam.z, f.x, f.y, f.z,
                      cam.x + kPlacementDistance * f.x, cam.y + kPlacementDistance * f.y, cam.z + kPlacementDistance * f.z + kPlacementDrop);
        s += where;
        eng::Params p(eng::FunctionOn(Handler(), "GetSelectionTransform"));   // the editor's own pivot (the gizmo)
        eng::Invoke(Handler(), p);
        const uint8_t* t = p.Get("OutTransform");
        const int at = eng::NestedOffset(eng::FindClass("SKGMLEHandlerComponent") ? eng::StructOf(eng::FindProp(p.Fn(), "OutTransform")) : nullptr, {"Translation"});
        if (t && at >= 0 && p.ReturnBool()) {
            Vec3 pivot;
            std::memcpy(&pivot, t + at, sizeof pivot);
            char buf[96];
            std::snprintf(buf, sizeof buf, " pivot=%.1f,%.1f,%.1f", pivot.x, pivot.y, pivot.z);
            s += buf;
        }
    }
    const auto boxes = TransformBoxes();
    for (size_t i = 0; i < boxes.size(); ++i)
        if (boxes[i] && eng::Call(boxes[i], "HasKeyboardFocus").ReturnBool()) s += " focus=box" + std::to_string(i);
    for (int id : ids) {
        Vec3 l;
        Rot r;
        Location(id, &l);
        Rotation(id, &r);
        char buf[160];
        std::snprintf(buf, sizeof buf, " [%d %s at %.3f,%.3f,%.3f rot %.3f,%.3f,%.3f]", id, eng::ObjName(Resolve(id)).c_str(), l.x, l.y, l.z,
                      r.pitch, r.yaw, r.roll);
        s += buf;
    }
    return s;
}

void SetWatchingClicks(bool on) { gWatchingClicks = on; }

}  // namespace editor
