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
//   * the piece budget (the header's bar) is the handler's AllocatedBudget; opening a saved map leaves it at 0, so the
//     host has the handler count it again (InitializeBudget) whenever it differs from the pieces' BudgetCost.
//   * nothing here changes the selection while the left button is down: that ends the drag the press began.
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
// Where a piece's middle (its bounds' centre) is on screen, in the units of window offsets and the mouse position
// (the viewport's widget units, top left 0,0). False if it isn't in front of the camera.
bool ScreenPosition(int id, double* x, double* y);
// The editor's selection outline on a piece, as the game draws it for a selected one (read from BP_BaseItem: its Main
// mesh renders custom depth with stencil 1), without selecting it. Selecting or deselecting it resets it.
bool SetOutline(int id, bool on);

// Selects exactly these pieces (the editor's own selection, with its pivot and highlighting).
void Select(const std::vector<int>& ids);
// Copies the selection and pastes it in place; the copies are left selected (by the paste, measured) and returned.
std::vector<int> DuplicateSelection();
// Turns pieces about `center` by the given degrees around the world X, Y and Z axes (X first, then Y, then Z).
void RotatePieces(const std::vector<int>& ids, const Vec3& center, double degreesX, double degreesY, double degreesZ);

// Behaviour the host runs while a plugin has it switched on.
void SetWatchingClicks(bool on);   // test: off leaves every click on the world entirely to the game (no Click records)
void SetTabCycling(bool on);            // Tab / Shift+Tab move between the nine transform boxes
void SetRotateAroundCenter(bool on);    // rotating two or more pieces turns them about their centre
// How rotating two or more pieces works: as the editor does (about the last selected piece), about the selection's
// centre, or mirrored (each piece turns in place, the two sides of the centre opposite ways, as mirror images).
constexpr int kRotateDefault = 0, kRotateAroundCenter = 1, kRotateMirrored = 2;
void SetRotateMode(int mode);
int RotateMode();

// Clicks on the world (not on the UI), from the editor pawn's own click events, oldest first.
constexpr int kClickShift = 1, kClickCtrl = 2, kClickAlt = 4, kClickOnGizmo = 8;
struct Click {
    int piece = -1;                     // the piece under the cursor, or -1
    int modifiers = 0;                  // kClick* bits: the keys held, and whether the click was on the gizmo
    bool wasSelected = false;           // the piece was selected before the game handled the click
};
bool NextClick(Click* out);

std::vector<int> Pieces();              // every piece of the map
std::string PieceClass(int id);         // its class name ("BP_Ramp_Curve1_C"), or ""
std::string MapName();                  // the map open in the editor, or "" before it has a name
bool Typing();                          // a text box of the game has keyboard focus

// A dropdown button in the editor's toolbar, after the world/local toggle, of the toolbar's own kind. `icon` is a
// game texture path ("/Game/...") or a PNG file. Returns its number.
int AddToolbarChoice(int owner, const std::string& icon, const std::vector<std::string>& options, int selected);
int ToolbarChoiceSelected(int choice);
void SetToolbarChoiceSelected(int choice, int selected);
// A row of the editor's key list: a key image (game texture path or PNG file) and what the key does.
// A row of the editor's key list: a key image (game texture path or PNG file) and what the key does; with a second
// image the row reads "first + second" (a modifier and a key or mouse button).
void AddHotkey(int owner, const std::string& icon, const std::string& label, const std::string& secondIcon = "");
void RemoveOwner(int owner);            // a plugin's toolbar choices and key rows, and its budget limit
// The map's piece budget: its limit (the level editor's MaximumMapBudget, read from the game: 300) and what the map
// uses (its pieces' BudgetCost added up, -1 outside the editor). SetBudgetLimit changes the limit for the session, for
// the plugin that set it; 0 gives the game's own back, as does the plugin stopping. The game has had no budget since
// its 2026-10-06 update, so the limit no longer stops anything.
int BudgetLimit();
int BudgetUsed();
bool SetBudgetLimit(int owner, int limit);
void ForceRotateContext(bool on);       // test hook: treat rotations as user rotations without a mouse drag

eng::Obj DetailsContainer();            // where plugin sections are added in the details panel, or null
std::string Status();                   // for the test channel
std::string PiecesStatus();             // test: every piece with its attach parent
bool CallHandler(const std::string& function);   // test: a handler function without parameters

}  // namespace editor
