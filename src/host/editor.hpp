// The track editor (Map_LevelEditorMain), measured on this build:
//   * the editor pawn (P_LevelEditorPawn_C) holds the SKG level editor handler in its SKGMLEHandler property. The
//     handler's AllActors array lists every piece; GetSelection returns the selection; CopySelection + Paste(at the
//     copied location) duplicates; Deselect + GrabWithNotifications selects. The handler keeps its own copy of the
//     selection's transform (the gizmo pivot): after pieces are moved from outside, the selection is selected again
//     so that copy matches.
//   * a palette tile spawns its piece at pawn location + 150 x view direction + (0, 0, -300) (measured at two view
//     angles, exact); that is how placements are told apart from pastes and duplicates.
//   * the details panel is a W_Details_C whose WS_Details switcher shows its second child (a VerticalBox: the
//     transform header, W_Transform, the paint section) while something is selected. W_Transform holds W_Location,
//     W_Rotation and W_Scale, each with three ValidatedTextEntry boxes VTE_Axis1..3 around an EditableText.
// Pieces are identified by their object-array slot; an id stops resolving when the piece is deleted.
// Game thread only.
#pragma once
#include <vector>

#include "engine.hpp"

namespace editor {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
struct Rot {                            // FRotator's order in memory
    double pitch = 0, yaw = 0, roll = 0;
};

void Frame();                           // after game::Frame
bool Open();                            // the track editor is running

std::vector<int> Selection();
std::vector<int> Placed();              // pieces placed from the palette this frame
bool Location(int id, Vec3* out);
bool Rotation(int id, Rot* out);
bool SetLocation(int id, const Vec3& location);
bool SetRotation(int id, const Rot& rotation);
Vec3 ViewForward();

// Selects exactly these pieces (the editor's own selection, with its pivot and highlighting).
void Select(const std::vector<int>& ids);
// Copies the selection and pastes it in place; the copies are left selected (by the paste, measured) and returned.
std::vector<int> DuplicateSelection();
// Turns pieces about `center` by the given degrees around the world X, Y and Z axes (X first, then Y, then Z).
void RotatePieces(const std::vector<int>& ids, const Vec3& center, double degreesX, double degreesY, double degreesZ);

// Behaviour the host runs while a plugin has it switched on.
void SetTabCycling(bool on);            // Tab / Shift+Tab move between the nine transform boxes
void SetRotateAroundCenter(bool on);    // rotating two or more pieces turns them about their centre
void ForceRotateContext(bool on);       // test hook: treat rotations as user rotations without a mouse drag

eng::Obj DetailsContainer();            // where plugin sections are added in the details panel, or null
std::string Status();                   // for the test channel

}  // namespace editor
