#include "editor.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "game.hpp"
#include "input.hpp"
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
bool gTabCycling = false, gRotateAroundCenter = false, gForceRotateContext = false;
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

// New entries of AllActors; the ones at the palette's spawn point are placements. A placement makes two actors
// (measured: the piece and a twin on the same spot, the twin appearing or moving there a frame later), so each new
// piece is watched for a few frames.
void FindPlacements() {
    gPlaced.clear();
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
        for (auto it = gFresh.begin(); it != gFresh.end();) {
            Vec3 at;
            if (Location(it->first, &at) && Near(at, spawn, kPlacementTolerance)) {
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

// While the player rotates two or more pieces, the editor turns them about the last selected one. From the moment a
// rotation starts, each frame's turn (read from the first piece's rotation) is re-applied about the centre instead.
void RotateAboutCenter() {
    if (!gRotateAroundCenter) {
        gRotation.active = false;
        return;
    }
    if (gRotationFocusFrames > 0) --gRotationFocusFrames;
    if (RotationBoxFocused()) gRotationFocusFrames = 3;
    const bool rotating = input::Down(kLeftMouse) || gRotationFocusFrames > 0 || gForceRotateContext;
    const auto ids = Selection();
    if (!rotating || ids.size() < 2) {
        if (gRotation.active && gRotation.turned) Select(gRotation.ids);    // the editor's pivot catches up
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
    if (!Rotation(ids[0], &now)) return;
    const Quat turn = Multiply(ToQuat(now), Inverse(gRotation.rotations[0]));
    if (std::fabs(std::fabs(turn.w) - 1.0) < 1e-9) return;           // not turned (a move, or nothing)
    gRotation.turned = true;
    for (size_t i = 0; i < ids.size(); ++i) {
        const Vec3& from = gRotation.locations[i];
        const Vec3 offset = RotateVector(turn, {from.x - gRotation.center.x, from.y - gRotation.center.y, from.z - gRotation.center.z});
        SetLocation(ids[i], {gRotation.center.x + offset.x, gRotation.center.y + offset.y, gRotation.center.z + offset.z});
        SetRotation(ids[i], ToRot(Multiply(turn, gRotation.rotations[i])));
    }
}

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
void SetRotateAroundCenter(bool on) { gRotateAroundCenter = on; }
void ForceRotateContext(bool on) { gForceRotateContext = on; }

eng::Obj DetailsContainer() {
    Obj switcher = eng::ReadObj(eng::Get(gDetails), "WS_Details");
    const auto slots = switcher ? eng::ReadObjArray(switcher, "Slots") : std::vector<Obj>{};
    return slots.size() > 1 ? eng::ReadObj(slots[1], "Content") : nullptr;
}

std::string Status() {
    std::string s = std::string("editor=") + (Open() ? "open" : "closed");
    if (!Open()) return s;
    const auto ids = Selection();
    s += " pieces=" + std::to_string(AllActors().size()) + " selected=" + std::to_string(ids.size());
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

}  // namespace editor
