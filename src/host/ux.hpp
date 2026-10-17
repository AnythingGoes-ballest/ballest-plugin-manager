// Building blocks for Cosmetics 2's own screens (the Customize page's strip and slot tabs, the cosmetic browser, the
// plugin manager's window): host-made UMG widgets placed by position on a full-screen canvas of their own, in the game's look
// (its font, Cocogoose Pro; its bubble hover material, M_UI_BubbleButton; rounded boxes per corner). Sizes are widget
// units: at 1920x1080 one unit is one pixel, so design pixels at 1080p are used as they are (at 2560x1440 a unit is 4/3 of
// a pixel). Colours are given as sRGB hex and made linear. Every function does nothing on null objects.
// Game thread only.
#pragma once
#include <string>
#include <vector>

#include "engine.hpp"
#include "ui.hpp"
#include "widgets.hpp"

namespace ux {

using eng::Obj;
using ui::Color;

Color Hex(const char* hex, float alpha = 1);    // "#RRGGBB" (sRGB) as a linear widget colour
Color Mix(const Color& a, const Color& b, double t);
double Smooth(double t);                         // smoothstep of t clamped to 0..1
// Eased: about 63% of the way per tau seconds, but never slower than `finish` a second (so the last bit of the way doesn't
// creep, unseen, and then land as a one-pixel jump: reported).
double Approach(double value, double target, double dt, double tau, double finish = 1.5);
double Wiggle(double seconds);                   // the tab buttons' text wiggle (degrees) this long after it started

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool ok = false;
    double Right() const { return x + w; }
    double Bottom() const { return y + h; }
    bool Has(double px, double py) const { return ok && px >= x && px < x + w && py >= y && py < y + h; }
};
Rect Bounds(Obj widget);                         // where a widget was last drawn, in widget units of the viewport
// A point in the viewport's widget units (as Mouse gives) onto the widget's own units, by where it was last drawn (its
// render scale and wherever its parents put it included); false before it has been drawn.
bool ToLocal(Obj widget, double* x, double* y);
bool Mouse(double* x, double* y);                // the pointer, in the same units
bool ScreenSize(double* w, double* h);

Obj Font();                                      // the game's font (a UFont), or null
Obj BubbleMaterial();                            // the main menu buttons' hover material, or null
// The same bubbles, quiet: grey liquid a little lighter than a dark row, white bubbles and trail (the plugin manager's
// hovers; its lime is kept for what is picked). A dynamic copy of it, or the lime one if it can't be made.
Obj QuietBubbleMaterial();
Obj BlackBubbleMaterial();
Obj WhiteBubbleMaterial();                         // (Ballest Light's: the black one's other way round)
Obj InkBubbleMaterial();                           // (white, its bubbles dark: Ballest Light's settings menu)
// A material for an Image's brush by its asset path (the game's own, "/Game/..."), kept loaded; null if it can't be.
Obj LoadMaterial(const wchar_t* path);
// The game's halftone panel (MI_Halftone_White, the Customize page's background) as a copy of its own, its background
// box scaled past the edges (`boxScale`): only the rounding of the Image it's drawn in shows (the box's own, inset
// corner showed inside the window's, Lvcid). Null if it can't be made.
Obj HalftoneMaterial(float boxScale, bool dark = false);     // dark: the game's own dark one (MI_Halftone)

// A theme's colour map (the plugin manager's Ballest Light, Lvcid 2026-10-11): while one is set, the colours drawn
// through pmui (box fills, texts, UI pictures) and outlines go through it; `kind` 0 a fill, 1 a text, 2 a picture's
// tint, 3 an outline; 4 asks for a text's drop shadow (its colour; alpha 0: none). Set only while the plugin manager draws (null: as given). RawColors: a scope whose colours
// are drawn as given (white words on a pink tab, black words on a lime button, ...).
using ThemeMap = Color (*)(const Color& c, int kind);
void SetThemeMap(ThemeMap map);
// Outlines' alpha times this (the window's fade): a rounded box's outline ignores its parents' render opacity, so as
// the window faded every outline stayed behind, floating (Lvcid's videos).
void SetOutlineFade(double fade);
Color Themed(const Color& c, int kind);
bool ThemeMapped();                              // a map is set and no RawColors is open
struct RawColors {
    RawColors();
    ~RawColors();
    RawColors(const RawColors&) = delete;
    RawColors& operator=(const RawColors&) = delete;
};
Obj LoadPicture(const std::wstring& file);       // a PNG/JPEG file as a texture, cached by file; null if it can't be
// One of the host's own pictures (src/host/pictures, built into the DLL: "slot-ball.png", "globe.png", "glow-halo.png", ...),
// written to the host's cache folder the first time and loaded from there; null if it can't be.
Obj Picture(const char* name);

// A screen of its own: a UserWidget on the viewport whose root is a canvas. Made again after a map change.
struct Screen {
    eng::Weak host, tree, canvas;
    int generation = -1;
    int zOrder = 50;
    bool shown = true;
    bool Ready() const;
    bool Make();                                 // false if it can't be made now (no player controller)
    void Show(bool on);
    void Remove();
    // Everything on it drawn at this size about the screen's middle (a window bigger than a small screen shrinks to fit).
    void Scale(double k);
    double scale = 1;
    Obj Tree() const { return eng::Get(tree); }
    Obj Canvas() const { return eng::Get(canvas); }
};

// A widget on a screen's canvas, changed in place: every setter calls the engine only when the value changed.
struct Node {
    enum Type { kBorder, kImage, kText, kButton, kOther };
    eng::Weak widget, slot;
    Type type = kOther;
    double x = -1e9, y = -1e9, w = -1, h = -1;
    double scale = 1, angle = 0;
    float opacity = 1;
    Color color{-1, -1, -1, -1};
    bool visible = true, centered = false;
    float outline = -1;
    Color outlineColor{-1, -1, -1, -1};
    double corners[4] = {-1, -1, -1, -1};                   // as last set (-1: as made)
    int z = -1000000;                                        // as last set (as made: unknown)
    Obj Get() const { return eng::Get(widget); }
    void Box(double x, double y, double w, double h);       // top left and size
    void Center(double cx, double cy);                       // its middle there, at its own size (texts)
    void Transform(double scale, double angle);              // render scale and angle about its middle
    void Opacity(float opacity);
    void Tint(const Color& c);                               // Border fill, Image tint, text colour
    void Outline(float width, const Color& c);               // a rounded box's outline (Border or Image)
    void Round(double tl, double tr, double br, double bl);  // a rounded box's corners (Border or Image), changed
    void Z(int z);                                           // its order on its canvas, changed
    void Show(bool on);
    void Picture(Obj texture);                               // an Image's texture (or any brush resource)
    void Text(const std::string& s);
    double TextWidth() const;                                // a text's width at its own font size (0 before it is laid out)
    std::string shownText;
    eng::Weak shownPicture;
    int hasPicture = -1;
};

// Made on `canvas` (z: drawn over lower ones); they start hidden at 0,0 until placed.
Node MakeBox(Obj tree, Obj canvas, const Color& fill, double tl, double tr, double br, double bl, int z);
Node MakePicture(Obj tree, Obj canvas, double radius, int z);            // an Image drawn as a rounded box
Node MakeBubble(Obj tree, Obj canvas, double tl, double tr, double br, double bl, int z, int look = 0);    // the bubble hover (look: 0 lime, 1 quiet grey, 2 black, 3 white, 4 white with dark bubbles)
Node MakeMaterialImage(Obj tree, Obj canvas, Obj material, double radius, int z);       // an Image of a material, rounded
// alignX: which point of the text Node::Center places (0 its left end, 0.5 its middle, 1 its right end), vertically centred.
void TextFace(Obj text, double px, const char* face);
void TextShadow(Obj text, double dx, double dy, const Color& c);   // a text block's own drop shadow (alpha 0: none)
void TextOutline(Obj text, int size, const Color& c);               // its letters' own outline (size 0: none); lay it out again after      // a text widget's size (design px) and face, changed
Node MakeText(Obj tree, Obj canvas, const std::string& s, double px, const Color& c, int z, const char* face = "Regular",
              double alignX = 0.5);
Node MakeHit(Obj tree, Obj canvas, int z);                                // an invisible button (clicks and hover)
Node Place(Obj canvas, Obj widget, int z, Node::Type type = Node::kOther);   // any widget, placed with Node::Box

// A press that ends while the pointer is still over the button.
struct Click {
    bool wasPressed = false, hovered = false;
    bool Update(const Node& hit);                            // true once, on the click
};

// The rounded-box parts of a brush ("Background" of a Border, "Brush" of an Image): corner radii, outline.
void RoundBrush(Obj widget, const char* brush, double tl, double tr, double br, double bl);
void OutlineBrush(Obj widget, const char* brush, float width, const Color& c);

// Plain UMG pieces for scrolling lists inside a panel (the browser's grid).
Obj Spawn(const char* cls, Obj tree);
Obj TextBlock(Obj tree, const std::string& s, double px, const Color& c, const char* face = "Regular");
Obj Rounded(Obj tree, const Color& fill, double radius, ui::widgets::Margin padding);
Obj Sized(Obj tree, Obj child, double w, double h);
void InBox(Obj vbox, Obj child, ui::widgets::Margin padding, uint8_t alignX = 0);
void InRow(Obj row, Obj child, double left, bool fill = false, uint8_t alignY = 2);

}  // namespace ux
