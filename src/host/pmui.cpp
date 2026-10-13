#include "pmui.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game.hpp"
#include "input.hpp"
#include "log.hpp"

namespace w = ui::widgets;

namespace pmui {
namespace {

Layer gRoot;
eng::Weak gTree;
Pointer gPointer;
double gScale = 1, gCx = 0, gCy = 0;
double gNow = 0, gDt = 0, gLast = -1;
long long gFrame = 0;
int gModal = 0, gLevel = 0;
std::string gCandidate, gPressedOn;     // what a press this frame is on; what the button went down on
std::unordered_map<std::string, long long> gChangedAt;     // when a text's key last changed (its size is read later)
std::unordered_map<std::string, double> gWidths;           // measured widths, by size, face and text
std::vector<Rect> gZones, gLastZones;       // inner scrolling boxes (on screen), this frame and last
std::vector<Rect> gShields;                 // boxes only Pierce'd asks answer in, this frame (Shield)
bool gPierce = false;
std::string gHoverNow, gHoverLast;          // the topmost clickable under the pointer: this frame, last frame
bool gMoved = false;                        // the pointer moved this frame (the hover sound: only then)
void (*gHoverSink)(const std::string&) = nullptr;

// the focus (keys and pads)
struct Candidate {
    std::string id;
    Rect r;                                 // on screen (design units), as much of it as shows
};
std::vector<Candidate> gCands, gLastCands;  // this frame's Click targets at the top level; last frame's (to move among)
bool gNav = false, gMouseMoved = false;
std::string gFocus, gPress, gPrefer;
Rect gFocusRect;
bool gFocusKnown = false;
double gRealX = -1e9, gRealY = -1e9;        // the real mouse (moving it ends the focus)
int gLastModal = 0;
std::vector<std::pair<std::string, Rect>> gFocusStack;     // the focus under each level opened over it
double gScreenArea = 1;

std::string WidthKey(const std::string& s, double px, const std::string& face) { return std::to_string(px) + "|" + face + "|" + s; }

Obj Tree() { return eng::Get(gTree); }

Entry& Use(Layer& l, const std::string& key, bool* made) {
    Entry& e = l.items[key];
    *made = !e.node.Get();
    e.used = true;
    return e;
}

Obj Built(const char* name) {
    static std::unordered_map<std::string, eng::Weak> cache;
    eng::Weak& slot = cache[name];
    if (Obj t = eng::Get(slot)) return t;
    Obj t = ux::Picture(name);
    slot = eng::MakeWeak(t);
    return t;
}

// The brush drawn as a box (9-sliced): margins as fractions of the picture, the picture `units` wide and tall.
void SliceBrush(Obj image, double margin, double units, double unitsTall) {
    Obj cls = eng::ClassOf(image);
    eng::Prop p;
    int at = eng::NestedOffset(cls, {"Brush", "DrawAs"}, &p);
    if (at >= 0 && p.size == 1) image[at] = 1;                     // ESlateBrushDrawType::Box
    at = eng::NestedOffset(cls, {"Brush", "Margin"}, &p);
    if (at >= 0 && p.size == 16) {
        const float m[4] = {float(margin), float(margin), float(margin), float(margin)};
        std::memcpy(image + at, m, sizeof m);
    }
    at = eng::NestedOffset(cls, {"Brush", "ImageSize"}, &p);
    if (at >= 0 && p.size == 16) {
        const double s[2] = {units, unitsTall};
        std::memcpy(image + at, s, sizeof s);
    } else if (at >= 0 && p.size == 8) {
        const float s[2] = {float(units), float(unitsTall)};
        std::memcpy(image + at, s, sizeof s);
    }
}

// Measured once the widget has been laid out with the new text (two frames on: the frame it changes still has the
// old size).
void Measure(Entry& e, const std::string& key, const std::string& s, bool height) {
    if (e.measured != s) {
        e.measured = s;
        e.width = e.height = 0;
        gChangedAt[key] = gFrame;
        return;
    }
    if ((height ? e.height : e.width) > 0 || gFrame - gChangedAt[key] < 2) return;
    Obj o = e.node.Get();
    if (!o) return;
    const w::Vec2 size = eng::Call(o, "GetDesiredSize").ReturnAs<w::Vec2>(w::Vec2{0, 0});
    e.width = size.x;
    e.height = size.y;
    if (!height && e.width > 0 && e.px > 0) gWidths[WidthKey(s, e.px, e.face)] = e.width;
}

void Sweep(Layer& l) {
    for (auto& [key, e] : l.items) {
        if (!e.used) e.node.Show(false);
        e.used = false;
    }
    for (auto& [key, g] : l.groups) {
        if (!g->used) {
            if (g->shown) {
                w::SetVisibility(g->node.Get(), w::kCollapsed);
                g->shown = false;
            }
            continue;
        }
        g->used = false;
        Sweep(*g);
    }
}

}  // namespace

double Estimate(const std::string& s, double px) { return static_cast<double>(s.size()) * px * 0.55; }

double Width(const std::string& s, double px, const std::string& face) {
    const auto it = gWidths.find(WidthKey(s, px, face));
    return it != gWidths.end() ? it->second : Estimate(s, px);
}

Rect Intersect(const Rect& a, const Rect& b) {
    const double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0, std::min(a.R(), b.R()) - x), std::max(0.0, std::min(a.B(), b.B()) - y)};
}

// --- the frame ------------------------------------------------------------------------------------------------------
Layer& Begin(ux::Screen& screen, double scale) {
    gNow = game::Seconds();
    gDt = gLast < 0 ? 0 : std::min(0.1, gNow - gLast);
    gLast = gNow;
    ++gFrame;
    if (eng::Get(gRoot.canvas) != screen.Canvas()) Reset();
    gRoot.canvas = eng::MakeWeak(screen.Canvas());
    gTree = eng::MakeWeak(screen.Tree());
    double sw = 1920, sh = 1080;
    ux::ScreenSize(&sw, &sh);
    gScale = scale > 0.1 ? scale : 1;
    gCx = sw / 2;
    gCy = sh / 2;
    gRoot.clip = {-1e6, -1e6, 2e6, 2e6};
    gRoot.used = true;
    double mx = 0, my = 0;
    const bool have = ux::Mouse(&mx, &my);
    const double px = gPointer.x, py = gPointer.y;
    double rx = have ? gCx + (mx - gCx) / gScale : -1e9, ry = have ? gCy + (my - gCy) / gScale : -1e9;
    // The pointer onto the screen's canvas by where the canvas really was drawn (ux::ToLocal), not only by the sum above
    // (its scale about the viewport's middle): they should agree; Lvcid's 2560x1440 monitor had every click shifted
    // after a resize. Where they part, the canvas is believed (what's drawn is where it answers), and it's logged.
    // (a frame it can't be measured (a new screen, a failed call): the last measured difference kept, so the pointer
    // doesn't jump between the two and turn the pad's focus off on its own)
    static double offX = 0, offY = 0;
    if (double lx = mx, ly = my; have && ux::ToLocal(screen.Canvas(), &lx, &ly)) {
        offX = lx - rx;
        offY = ly - ry;
        static double toldAt = -100;
        if ((std::fabs(lx - rx) > 2 || std::fabs(ly - ry) > 2) && gNow - toldAt > 10) {
            toldAt = gNow;
            char line[256];
            std::snprintf(line, sizeof line, "pmui: the pointer by the canvas (%.1f, %.1f) and by the sum (%.1f, %.1f) differ: screen %.0fx%.0f, scale %.3f, mouse (%.1f, %.1f)",
                          lx, ly, rx, ry, sw, sh, gScale, mx, my);
            hostlog::Info(line);
        }
        rx = lx;
        ry = ly;
    } else if (have) {
        rx += offX;
        ry += offY;
    }
    const bool down = input::Down(VK_LBUTTON);
    // the real mouse moved or clicked: the focus is off, the pointer the mouse's again
    gMouseMoved = std::fabs(rx - gRealX) + std::fabs(ry - gRealY) > 3 || (down && !gPointer.down);
    if (gNav && gMouseMoved) gNav = false;
    gRealX = rx;
    gRealY = ry;
    gScreenArea = (sw / gScale) * (sh / gScale);
    gPointer.x = gNav && gFocusKnown ? gFocusRect.Cx() : rx;
    gPointer.y = gNav && gFocusKnown ? gFocusRect.Cy() : ry;
    gMoved = std::fabs(gPointer.x - px) + std::fabs(gPointer.y - py) > 0.5;
    gPointer.pressed = down && !gPointer.down;
    gPointer.released = !down && gPointer.down;
    gPointer.down = down;
    gPointer.wheel = input::Wheel();
    gCandidate.clear();
    gCands.clear();
    gLevel = 0;
    gShields.clear();
    gPierce = false;
    return gRoot;
}

namespace {
const Candidate* FindCand(const std::string& id) {
    for (const Candidate& c : gLastCands)
        if (c.id == id) return &c;
    return nullptr;
}
const Candidate* Nearest(double x, double y) {
    const Candidate* best = nullptr;
    double bestD = 1e18;
    for (const Candidate& c : gLastCands) {
        const double dx = c.r.Cx() - x, dy = c.r.Cy() - y, d = dx * dx + dy * dy;
        if (d < bestD) bestD = d, best = &c;
    }
    return best;
}
void FocusOn(const Candidate* c) {
    gFocusKnown = c != nullptr;
    if (!c) return;
    gFocus = c->id;
    gFocusRect = c->r;
}
}  // namespace

void End() {
    // what can take the focus: this frame's targets, not the window-wide ones (the window, the dim)
    gLastCands.clear();
    for (const Candidate& c : gCands)
        if (c.r.w * c.r.h < 0.3 * gScreenArea) gLastCands.push_back(c);
    // a level opened over the focus: into it; closed: back to the focus under it
    if (gModal != gLastModal) {
        if (gModal > gLastModal) {
            gFocusStack.push_back({gFocus, gFocusRect});
            gFocus.clear();
        } else if (!gFocusStack.empty()) {
            gFocus = gFocusStack.back().first;
            gFocusRect = gFocusStack.back().second;
            gFocusStack.pop_back();
        }
        gLastModal = gModal;
    }
    if (gModal == 0) gFocusStack.clear();
    if (const Candidate* c = FindCand(gFocus)) FocusOn(c);
    else if (const Candidate* p = gPrefer.empty() ? nullptr : FindCand(gPrefer)) FocusOn(p);
    else if (!gLastCands.empty()) FocusOn(gFocusKnown ? Nearest(gFocusRect.Cx(), gFocusRect.Cy()) : Nearest(gRealX, gRealY));
    else gFocusKnown = false;
    gPrefer.clear();
    gPress.clear();
    // the focus edge (Lvcid's option A): a soft light edge and a faint glow round it, not lime; only while keys or a
    // pad move it
    if (gNav && gFocusKnown) {
        const Rect f = gFocusRect.Grow(3);
        const double rad = std::min(16.0, f.h / 2);
        const Color clear{1, 1, 1, 0};
        for (int i = 3; i >= 1; --i) {      // the glow: wider, fainter rings outside the edge
            const double g = 2.0 * i;
            BoxCorners(gRoot, "nav.glow" + std::to_string(i), 3999, f.Grow(g), clear, rad + g, rad + g, rad + g, rad + g)
                .Outline(2.0f, Color{1, 1, 1, static_cast<float>(0.07 / i)});
        }
        BoxCorners(gRoot, "nav.ring", 4000, f, clear, rad, rad, rad, rad).Outline(2.0f, Color{1, 1, 1, 0.55f});
    }
    gLastZones.swap(gZones);
    gZones.clear();
    Sweep(gRoot);
    if (gPointer.pressed) gPressedOn = gCandidate;
    if (gHoverNow != gHoverLast && !gHoverNow.empty() && gHoverSink && gMoved) gHoverSink(gHoverNow);
    gHoverLast = gHoverNow;
    gHoverNow.clear();
    if (gPointer.released || !gPointer.down) gPressedOn.clear();
}

void Reset() {
    gRoot.items.clear();
    gRoot.groups.clear();
    gRoot.canvas = {};
    gChangedAt.clear();
    gPressedOn.clear();
}

double Now() { return gNow; }
Obj TreeOf() { return Tree(); }
double Dt() { return gDt; }

// --- drawing ----------------------------------------------------------------------------------------------------------
ux::Node& BoxCorners(Layer& l, const std::string& key, int z, const Rect& r, const Color& fill, double tl, double tr, double br, double bl) {
    bool made = false;
    Entry& e = Use(l, key, &made);
    const Color c = ux::Themed(fill, 0);
    if (made) e.node = ux::MakeBox(Tree(), l.Canvas(), c, tl, tr, br, bl, z);
    e.node.Round(tl, tr, br, bl);
    e.node.Z(z);
    e.node.Show(true);
    e.node.Box(r.x, r.y, r.w, r.h);
    e.node.Tint(c);
    // (a theme's trim for it: Ballest Light's black edge round anything lime; the caller's own Outline after this wins)
    const Color trim = ux::ThemeMapped() ? ux::Themed(fill, 5) : Color{0, 0, 0, 0};
    if (trim.a > 0.004) {
        e.node.Outline(1.3f, trim);
        e.trimmed = true;
    } else if (e.trimmed) {
        e.node.Outline(0, {0, 0, 0, 0});
        e.trimmed = false;
    }
    return e.node;
}

ux::Node& Box(Layer& l, const std::string& key, int z, const Rect& r, const Color& fill, double radius) {
    return BoxCorners(l, key, z, r, fill, radius, radius, radius, radius);
}

ux::Node& Bubble(Layer& l, const std::string& key, int z, const Rect& r, double radius, double alpha, int look) {
    // (Ballest Light: the black and the grey ones white; the lime one with a black trim, as every lime there)
    if ((look == 2 || look == 1) && ux::ThemeMapped()) return Bubble(l, key + "~w", z, r, radius, alpha, 3);
    bool made = false;
    Entry& e = Use(l, key, &made);
    if (made) e.node = ux::MakeBubble(Tree(), l.Canvas(), radius, radius, radius, radius, z, look);
    e.node.Round(radius, radius, radius, radius);
    e.node.Z(z);
    e.node.Show(alpha > 0.004);
    e.node.Box(r.x, r.y, r.w, r.h);
    e.node.Tint({1, 1, 1, static_cast<float>(std::clamp(alpha, 0.0, 1.0))});
    if (look == 0 && ux::ThemeMapped()) {
        ux::RawColors raw;
        e.node.Outline(1.3f, {0, 0, 0, static_cast<float>(0.85 * std::clamp(alpha, 0.0, 1.0))});
        e.trimmed = true;
    } else if (e.trimmed) {
        e.node.Outline(0, {0, 0, 0, 0});
        e.trimmed = false;
    }
    return e.node;
}

ux::Node& Texture(Layer& l, const std::string& key, int z, const Rect& r, Obj texture, const Color& tint, double radius) {
    bool made = false;
    Entry& e = Use(l, key, &made);
    if (made) e.node = ux::MakePicture(Tree(), l.Canvas(), radius, z);
    e.node.Round(radius, radius, radius, radius);
    e.node.Z(z);
    e.node.Show(true);
    e.node.Picture(texture);
    e.node.Box(r.x, r.y, r.w, r.h);
    e.node.Tint(tint);
    return e.node;
}

ux::Node& Pic(Layer& l, const std::string& key, int z, const Rect& r, const char* picture, const Color& tint, double radius) {
    // (pictures with colours of their own, and effects, as given: the theme previews, the logo, the knobs, the chrome,
    // the glows and smoke, the twinkle)
    static const char* const kOwn[] = {"pm-theme-", "pm-logo", "pm-knob", "pm-ring-", "pm-glow", "pm-refresh-glow", "pm-twinkle", "fx-", "glow-", "pm-x-glow"};
    bool own = false;
    for (const char* p : kOwn) own = own || std::strncmp(picture, p, std::strlen(p)) == 0;
    // (a soft fade continues a fill or a line: repainted as one, not as an icon, or it came out darker than what it
    // continues: a bead on a line, a hard step on a panel)
    const bool fade = std::strncmp(picture, "pm-fade", 7) == 0;
    return Texture(l, key, z, r, Built(picture), own ? tint : ux::Themed(tint, fade ? 0 : 2), radius);
}

ux::Node& Material(Layer& l, const std::string& key, int z, const Rect& r, const wchar_t* path, double radius, double alpha) {
    return MaterialOf(l, key, z, r, ux::LoadMaterial(path), radius, alpha);
}

ux::Node& MaterialOf(Layer& l, const std::string& key, int z, const Rect& r, Obj material, double radius, double alpha) {
    static ux::Node none;                   // (not loaded (yet): nothing drawn, not a plain box in its place)
    if (!material) return none;
    bool made = false;
    Entry& e = Use(l, key, &made);
    if (made) e.node = ux::MakeMaterialImage(Tree(), l.Canvas(), material, radius, z);
    e.node.Round(radius, radius, radius, radius);
    e.node.Z(z);
    e.node.Show(alpha > 0.004);
    e.node.Box(r.x, r.y, r.w, r.h);
    e.node.Tint({1, 1, 1, static_cast<float>(std::clamp(alpha, 0.0, 1.0))});
    return e.node;
}

ux::Node& Sliced(Layer& l, const std::string& key, int z, const Rect& r, const char* picture, double margin, double units, const Color& tint,
                 double unitsTall) {
    bool made = false;
    Entry& e = Use(l, key, &made);
    if (made) e.node = ux::MakePicture(Tree(), l.Canvas(), 0, z);
    e.node.Show(true);
    const bool had = e.node.hasPicture == 1;
    e.node.Picture(Built(picture));
    if (!had && e.node.hasPicture == 1) SliceBrush(e.node.Get(), margin, units, unitsTall > 0 ? unitsTall : units);
    e.node.Box(r.x, r.y, r.w, r.h);
    e.node.Tint(tint);
    return e.node;
}

bool gShadowSet = false;                    // (TextShadowScope)
Color gShadowColour{0, 0, 0, 0};

double Text(Layer& l, const std::string& key, int z, const std::string& s, double px, const char* face, const Color& given, double x, double cy,
            double alignX) {
    const Color c = ux::Themed(given, 1);
    // (a theme's drop shadow for it, tight (Ballest Light's lime and gold words), or a TextShadowScope's: the text
    // block's own)
    const Color shadow = gShadowSet ? gShadowColour : ux::ThemeMapped() ? ux::Themed(given, 4) : Color{0, 0, 0, 0};
    bool made = false;
    Entry& e = Use(l, key, &made);
    if (made) {
        e.node = ux::MakeText(Tree(), l.Canvas(), s, px, c, z, face, alignX);
        e.measured.clear();
        e.px = px;
        e.face = face;
    } else if (std::fabs(e.px - px) > 0.01 || e.face != face) {
        // a new size or face (a growing title): set, and measured again; its text sent again, so it is laid out at the
        // new size now (the size alone, written into the block, was missed: a closed card's name stayed big)
        ux::TextFace(e.node.Get(), px, face);
        e.px = px;
        e.face = face;
        e.measured.clear();
        e.node.shownText.clear();
    }
    if (std::fabs(shadow.a - e.shadowA) > 0.01f) {
        // (a theme's: tight; a TextShadowScope's: the main menu's, a little further)
        const double d = gShadowSet ? std::max(1.2, px * 0.09) : std::max(0.8, px * 0.05);
        ux::TextShadow(e.node.Get(), d * 0.8, d, shadow);
        e.shadowA = shadow.a;
    }
    e.node.Show(true);
    e.node.Text(s);
    e.node.Tint(c);
    e.node.Center(x, cy);
    Measure(e, key + "@" + std::to_string(reinterpret_cast<uintptr_t>(&l)), s, false);
    return e.width > 0 ? e.width : Width(s, px, face);
}

double Para(Layer& l, const std::string& key, int z, const std::string& s, double px, const char* face, const Color& given, const Rect& r) {
    const Color c = ux::Themed(given, 1);
    bool made = false;
    Entry& e = Use(l, key, &made);
    if (made) {
        Obj text = ux::TextBlock(Tree(), s, px, c, face);
        if (text) {
            eng::Call(text, "SetAutoWrapText", uint8_t{1});
            w::SetVisibility(text, w::kHitTestInvisible);
        }
        e.node = ux::Place(l.Canvas(), text, z, ux::Node::kText);
        e.node.color = c;
        e.node.shownText = s;
        e.measured.clear();
    }
    e.node.Show(true);
    e.node.Text(s);
    e.node.Tint(c);
    e.node.Box(r.x, r.y, r.w, std::max(r.h, 20.0));
    Measure(e, key + "@" + std::to_string(reinterpret_cast<uintptr_t>(&l)), s + "|" + std::to_string(static_cast<int>(r.w)), true);
    return e.height;
}

Obj Field(Layer& l, const std::string& key, int z, const Rect& r, const std::string& hint, double px, const Color& given, const char* face) {
    const Color c = ux::Themed(given, 1);       // (as it's made: a theme switched later shows on the next new field)
    bool made = false;
    Entry& e = Use(l, key, &made);
    if (made) {
        Obj input = w::Spawn("EditableTextBox", Tree());
        if (input) {
            eng::WriteBool(input, "ClearKeyboardFocusOnCommit", false);
            for (const char* member : {"ForegroundColor", "FocusedForegroundColor"}) w::WriteSlateColor(input, {"WidgetStyle", member}, c);
            w::WriteSlateColor(input, {"WidgetStyle", "TextStyle", "ColorAndOpacity"}, c);
            w::WriteSlateColor(input, {"WidgetStyle", "BackgroundColor"}, {0, 0, 0, 0});
            eng::Prop fontProp;
            const int at = eng::NestedOffset(eng::ClassOf(input), {"WidgetStyle", "TextStyle", "Font", "FontObject"}, &fontProp);
            if (Obj font = ux::Font(); font && at >= 0 && fontProp.size == static_cast<int32_t>(sizeof font)) std::memcpy(input + at, &font, sizeof font);
            (void)face;
            w::SetFontSize(input, static_cast<float>(px * 0.75), {"WidgetStyle", "TextStyle", "Font"});
            w::SetHintText(input, hint);
        }
        e.node = ux::Place(l.Canvas(), input, z, ux::Node::kButton);
        e.colour = c;
    }
    // (its words' colour changed (another theme): set on it)
    if (Obj input = e.node.Get(); input && (std::fabs(e.colour.r - c.r) > 0.002f || std::fabs(e.colour.g - c.g) > 0.002f ||
                                            std::fabs(e.colour.b - c.b) > 0.002f || std::fabs(e.colour.a - c.a) > 0.002f)) {
        // (the focused and typed colours too, as it was made with: they kept the old theme's while typing)
        for (const char* member : {"ForegroundColor", "FocusedForegroundColor"}) w::WriteSlateColor(input, {"WidgetStyle", member}, c);
        w::WriteSlateColor(input, {"WidgetStyle", "TextStyle", "ColorAndOpacity"}, c);
        eng::Call(input, "SetForegroundColor", c);
        e.colour = c;
    }
    e.node.Show(true);
    e.node.Box(r.x, r.y, r.w, r.h);
    return e.node.Get();
}

Layer& Group(Layer& l, const std::string& key, int z, const Rect& r, bool clip, double opacity) {
    std::unique_ptr<Layer>& g = l.groups[key];
    if (!g || !g->node.Get()) {
        g = std::make_unique<Layer>();
        Obj canvas = w::Spawn("CanvasPanel", Tree());
        if (canvas) {
            w::SetVisibility(canvas, w::kSelfHitTestInvisible);
            if (clip) eng::Call(canvas, "SetClipping", uint8_t{1});         // EWidgetClipping::ClipToBounds
        }
        g->node = ux::Place(l.Canvas(), canvas, z);
        g->canvas = eng::MakeWeak(canvas);
        g->shown = true;
    }
    g->used = true;
    if (!g->shown) {
        w::SetVisibility(g->node.Get(), w::kSelfHitTestInvisible);
        g->shown = true;
    }
    g->node.Box(r.x, r.y, r.w, r.h);
    g->node.Opacity(static_cast<float>(std::clamp(opacity, 0.0, 1.0)));
    g->ox = l.ox + r.x;
    g->oy = l.oy + r.y;
    g->clip = clip ? Intersect(l.clip, {g->ox, g->oy, r.w, r.h}) : l.clip;
    return *g;
}

TextShadowScope::TextShadowScope(const Color& c) {
    gShadowSet = true;
    gShadowColour = c;
}
TextShadowScope::~TextShadowScope() { gShadowSet = false; }

void TextOutline(Layer& l, const std::string& key, int size, const Color& c) {
    Entry* e = Find(l, key);
    if (!e || e->outline == size) return;
    ux::TextOutline(e->node.Get(), size, c);
    e->outline = size;
    if (Obj o = e->node.Get()) w::SetText(o, e->node.shownText);   // (laid out again with it)
}

Entry* Find(Layer& l, const std::string& key) {
    const auto it = l.items.find(key);
    return it == l.items.end() ? nullptr : &it->second;
}

std::string Fit(const std::string& s, double width, double maxWidth) {
    if (width <= 0 || width <= maxWidth || s.size() < 4) return s;
    size_t n = static_cast<size_t>(std::max(0.0, std::floor(static_cast<double>(s.size()) * (maxWidth - 14) / width)));
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;      // not inside a UTF-8 letter
    while (n > 0 && s[n - 1] == ' ') --n;
    return s.substr(0, n) + "...";
}

// --- the pointer --------------------------------------------------------------------------------------------------------
const Pointer& Mouse() { return gPointer; }
void SetModal(int level) { gModal = level; }
int Modal() { return gModal; }
void Level(int level) { gLevel = level; }

Rect OnScreen(const Layer& l, const Rect& r) { return {l.ox + r.x, l.oy + r.y, r.w, r.h}; }

bool Over(const Layer& l, const Rect& r) {
    if (gLevel < gModal) return false;
    if (!gPierce)
        for (const Rect& s : gShields)
            if (s.Has(gPointer.x, gPointer.y)) return false;
    return Intersect(l.clip, OnScreen(l, r)).Has(gPointer.x, gPointer.y);
}

bool Click(const Layer& l, const Rect& r, const std::string& id, bool hoverSound) {
    if (gLevel >= gModal && hoverSound) {   // (hover-silent ones, backgrounds and scroll bars, never take the focus)
        const Rect on = Intersect(l.clip, OnScreen(l, r));
        if (on.w > 2 && on.h > 2) gCands.push_back({id, on});
        if (!gPress.empty() && id == gPress) {
            gPress.clear();
            return true;
        }
    }
    if (!Over(l, r)) return false;
    gHoverNow = hoverSound ? id : "";       // (the last one drawn is on top, as for the press)
    if (gPointer.pressed) gCandidate = id;
    return gPointer.released && gPressedOn == id;
}

bool Held(const std::string& id) { return gPointer.down && gPressedOn == id; }

bool PressedOutside(const Layer& l, const Rect& r) {
    return gPointer.pressed && !Intersect(l.clip, OnScreen(l, r)).Has(gPointer.x, gPointer.y);
}

double Wheel(const Layer& l, const Rect& r) {
    if (!Over(l, r)) return 0;
    // an inner box under the pointer (smaller than this area) takes the wheel instead
    const Rect on = OnScreen(l, r);
    for (const Rect& z : gLastZones)
        if (z.Has(gPointer.x, gPointer.y) && z.w * z.h < 0.9 * on.w * on.h) return 0;
    return gPointer.wheel;
}

void WheelZone(const Layer& l, const Rect& r) { gZones.push_back(Intersect(l.clip, OnScreen(l, r))); }

void Shield(const Layer& l, const Rect& r) { gShields.push_back(OnScreen(l, r)); }
void Pierce(bool on) { gPierce = on; }
void OnHoverStart(void (*sink)(const std::string& id)) { gHoverSink = sink; }

void Nav(int dx, int dy) {
    if (!gNav || !gFocusKnown || (dx == 0 && dy == 0)) {
        // turned on (or only shown): where it was, if that's still there, else nearest the mouse
        gNav = true;
        if (const Candidate* c = FindCand(gFocus)) FocusOn(c);
        else FocusOn(Nearest(gRealX, gRealY));
    } else {
        // the nearest thing that way, a sideways step costing more than one straight on
        const double fx = gFocusRect.Cx(), fy = gFocusRect.Cy();
        const Candidate* best = nullptr;
        double bestScore = 1e18;
        for (const Candidate& c : gLastCands) {
            if (c.id == gFocus) continue;
            const double vx = c.r.Cx() - fx, vy = c.r.Cy() - fy;
            const double along = vx * dx + vy * dy, side = std::fabs(vx * dy - vy * dx);
            if (along < 6) continue;
            const double score = along + side * 2.5;
            if (score < bestScore) bestScore = score, best = &c;
        }
        if (best) FocusOn(best);
        else if (dy != 0) gPointer.wheel += dy > 0 ? -1 : 1;     // (the end of what shows: the list under it scrolls)
    }
    if (gFocusKnown) {
        gPointer.x = gFocusRect.Cx();
        gPointer.y = gFocusRect.Cy();
    }
}
// Tab / Shift+Tab: the next (or previous) thing in reading order (top to bottom, then left to right), wrapping round
void NavOrder(int step) {
    if (gLastCands.empty()) return;
    std::vector<const Candidate*> order;
    for (const Candidate& c : gLastCands) order.push_back(&c);
    std::stable_sort(order.begin(), order.end(), [](const Candidate* a, const Candidate* b) {
        if (std::fabs(a->r.Cy() - b->r.Cy()) > 8) return a->r.Cy() < b->r.Cy();
        return a->r.Cx() < b->r.Cx();
    });
    if (!gNav || !gFocusKnown) return Nav(0, 0);
    size_t at = 0;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i]->id == gFocus) at = i;
    const size_t n = order.size();
    FocusOn(order[(at + n + static_cast<size_t>(step > 0 ? 1 : n - 1)) % n]);
    gPointer.x = gFocusRect.Cx();
    gPointer.y = gFocusRect.Cy();
}
bool Navigating() { return gNav && gFocusKnown; }
void NavOff() { gNav = false; }
std::string Focus() { return gFocusKnown ? gFocus : ""; }
void Press(const std::string& id) { gPress = id; }
void PreferFocus(const std::string& id) {
    if (gPrefer.empty()) gPrefer = id;
}
bool MouseMoved() { return gMouseMoved; }

}  // namespace pmui
