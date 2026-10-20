// Immediate-mode drawing for the plugin manager window (manager.cpp), on ux's widgets: every frame the window's code
// asks for its boxes, pictures, texts and text boxes by key, where they are now. The widget behind a key is made the
// first time it is asked for, changed only when a value changes (ux::Node), and hidden in a frame that doesn't ask for
// it. Groups are canvases of their own inside the screen (optionally clipping what is drawn on them: rows, lists).
//
// Hover and clicks are worked out from the pointer, not from button widgets: one invisible button under everything
// takes the clicks away from the game, and a click belongs to the last thing drawn under the pointer that asked for it
// (so a row's own buttons win over the row). Levels keep a popup's clicks to the popup.
//
// Sizes and positions are design units: 1920x1080 at scale 1, the screen's units divided by the window's scale.
// Game thread only.
#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "ux.hpp"

namespace pmui {

using eng::Obj;
using ui::Color;

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    double R() const { return x + w; }
    double B() const { return y + h; }
    double Cx() const { return x + w / 2; }
    double Cy() const { return y + h / 2; }
    bool Has(double px, double py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    Rect Grow(double d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }
    Rect Moved(double dx, double dy) const { return {x + dx, y + dy, w, h}; }
};
Rect Intersect(const Rect& a, const Rect& b);

struct Entry {
    ux::Node node;
    bool used = false;
    double width = 0, height = 0;           // a text's measured size, for the text in `measured`
    std::string measured;
    double px = 0;                          // a text's size and face
    std::string face;
    bool trimmed = false;                   // given a theme's trim (a lime fill's black edge on the light panel)
    float shadowA = 0;                      // a text's drop shadow as set (its alpha; 0: none)
    int outline = 0;                        // a text's letters' outline as set (pmui::TextOutline)
    Color colour{-1, -1, -1, -1};           // a field's text colour as set
};

struct Layer {
    eng::Weak canvas;
    ux::Node node;                          // a group's canvas on its parent (unused for the screen's own)
    double ox = 0, oy = 0;                  // its top left, in the screen's design units
    Rect clip;                              // what of the screen it shows, design units
    std::unordered_map<std::string, Entry> items;
    std::unordered_map<std::string, std::unique_ptr<Layer>> groups;
    bool used = false, shown = true;
    Obj Canvas() const { return eng::Get(canvas); }
};

// --- the frame ------------------------------------------------------------------------------------------------------
// Begin binds the root layer to the screen's canvas (again after a new screen), reads the pointer and the mouse button,
// and turns the screen's units into design units for `scale` about the screen's middle. End hides what wasn't drawn.
Layer& Begin(ux::Screen& screen, double scale);
void End();
void Reset();                               // a new screen: every widget is gone
double Now();
double Dt();

// --- drawing ----------------------------------------------------------------------------------------------------------
// Rounded boxes (radius, or each corner): the shape is fixed when the key is first drawn.
ux::Node& Box(Layer& l, const std::string& key, int z, const Rect& r, const Color& fill, double radius = 0);
ux::Node& BoxCorners(Layer& l, const std::string& key, int z, const Rect& r, const Color& fill, double tl, double tr, double br, double bl);
// The game's lime bubble (M_UI_BubbleButton), at an alpha; `look` 1 (true): its grey copy (hovers), 2: a black copy.
ux::Node& Bubble(Layer& l, const std::string& key, int z, const Rect& r, double radius, double alpha, int look = 0);
// One of the host's own pictures (ux::Picture), tinted; or any texture.
ux::Node& Pic(Layer& l, const std::string& key, int z, const Rect& r, const char* picture, const Color& tint, double radius = 0);
// An Image of a material (the game's own, kept loaded: ux::LoadMaterial), rounded, at an alpha.
ux::Node& Material(Layer& l, const std::string& key, int z, const Rect& r, const wchar_t* path, double radius, double alpha);
ux::Node& MaterialOf(Layer& l, const std::string& key, int z, const Rect& r, Obj material, double radius, double alpha);   // (one in hand)
ux::Node& Texture(Layer& l, const std::string& key, int z, const Rect& r, Obj texture, const Color& tint, double radius = 0);
// A picture drawn 9-sliced over `r`: its outer `margin` (a fraction of it, each side) keeps its size, `units` across
// for the whole picture; the middle stretches. For glows round boxes of any size.
ux::Node& Sliced(Layer& l, const std::string& key, int z, const Rect& r, const char* picture, double margin, double units, const Color& tint,
                 double unitsTall = 0);     // (a picture that isn't square: `units` wide, this tall)
// One line of text: `alignX` of it at x (0 its left end, 0.5 its middle, 1 its right end), its middle at cy. The face
// and size are fixed when the key is first drawn. Returns its width (estimated until it has been laid out once).
double Text(Layer& l, const std::string& key, int z, const std::string& s, double px, const char* face, const Color& c, double x,
            double cy, double alignX = 0);
// Text wrapped inside r's width, from its top. Returns its height (0 until laid out).
double Para(Layer& l, const std::string& key, int z, const std::string& s, double px, const char* face, const Color& c, const Rect& r);
// A text box (EditableTextBox) in the game's font; the widget, to read and focus.
Obj Field(Layer& l, const std::string& key, int z, const Rect& r, const std::string& hint, double px, const Color& c, const char* face = "Regular");
// A canvas of its own at r (its children are placed from r's top left), clipping them to r when `clip`.
Layer& Group(Layer& l, const std::string& key, int z, const Rect& r, bool clip, double opacity = 1);
Entry* Find(Layer& l, const std::string& key);
// A drawn text's letters outlined (size in pixels, 0: none), after its pmui::Text this frame.
void TextOutline(Layer& l, const std::string& key, int size, const Color& c);
// A drop shadow for the texts drawn while it's open (as the game's main menu's words have), whatever the theme's map
// says; its colour's alpha 0: none.
struct TextShadowScope {
    explicit TextShadowScope(const Color& c);
    ~TextShadowScope();
    TextShadowScope(const TextShadowScope&) = delete;
    TextShadowScope& operator=(const TextShadowScope&) = delete;
};

double Estimate(const std::string& s, double px);          // a text's width before it is laid out (the game's font)
// A text's width in a size and face: as measured once on screen (by any key), else estimated. Measured widths are kept,
// so shortening a text to fit doesn't flip between its long and short forms (each measured in turn: a flicker).
double Width(const std::string& s, double px, const std::string& face);
// A text shortened with "..." to about `maxWidth`, from its width `width` at full length (by its length: good enough
// for one line in the game's font).
std::string Fit(const std::string& s, double width, double maxWidth);

// --- the pointer --------------------------------------------------------------------------------------------------------
struct Pointer {
    double x = -1e9, y = -1e9;              // design units
    bool down = false, pressed = false, released = false;
    double wheel = 0;                       // turned this frame (up is positive)
};
const Pointer& Mouse();
// Interactions below the modal level do nothing (a popup open: only it, level 3, answers).
void SetModal(int level);
int Modal();
void Level(int level);                      // the level of what is drawn next
bool Over(const Layer& l, const Rect& r);   // the pointer is on r (in l's units) and on what l shows
bool Click(const Layer& l, const Rect& r, const std::string& id, bool hoverSound = true);    // pressed and let go on it
// The topmost clickable thing under the pointer changed to `id` (not to nothing): `sink` is told at End (the hover sound;
// a Click with hoverSound false, as a scroll bar or the window's own background, never counts).
void OnHoverStart(void (*sink)(const std::string& id));
bool Held(const std::string& id);           // pressed on it and still held (dragging)
bool PressedOutside(const Layer& l, const Rect& r);     // a press this frame anywhere but r
double Wheel(const Layer& l, const Rect& r);                        // the wheel, while the pointer is on r
// A scrolling box inside another (the disk's, in the settings pane): while the pointer is on it, the wheel is its alone
// (Wheel answers 0 for the bigger areas round it). Asked for each frame it's drawn.
void WheelZone(const Layer& l, const Rect& r);
// A box that only answers what is asked inside Pierce(true) ... Pierce(false): nothing else under it is Over (no hover, no
// press, no wheel) this frame (the window's resize edge under the pointer, or everything while one is dragged).
void Shield(const Layer& l, const Rect& r);
void Pierce(bool on);
Rect OnScreen(const Layer& l, const Rect& r);                       // r in the screen's design units

// --- keys and pads: the focus --------------------------------------------------------------------------------------------
// Everything drawn with Click at the top level (the modal level and up; not the hover-silent ones: backgrounds, scroll
// bars) can take the focus. A direction moves it to the nearest such thing that way (none that way, up or down: the
// list under it scrolls instead); a soft light edge shows it (Lvcid's option A: not lime). While it's on, the pointer is
// where the focus is, so hover, tooltips and the wheel follow it; moving the real mouse (or clicking) turns it off. When
// a higher level opens (a popup, a list) the focus goes into it (to PreferFocus's id, else the thing nearest the old
// focus) and comes back to where it was when that level closes.
void Nav(int dx, int dy);                   // a direction (0, 0: only show the focus)
void NavOrder(int step);                    // Tab (+1) / Shift+Tab (-1): next / previous in reading order
bool Navigating();
void NavOff();
std::string Focus();                        // the focused thing's id, or ""
void Press(const std::string& id);          // that id's Click answers true this frame, as if clicked
void PreferFocus(const std::string& id);    // a level that opens this frame starts on it (a popup's safe button)
bool MouseMoved();                          // the real mouse moved (or clicked) this frame

}  // namespace pmui
