// Retained-mode UI for plugins. Plugins describe what they want (footer buttons, panels, windows of widgets) and
// keep those descriptions; the host builds them out of the game's own UMG widgets, keeps them in sync every
// frame, and rebuilds them whenever the game destroys them (menu rebuilt, map loaded). Plugin handles point at
// these descriptions, so they stay valid however often the game widgets behind them are replaced.
//
// Implementation: footer.cpp (the main menu / race footer), windows.cpp (windows anywhere on screen), both built
// from widgets.hpp. Game objects are only ever held as eng::Weak.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "engine.hpp"

namespace ui {

struct Color {
    float r, g, b, a;
};

// --- footer: entries in the game's own footer bar, and a panel above it --------------------------------------------
struct FooterButton {
    int owner = -1;
    std::string label;
    bool clickPending = false;          // set on click, cleared when the plugin reads it
    bool hovered = false;
    // live widgets and what they last showed
    eng::Weak button, text;
    std::string shownLabel;
    bool wasPressed = false, shownHover = false;
    Color normalColor{0.6f, 0.6f, 0.6f, 1};
};

struct Panel {
    int owner = -1;
    std::string title;
    std::vector<std::string> lines;
    std::vector<std::unique_ptr<FooterButton>> buttons;     // a row under the title
    bool visible = false;
    // live widgets and what they last showed (rebuilt only when the title, lines or buttons actually change). The
    // panel is an on-screen widget of its own (host) in front of everything, not part of the game's menu.
    eng::Weak host, border, box;
    bool shownVisible = false, shownOnce = false;
    std::string shownTitle;
    std::vector<std::string> shownLines;
    size_t shownButtons = 0;
};

FooterButton* AddFooterButton(int owner, const std::string& label);
Panel* CreatePanel(int owner);
FooterButton* AddPanelButton(Panel* panel, const std::string& label);

// Everything a plugin made (footer entries, panels, windows) is taken off screen and forgotten. Only for a plugin
// whose script is gone (removed), since its handles are freed.
void RemoveOwner(int owner);
void HideOwner(int owner);              // a stopped plugin: its windows and panels are hidden, not freed

// --- windows: rows of widgets anywhere on screen, in any map --------------------------------------------------------
// A window's main area holds one or more views (groups of rows); one view is shown at a time. A plugin can clear a
// view and fill it again (a list that changes). Cleared widgets are retired, not freed: the plugin may still hold
// handles to them, and those stay valid but show nothing.
enum class Kind { Text, Button, IconButton, Slider, Dropdown, Space, TextArea, TextInput, Image };

struct Window;

struct Widget {
    Window* window = nullptr;
    Kind kind = Kind::Text;
    int row = 0;
    bool inSidebar = false;
    bool retired = false;               // cleared from its view
    bool visible = true;                // hidden widgets take no space
    std::string text;                   // text, button label, icon name ("play" / "pause"), text input hint, or image file
    float size = 16;                    // text size
    float width = 200;                  // slider, dropdown, space, text area, text input and image width; 0 = fill
    float height = 0;                   // text area and image height; 0 = fill
    Color background{0.15f, 0.15f, 0.15f, 1};                               // buttons
    bool backgroundDirty = false;
    Color color{1, 1, 1, 1};
    bool colorDirty = false;
    bool clickPending = false, hovered = false, wasPressed = false;         // buttons
    float value = 0;                                                        // slider, 0..1
    bool dragging = false;
    std::vector<std::string> options;                                       // dropdown
    int selected = 0;
    bool changedPending = false;
    std::string submitted;                                                  // text input: the last submitted text
    bool submitPending = false, submitRequested = false, focusRequested = false, focused = false;
    int focusAttempts = 0;
    bool scrollToEnd = false;                                               // text area
    // live widgets and what they last showed (a text area's main is its ScrollBox, label its TextBlock)
    eng::Weak main, label, iconA, iconB;
    eng::Weak outer;                    // what sits in the row (a SizeBox around sliders, images, ...)
    uint8_t normalVisibility = 0;       // the outer widget's visibility when shown
    bool shownVisible = true;
    std::string shownText;
    float shownValue = -1;
    int shownSelected = -2;
};

struct Window {
    int owner = -1;
    float anchorX = 0.5f, anchorY = 1.0f;       // point of the screen the window is anchored to (0..1)
    float pivotX = 0.5f, pivotY = 1.0f;         // point of the window placed there (0..1)
    float offsetX = 0, offsetY = -40;           // pixels from the anchor
    Color background{0, 0, 0, 0.65f};
    bool blocksClicks = false;                  // clicks on the window never reach what is underneath it
    int zOrder = 100;                           // windows with a higher z-order are drawn in front
    // Movable: while the cursor is on screen the window can be dragged; its position is saved per plugin
    // (Storage "window.<n>.x/y", n = the plugin's nth window) and restored when it is made movable.
    bool movable = false;
    std::string storageOwner;                   // plugin id the position is saved under
    int ordinal = 0;
    float defaultOffsetX = 0, defaultOffsetY = 0;
    bool dragging = false;
    double dragMouseX = 0, dragMouseY = 0, dragOffsetX = 0, dragOffsetY = 0;
    float screenWidth = 0, screenHeight = 0;    // fraction of the screen covered, centred; 0 = fit the content
    float sidebarWidth = 0;                     // 0 = no sidebar
    bool addingToSidebar = false;
    std::vector<int> rowView{0};                // the view each row belongs to
    std::vector<bool> rowRetired{false};        // rows of a cleared view
    int addRow = 0;                             // the row new widgets go into
    int views = 1;
    int shownView = 0;                          // the view on screen
    bool visible = true;
    bool layoutDirty = false;                   // rebuild on the next frame
    std::vector<std::unique_ptr<Widget>> items;
    eng::Weak host, border;
    std::vector<eng::Weak> viewBoxes;
    eng::Weak slot, dragSurface;                // the border's canvas slot, and the invisible button dragged
    int generation = -1;
    bool shownVisible = false;
    int appliedView = -1;
};

Window* MakeWindow(int owner);
void NewRow(Window* w);                 // widgets added after this go on a new row underneath
void StartSidebar(Window* w, float width);  // widgets added after this stack in a column on the left
void StartMain(Window* w);              // ... and after this go back into the rows
int StartView(Window* w);               // widgets added after this go into a new view; returns its number
void ShowView(Window* w, int view);
void ClearView(Window* w, int view);    // retires the view's widgets; widgets added after this go into it
void SetMovable(Window* w, bool movable, const std::string& pluginId);
void ResetPositions(const std::string& pluginId);   // movable windows of a plugin back where the plugin put them
bool HasMovable(const std::string& pluginId);
Widget* AddWidget(Window* w, Kind kind, const std::string& text, float sizeOrWidth);
void AddOption(Widget* dropdown, const std::string& option);
bool Typing();                          // a text input has keyboard focus: keys belong to it, not to plugins

// --- frame and test hooks ------------------------------------------------------------------------------------------
void Frame();                                   // game thread, every frame
bool SimulateClick(const std::string& label);   // a footer or window button by label ("icon", "window:<label>")
bool SimulateSelect(const std::string& firstOption, int index);     // a dropdown, found by its first option
void SimulateSlider(float value);               // every slider reports this value as dragged, for one frame
bool SimulateSubmit(const std::string& text);   // the first text input reports this text as submitted
std::string Status();

namespace footer {
void Frame();
void RemoveOwner(int owner);
void HideOwner(int owner);
bool SimulateClick(const std::string& label);
std::string Status();
}  // namespace footer

namespace windows {
void Frame();
bool SimulateClick(const std::string& label);
bool SimulateSelect(const std::string& firstOption, int index);
void SimulateSlider(float value);
bool SimulateSubmit(const std::string& text);
bool Typing();
void RemoveOwner(int owner);
void HideOwner(int owner);
std::string Status();
}  // namespace windows

}  // namespace ui
