// The race HUD as elements a plugin can lay out: moved, resized, turned off or forced on, and kept that way while the
// game rebuilds or animates its widgets. Measured on the game (WBP_RaceUIManager and WBP_PlayerUI, read from their
// blueprints):
//   * WBP_RaceUIManager (the controller's CachedRaceUIManager) holds the race's own panels (header, medal row,
//     leaderboard in Menus_Root, countdown, ...) and WBP_PlayerUI, which holds the HUD proper (timer group, speed,
//     split, checkpoint counter, input display, ...).
//   * an element is a widget of those trees that is placed on a canvas, is a user widget (WBP_...), or is a panel the
//     designer named; its key is "RaceUI/<name>" or "PlayerUI/<name>". Menus, modals, towers and the first-time tips
//     (Menus_Root and names with Modal, Tower or FTU) are not elements, except the few HUD panels that live inside
//     them (header, medal row, countdown and leaderboard, taken from the race UI's variables). Plugin windows on
//     screen are elements too ("Window/<plugin>/<n>").
//   * moving uses the widget's slot (canvas position, or padding in a box or overlay), since the game's own
//     animations use the render transform (the speed display on a jump) and would undo a move made there; size uses
//     the render scale; off is render opacity 0 (the game still shows and hides it as it likes); on forces a widget
//     the game hides to be visible.
// Game thread only.
#pragma once
#include <string>
#include <vector>

namespace hud {

void Frame();                                   // after race::Frame

struct Element {
    std::string key, name, className, label;    // label: plugin windows' plugin name, else ""
    bool shown = false;                         // visible, with everything it sits in
    bool parentShown = false;                   // what it sits in is visible (it may be hidden for now, e.g. a split)
};
std::vector<Element> Elements();                // the race HUD on screen now; empty off track

constexpr int kNormal = 0, kOff = 1, kOn = 2;
// A layout for an element, kept (and applied again whenever the game resets it) until cleared; x and y add to where
// the game puts it, in pixels.
void SetLayout(const std::string& key, double x, double y, double scale, int mode);
void ClearLayout(const std::string& key);       // back to the game's own
void SetEditing(bool on);                       // while on, elements laid out as off show faintly instead of not at all
void SetBlink(const std::string& key);          // that element blinks (and shows even if the game hides it); "" none
// A named part of an element (a variable of its widget blueprint, e.g. the input display's "Key_Jump"), tinted;
// ResetPartColor gives it back its own colour.
bool SetPartColor(const std::string& key, const std::string& part, float r, float g, float b, float a);
void ResetPartColor(const std::string& key, const std::string& part);

}  // namespace hud
