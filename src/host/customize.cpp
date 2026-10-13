#include "customize.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "browser.hpp"
#include "cosmetics.hpp"
#include "game.hpp"
#include "log.hpp"
#include "manager.hpp"
#include "packages.hpp"
#include "plugins.hpp"
#include "ux.hpp"
#include "widgets.hpp"

using eng::Obj;
using ui::Color;
namespace w = ui::widgets;

namespace customize {
namespace {

// The globe and the custom section's "cosmetic browser": the plugin manager's cosmetics page (its new look), else the
// old browser window.
void OpenBrowser() {
    if (manager::NewLook()) manager::Open("cosmetics");
    else browser::Open();
}

// --- the look (measured from Lvcid's concept and the approved previews, 1920x1080 game pixels) -----------------------
const Color kLime = ux::Hex("#BEFF00"), kBlack = ux::Hex("#000000"), kWhite = ux::Hex("#FFFFFF");
const Color kStrip = ux::Hex("#1E1E1E"), kPublicText = ux::Hex("#5F6163"), kGrey = ux::Hex("#5C5F63");
const Color kDark = ux::Hex("#18191C"), kSlotBack = ux::Hex("#16171A"), kRing = ux::Hex("#2B2D31");
const Color kGreyIn = ux::Hex("#828589"), kLight = ux::Hex("#EBEBEB");
const Color kScrollBar = ux::Hex("#92C105");       // the panel's scroll bar as it shows (its BCFC00 thumb, drawn dimmer)
const Color kPale = ux::Hex("#C4FF1C");            // the hover effect's sparks: a strong lime (drawn faint, it loses its green)
// The strip: from the balls tab's right edge (x 238) and top (y 275) to x 252..524, y 140..190.
constexpr double kStripLeft = 14, kStripTop = -131, kStripWidth = 272, kStripHeight = 50;   // (4 lower than the concept: Lvcid, to sit on the panel's light grey)
constexpr double kModeFont = 24;
constexpr double kGlobeX = 305, kGlobeY = 22, kGlobe = 48;     // its middle, from the strip's top left
// The slot tabs and loadout tabs a little bigger than the approved previews (Lvcid, after the first test).
constexpr double kScale = 1.15;
// Loadouts: right-aligned to the tabs' edge, y 200..230 (at scale 1).
constexpr double kLoadoutWidth = 28 * kScale, kLoadoutHeight = 30 * kScale, kLoadoutGap = 4 * kScale, kLoadoutFont = 20 * kScale;
constexpr double kLoadoutTop = -75 - (kLoadoutHeight - 30);
// Slot tabs (Slot tabs v4): slots from the tabs' edge (2 left of the white bar), tops from the balls tab's.
constexpr double kBase = 44 * kScale;      // the slots a little bigger again (room round their pictures, as the game's tiles)
constexpr double kGrow = 20 * kScale;    // the hovered slot pops a little more than in the previews
constexpr double kGap = 8 * kScale, kOver = 5 * kScale, kNear = 4 * kScale;
constexpr double kNameNear = 0.38;      // how far a neighbour's name grows towards full (the nearer one more)
constexpr double kNameEase = 0.06, kSlotEase = 0.1;     // names grow a little quicker than their slots
constexpr double kSlotRange = 80 * kScale, kFullFont = 25 * kScale;
constexpr double kBackGap = 16;
constexpr double kEase = 0.1;                     // seconds: the pointer entering or leaving the column, hovers
// A name tab's look in each state: text size, angle (style 1 sideways is -90), padding, thickness, text colour, corner.
struct Look {
    double font, rot, pad, thick;
    Color fg;
    double r;
};
Look Blend(const Look& a, const Look& b, double t) {
    auto l = [&](double x, double y) { return x + (y - x) * t; };
    return {l(a.font, b.font), l(a.rot, b.rot), l(a.pad, b.pad), l(a.thick, b.thick), ux::Mix(a.fg, b.fg, t), l(a.r, b.r)};
}
struct Looks {
    Look small, smallIn, med, medIn, full;
};
Look Scaled(Look l) {
    l.font *= kScale;
    l.pad *= kScale;
    l.thick *= kScale;
    l.r *= kScale;
    return l;
}
Looks LooksFor(int style) {
    const Look full{25, 0, 14, 34, kBlack, 8}, medIn{16, 0, 8, 24, kBlack, 6};
    Looks l = style == 1 ? Looks{{9, -90, 5, 15, kGrey, 5}, {11, 0, 6, 18, kGreyIn, 5}, {11, -90, 5, 19, kBlack, 6}, medIn, full}
                         : Looks{{10.5, 0, 6, 17, kGrey, 5}, {12, 0, 7, 19, kGreyIn, 5}, {16, 0, 8, 24, kBlack, 6}, medIn, full};
    for (Look* each : {&l.small, &l.smallIn, &l.med, &l.medIn, &l.full}) *each = Scaled(*each);
    return l;
}

// --- state -----------------------------------------------------------------------------------------------------------
ux::Screen gScreen;
double gLast = -1;

// The hover shine of every button here: a soft halo of the button's shape and tiny rays along its edge, over everything
// round it (reported: the neighbours hid it). Each shape has its own pictures (Handoff/sources/button_glow.py:
// "glow-<shape>-halo.png", "-rays0..3.png", 12 units of room round the shape); the four ray phases are cross-faded in
// turn, so the rays travel round the edge. (Rays placed one by one, turning smoothly, were tried: Lvcid liked these.)
struct Shine {
    ux::Node halo, rays[4];
    std::string shape;                             // which shape's pictures it has
};
constexpr int kShineZ = 40;                        // above every other part of the page's overlay
constexpr double kShineRoom = 12, kShineCycle = 1.2;   // units; seconds for the rays to move one ray on

struct Button {                                    // a strip or loadout button: a box, the bubble, a text, a hit area
    ux::Node base, bubble, text, hit;
    Shine shine;
    ux::Click click;
    double hover = 0, wiggleAt = -100;
    bool wasHovered = false;
};
struct Strip {
    ux::Node edge, back;
    // the open mode's tab joined down to the panel, a line along the panel's top and its lime, and the tab's foot
    // (its last few units turning into the line's green)
    ux::Node join, line, lineLime, foot, under;    // under: the line's quick fade into the grey below it
    Button local, pub;
    ux::Node globeEdge, globeRing, globeInner, globePic, globeHit;
    Shine globeShine;
    ux::Click globeClick;
    double globeHover = 0;
    double localOpen = -1;                         // eased: 1 while local is the open mode (its tab taller)
    bool built = false;
} gStrip;
std::vector<Button> gLoadouts;
int gLoadoutsShown = -1;

struct SlotTab {
    std::string slot;
    ux::Node outer, ring, back, pic, nameBase, nameBubble, nameText, hit;
    ux::Node join;                      // the open slot's name joined to its ring (over the black round it: Lvcid)
    ux::Node rim[3];                    // hovered: a lime ring round the name's left end, fading lines along it
    double radius = -1;                 // the name's corners as last set (round: half its height)
    Shine slotShine;                    // the hovered slot's, over its name tab too
    ux::Click click;
    double textWidth = 0;
    double hover = 0;                   // the slot: eased towards 1 while the pointer is on its row
    double lit = 0;                     // the name's colours (bubble, text): the same, a little quicker
    double grow = 0;                    // the name's size: its row's 1, a neighbour's part of it by distance
    double swell = 0;                   // the slot's growth as a neighbour of the hovered one
    double flashAt = -100;              // clicked while open: its white border flashes lime
};
std::vector<SlotTab> gSlots;
std::string gSlotsKey;
ux::Node gShadow;                       // the soft oval shade behind the column (tab-shadow.png, Handoff/sources/tab_shadow.py)
double gG = 0, gPy = 0;
bool gInside = false, gArmed = false;
int gWiggleSlot = -1;
double gWiggleAt = -100;

// The hover effect of a slot row (Lvcid's pick, Handoff/images/tab-beam-hq.mp4; its pictures from
// Handoff/sources/hover_fx.py): as a row is newly hovered, a soft lime glow blooms from its slot along its name tab, a fine
// thread of light lights with one wash sweeping out along it, and a few sparks leave along curling paths (most along the
// tab's top and bottom edges from the slot, some from its end), white turning lime, each ending in a soft smoky puff;
// then only smoke stays, faintly, drifting left while the row is hovered. Two of them, so the last row's can finish as the
// next one starts.
struct Spark {
    double t0, life, along, startIn, side, jitter, dist, amp, f, ph, drift, r;
    double shown, whiteFor;                        // how much of its path it's seen for; how far from the tab it stays white
    bool edge;
};
struct HoverFx {
    ux::Node glow, smokeA, smokeB, thread, wash;
    std::vector<ux::Node> sparks, puffs;
    std::vector<Spark> plan;
    int row = -1;
    double at = -100;
    double endAt = 1e9;                            // when the next row's started: this one fades out quickly then
};
constexpr int kFxSparks = 13, kSmokeFrames = 32;
constexpr double kSmokeLoop = 7.3;                 // seconds for the smoke frames to loop
HoverFx gFx[2];
int gFxRow = -1, gFxNext = 0;
std::mt19937 gFxRandom{std::random_device{}()};

eng::Weak gStack;                                  // the game's tabs (hidden while ours are shown)
uint8_t gStackVisibility = 0;
bool gStackHidden = false;
// Where the balls tab is from the cosmetics panel (both are in the page's OptionalInset, which its fly-in moves): measured
// once while the game's tabs are drawn, so they can then be hidden outright (see HideGameTabs).
bool gTabMeasured = false;
double gTabDx = 0, gTabDy = 0, gTabW = 0, gTabH = 0;
eng::Weak gBack;                                   // the game's back button (WBP_MainMenu_UIManager's)
double gBackLooked = -100;
bool gBackMoved = false;
bool gLoggedLayout = false;

void Drop(ux::Node& n) {
    if (Obj o = n.Get()) eng::Call(o, "RemoveFromParent");
    n = {};
}
void DropFx(HoverFx& fx) {
    for (ux::Node* n : {&fx.glow, &fx.smokeA, &fx.smokeB, &fx.thread, &fx.wash}) Drop(*n);
    for (auto& n : fx.sparks) Drop(n);
    for (auto& n : fx.puffs) Drop(n);
    fx = {};
}
void DropShine(Shine& s) {
    for (ux::Node* n : {&s.halo, &s.rays[0], &s.rays[1], &s.rays[2], &s.rays[3]}) Drop(*n);
}
void DropButton(Button& b) {
    for (ux::Node* n : {&b.base, &b.bubble, &b.text, &b.hit}) Drop(*n);
    DropShine(b.shine);
}

void ResetAll() {
    gStrip = {};
    gLoadouts.clear();
    gLoadoutsShown = -1;
    gSlots.clear();
    gSlotsKey.clear();
    gShadow = {};
    for (auto& fx : gFx) fx = {};
    gFxRow = -1;
}

// --- the game's widgets ------------------------------------------------------------------------------------------------
Obj Panel(Obj page) {
    Obj header = eng::ReadObj(page, "HB_CollectionSubheader");
    return eng::Call(eng::Call(eng::Call(header, "GetParent").ReturnObj(), "GetParent").ReturnObj(), "GetParent").ReturnObj();
}

// The game's tabs hidden (ESlateVisibility::Hidden: still laid out, not drawn, not clickable). Render opacity alone was
// not enough: the tab buttons' rounded outlines are drawn whatever the opacity (reported: their edges showed behind ours).
// Until where they are has been measured they are only made see-through for a frame.
void HideGameTabs(Obj page) {
    Obj stack = eng::ReadObj(page, "PLAYLISTTABS_Stack");
    if (!stack) return;
    if (eng::Get(gStack) != stack) {
        gStack = eng::MakeWeak(stack);
        gStackVisibility = eng::Call(stack, "GetVisibility").ReturnAs<uint8_t>(0);
        gStackHidden = false;
    }
    if (!gTabMeasured) {
        const ux::Rect tab = ux::Bounds(eng::ReadObj(page, "Skins_Tab")), panel = ux::Bounds(Panel(page));
        if (tab.ok && panel.ok) {
            gTabMeasured = true;
            gTabDx = tab.x - panel.x;
            gTabDy = tab.y - panel.y;
            gTabW = tab.w;
            gTabH = tab.h;
        } else {
            eng::Call(stack, "SetRenderOpacity", 0.001f);
            w::SetVisibility(stack, w::kHitTestInvisible);
            return;
        }
    }
    if (gStackHidden) return;
    eng::Call(stack, "SetRenderOpacity", 1.0f);
    w::SetVisibility(stack, uint8_t{2});            // Hidden
    gStackHidden = true;
}

void ShowGameTabs() {
    Obj stack = eng::Get(gStack);
    if (!stack || !gStackHidden) return;
    eng::Call(stack, "SetRenderOpacity", 1.0f);
    w::SetVisibility(stack, gStackVisibility);
    gStackHidden = false;
}

// Where the game's balls tab is (hidden or not): the anchor of the strip, the loadouts and the slot tabs.
ux::Rect BallsTab(Obj page) {
    ux::Rect r;
    if (!gTabMeasured) return r;
    const ux::Rect panel = ux::Bounds(Panel(page));
    if (!panel.ok) return r;
    r.x = panel.x + gTabDx;
    r.y = panel.y + gTabDy;
    r.w = gTabW;
    r.h = gTabH;
    r.ok = true;
    return r;
}

Obj BackButton() {
    if (Obj b = eng::Get(gBack)) return b;
    if (game::Seconds() - gBackLooked < 1) return nullptr;
    gBackLooked = game::Seconds();
    Obj cls = eng::FindClass("WBP_MainMenu_UIManager_C"), found = nullptr;
    if (!cls) return nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o)) found = o;
        return found == nullptr;
    });
    Obj back = found ? eng::ReadObj(found, "WBP_BackButton") : nullptr;
    gBack = eng::MakeWeak(back);
    return back;
}

// A widget's render translation (RenderTransform.Translation), read where the engine keeps it; false if it is not laid
// out as an FVector2D of doubles (then the back button is left where it is).
bool Translation(Obj widget, w::Vec2* out) {
    eng::Prop last;
    const int off = widget ? eng::NestedOffset(eng::ClassOf(widget), {"RenderTransform", "Translation"}, &last) : -1;
    if (off < 0 || last.size != static_cast<int32_t>(sizeof(w::Vec2))) {
        static bool logged = false;
        if (!logged) hostlog::Warn("customize: a widget's render translation is not laid out as measured; the back button stays");
        logged = true;
        return false;
    }
    std::memcpy(out, widget + off, sizeof *out);
    return true;
}

// The back button `shift` units down from where the game puts it (render translation: the page's layout is untouched).
void ShiftBack(double toTop) {
    Obj back = BackButton();
    if (!back) return;
    const ux::Rect r = ux::Bounds(back);
    w::Vec2 now{0, 0};
    if (!r.ok || !Translation(back, &now)) return;
    const double shift = toTop < -1e8 ? 0 : toTop - (r.y - now.y);
    if (std::fabs(now.y - shift) < 0.5) return;
    eng::Call(back, "SetRenderTranslation", w::Vec2{now.x, shift});
    gBackMoved = shift != 0;
}

void RestoreBack() {
    Obj back = eng::Get(gBack);
    w::Vec2 now{0, 0};
    if (!back || !gBackMoved || !Translation(back, &now)) return;
    eng::Call(back, "SetRenderTranslation", w::Vec2{now.x, 0});
    gBackMoved = false;
}

// --- building ----------------------------------------------------------------------------------------------------------
Shine MakeShine(Obj tree, Obj canvas) {
    Shine s;
    s.halo = ux::MakePicture(tree, canvas, 0, kShineZ);
    for (ux::Node& r : s.rays) r = ux::MakePicture(tree, canvas, 0, kShineZ);
    for (ux::Node* n : {&s.halo, &s.rays[0], &s.rays[1], &s.rays[2], &s.rays[3]}) n->Opacity(0);
    return s;
}

// A shine this frame round the box (x, y, w, h), its pictures stretched over it and their room, lit `lit`, in `tint`.
void UpdateShine(Shine& s, const std::string& shape, double x, double y, double wd, double ht, double lit, double now,
                 const Color& tint = kWhite) {
    if (s.shape != shape) {
        s.shape = shape;
        s.halo.Picture(ux::Picture(("glow-" + shape + "-halo.png").c_str()));
        for (int i = 0; i < 4; ++i) s.rays[i].Picture(ux::Picture(("glow-" + shape + "-rays" + std::to_string(i) + ".png").c_str()));
    }
    const double t = std::fmod(now / kShineCycle, 1.0) * 4;
    const double bx = x - kShineRoom, by = y - kShineRoom, bw = wd + 2 * kShineRoom, bh = ht + 2 * kShineRoom;
    s.halo.Box(bx, by, bw, bh);
    s.halo.Tint(tint);
    s.halo.Opacity(s.halo.hasPicture == 1 ? static_cast<float>(lit) : 0.0f);    // (no picture: an Image draws white)
    for (int i = 0; i < 4; ++i) {
        const double apart = std::fabs(t - i), weight = std::max(0.0, 1 - std::min(apart, 4 - apart));
        s.rays[i].Box(bx, by, bw, bh);
        s.rays[i].Tint(tint);
        s.rays[i].Opacity(s.rays[i].hasPicture == 1 ? static_cast<float>(lit * weight) : 0.0f);
    }
}

Button MakeButton(Obj tree, Obj canvas, const std::string& label, double px, double tl, double tr, double br, double bl, int z) {
    Button b;
    b.shine = MakeShine(tree, canvas);
    b.base = ux::MakeBox(tree, canvas, kBlack, tl, tr, br, bl, z + 1);
    b.bubble = ux::MakeBubble(tree, canvas, tl, tr, br, bl, z + 2);
    b.text = ux::MakeText(tree, canvas, label, px, kGrey, z + 3);
    b.hit = ux::MakeHit(tree, canvas, z + 4);
    return b;
}

void BuildStrip() {
    Obj tree = gScreen.Tree(), canvas = gScreen.Canvas();
    gStrip.edge = ux::MakeBox(tree, canvas, kBlack, 18, 26, 0, 0, 1);
    gStrip.back = ux::MakeBox(tree, canvas, kStrip, 16, 24, 0, 0, 2);
    gStrip.local = MakeButton(tree, canvas, "local", kModeFont, 6, 0, 0, 0, 3);
    gStrip.pub = MakeButton(tree, canvas, "public", kModeFont, 0, 12, 0, 0, 3);
    gStrip.globeEdge = ux::MakeBox(tree, canvas, kBlack, 26, 26, 26, 26, 1);
    gStrip.globeRing = ux::MakeBox(tree, canvas, kStrip, 24, 24, 24, 24, 2);
    gStrip.globeInner = ux::MakeBox(tree, canvas, kBlack, 19, 19, 19, 19, 3);
    gStrip.globePic = ux::MakePicture(tree, canvas, 0, 4);
    gStrip.globeHit = ux::MakeHit(tree, canvas, 6);
    gStrip.globePic.Picture(ux::Picture("globe.png"));
    gStrip.globeShine = MakeShine(tree, canvas);
    gStrip.join = ux::MakeBox(tree, canvas, kLime, 0, 0, 0, 0, 4);
    gStrip.line = ux::MakeBox(tree, canvas, kScrollBar, 1.5, 1.5, 0, 0, 4);  // its top corners a touch round
    gStrip.lineLime = ux::MakePicture(tree, canvas, 0, 4);
    gStrip.lineLime.Picture(ux::Picture("line-fade.png"));
    gStrip.foot = ux::MakePicture(tree, canvas, 0, 6);
    gStrip.foot.Picture(ux::Picture("fade-down.png"));
    gStrip.under = ux::MakePicture(tree, canvas, 0, 4);
    gStrip.under.Picture(ux::Picture("fade-up.png"));
    gStrip.built = true;
}

// A strip or loadout button this frame: its box, how lit its bubble is (hover), its text and its wiggle.
// Hovered: plain white (Lvcid's pick over the bubble, after a test on local and public), or the bubble.
enum class HoverLook { kBubble, kWhite };
void UpdateButton(Button& b, double x, double y, double wd, double ht, bool active, const Color& idleText, double dt, double now,
                  const std::string& shape, double textDx = 0, HoverLook look = HoverLook::kWhite, const Color& shineTint = kWhite,
                  double grow = 0, bool bubbleAtRest = false) {
    const bool hovered = b.click.hovered;
    if (hovered && !b.wasHovered) b.wiggleAt = now;
    b.wasHovered = hovered;
    b.hover = ux::Approach(b.hover, hovered ? 1.0 : 0.0, dt, hovered ? kEase * 0.6 : kEase * 0.25);
    const double h = b.hover;
    const double hitX = x, hitY = y, hitW = wd, hitH = ht;
    const double k = active ? 1 : 1 + grow * h;     // a little bigger while hovered (not the open one: it's joined below)
    x -= wd * (k - 1) / 2;
    y -= ht * (k - 1) / 2;
    wd *= k;
    ht *= k;
    b.base.Box(x, y, wd, ht);
    b.bubble.Box(x, y, wd, ht);
    if (look == HoverLook::kWhite) {
        b.base.Tint(ux::Mix(active ? kLime : kBlack, kWhite, h));
        b.base.Opacity(1);
        // the open one at rest: the game's bubble button in lime (Lvcid); plain white while hovered
        // the open mode's tab at rest: the game's bubble button over its lime, half strength so it stays lime (at full
        // strength it read darker green: Lvcid); plain white while hovered. Loadout tabs stay flat lime.
        // (untinted: the game's own lime, as its buttons; tinted lime it doubled the green: Lvcid)
        b.bubble.Tint(active && bubbleAtRest ? Color{1, 1, 1, static_cast<float>(1 - h)} : Color{1, 1, 1, 0});
        b.bubble.Outline(0, {1, 1, 1, 0});
    } else {
        b.base.Tint(active ? kLime : kBlack);
        b.base.Opacity(static_cast<float>(1 - h));
        b.bubble.Tint({1, 1, 1, static_cast<float>(h)});
        b.bubble.Outline(static_cast<float>(4 * h), {1, 1, 1, static_cast<float>(std::min(1.0, h * 1.5))});
    }
    b.text.Center(x + wd / 2 + textDx, y + ht / 2);
    b.text.Tint(ux::Mix(active ? kBlack : idleText, kBlack, h));
    b.text.Transform(k, ux::Wiggle(now - b.wiggleAt));
    b.hit.Box(hitX, hitY, hitW, hitH);
    UpdateShine(b.shine, shape, x, y, wd, ht, h, now, shineTint);
}

// The panel's grey (its BackgroundPanel_1, 10 short of its right edge; its light grey inside a dark frame about 4.5 wide,
// measured from Lvcid's screenshots).
struct PanelTop {
    ux::Rect grey;
    double right = 0;                               // where the line along its top ends
    bool ok = false;
};
PanelTop PanelTopOf(Obj page) {
    PanelTop t;
    Obj panel = Panel(page);
    const ux::Rect whole = ux::Bounds(panel);
    if (!whole.ok) return t;
    Obj grey = eng::Call(panel, "GetChildAt", int32_t{0}).ReturnObj();
    t.grey = grey ? ux::Bounds(grey) : ux::Rect{};
    if (!t.grey.ok) {
        t.grey = whole;
        t.grey.w -= 10;
    }
    t.right = t.grey.Right();
    t.ok = true;
    return t;
}

void UpdateStrip(const ux::Rect& tab, Obj page, double dt, double now) {
    const PanelTop panel = PanelTopOf(page);
    if (!gStrip.built) BuildStrip();
    const double sx = tab.Right() + kStripLeft, sy = tab.y + kStripTop;
    // (a unit shorter at the bottom: lowered, it would cover the light grey's top edge)
    gStrip.edge.Box(sx - 2, sy - 2, kStripWidth + 4, kStripHeight + 1);
    gStrip.back.Box(sx, sy, kStripWidth, kStripHeight - 1);
    const bool local = cosmetics::LocalMode();
    // The chosen mode's button is the taller one (local in the concept: 38 high from y 150, public 30 from 152), eased
    // between the two (reported: its text jumped a little as the mode changed).
    gStrip.localOpen = gStrip.localOpen < 0 ? (local ? 1 : 0) : ux::Approach(gStrip.localOpen, local ? 1.0 : 0.0, dt, kEase * 0.8);
    const double lo = gStrip.localOpen, po = 1 - lo;
    const double localY = sy + 12 - 2 * lo, localH = 30 + 8 * lo, pubY = sy + 12 - 2 * po, pubH = 30 + 8 * po;
    UpdateButton(gStrip.local, sx + 7, localY, 128, localH, local, kPublicText, dt, now, lo > 0.5 ? "local-38" : "local-30", 0,
                 HoverLook::kWhite, kWhite, 0.05, true);
    UpdateButton(gStrip.pub, sx + 144, pubY, 120, pubH, !local, kPublicText, dt, now, po > 0.5 ? "public-38" : "public-30", -5,
                 HoverLook::kWhite, kWhite, 0.05, true);
    // The open mode's tab runs down into the panel, and a thin line of its colour runs along the panel's top (Lvcid's
    // pick, "D"); both turn white with it while it's hovered. Hidden while the panel isn't just below (it flies in).
    {
        const Button& open = local ? gStrip.local : gStrip.pub;
        const double ax = sx + (local ? 7 : 144), aw = local ? 128 : 120, bottom = local ? localY + localH : pubY + pubH;
        // the line sits on the light grey's top edge, over the frame, as wide as the light grey, 4 tall (Lvcid's edits)
        constexpr double kLineH = 4, kFootH = 9, kUnderH = 3;
        const double panelTop = panel.ok ? panel.grey.y + 2 : sy + 47, gap = panelTop - bottom;
        const bool shown = gap > -8 && gap < 30;      // (the tab runs a little into the line: same colour)
        const Color c = ux::Mix(kLime, kWhite, open.hover);
        gStrip.join.Show(shown && gap > 0);          // (only while the tab hasn't reached the line yet)
        gStrip.line.Show(shown && panel.ok);
        gStrip.lineLime.Show(false);                  // (the tab's foot turns into the line's green instead: Lvcid's edit)
        gStrip.under.Show(shown && panel.ok);
        gStrip.foot.Show(shown);
        // the tab's foot: its lime easing into the line's green, ending exactly where the line starts
        gStrip.foot.Box(ax, panelTop - kFootH, aw, kFootH);
        gStrip.foot.Tint(kScrollBar);
        gStrip.foot.Opacity(static_cast<float>(1 - open.hover));
        if (shown) {
            gStrip.join.Box(ax, bottom - 1, aw, gap + 2);
            gStrip.join.Tint(c);
            if (panel.ok) {
                // the scroll bar's colour, turning into the open tab's lime under it (Lvcid); all white while it's hovered
                const double lx = panel.grey.x + 4, lr = panel.grey.Right() - 4;
                gStrip.line.Box(lx, panelTop, lr - lx, kLineH);
                gStrip.line.Tint(ux::Mix(kScrollBar, kWhite, open.hover));
                // and a quick fade from its green into the grey below (over whatever grey the theme makes it)
                gStrip.under.Box(lx, panelTop + kLineH, lr - lx, kUnderH);
                gStrip.under.Tint(ux::Mix(kScrollBar, kWhite, open.hover));
            }
        }
    }
    if (gStrip.local.click.Update(gStrip.local.hit) && !local) cosmetics::SetLocalMode(true);
    if (gStrip.pub.click.Update(gStrip.pub.hit) && local) cosmetics::SetLocalMode(false);
    // The cosmetic browser: a 48 circle, its 5 ring the strip's colour, black inside, the lime globe; white ring on hover.
    const double cx = sx + kGlobeX, cy = sy + kGlobeY;
    const bool over = gStrip.globeClick.hovered;
    gStrip.globeHover = ux::Approach(gStrip.globeHover, over ? 1.0 : 0.0, dt, over ? kEase * 0.6 : kEase * 0.25);
    const double g = kGlobe * (1 + 0.07 * gStrip.globeHover);      // a little bigger while hovered
    gStrip.globeEdge.Box(cx - g / 2 - 2, cy - g / 2 - 2, g + 4, g + 4);
    gStrip.globeRing.Box(cx - g / 2, cy - g / 2, g, g);
    gStrip.globeRing.Tint(ux::Mix(kStrip, kWhite, gStrip.globeHover));
    gStrip.globeInner.Box(cx - g / 2 + 5, cy - g / 2 + 5, g - 10, g - 10);
    const double pic = 30 * g / kGlobe;
    gStrip.globePic.Box(cx - pic / 2, cy - pic / 2, pic, pic);
    gStrip.globeHit.Box(cx - kGlobe / 2, cy - kGlobe / 2, kGlobe, kGlobe);
    const double edge = g + 4;                                     // its shine round the black ring
    UpdateShine(gStrip.globeShine, "globe", cx - edge / 2, cy - edge / 2, edge, edge, gStrip.globeHover, now);
    if (gStrip.globeClick.Update(gStrip.globeHit)) OpenBrowser();
}

void UpdateLoadouts(const ux::Rect& tab, bool local, double dt, double now) {
    const int count = packages::LoadoutCount();
    const int wanted = local && count > 0 ? count + 1 : 0;      // and the "+"
    if (wanted != gLoadoutsShown) {
        for (auto& b : gLoadouts) DropButton(b);
        gLoadouts.clear();
        for (int i = 0; i < wanted; ++i)
            gLoadouts.push_back(MakeButton(gScreen.Tree(), gScreen.Canvas(), i == count ? "+" : std::to_string(i + 1), kLoadoutFont, 4, 0, 0, 4, 3));
        gLoadoutsShown = wanted;
    }
    const int active = packages::ActiveLoadout();
    const double right = tab.Right(), y = tab.y + kLoadoutTop;
    for (int i = 0; i < wanted; ++i) {
        Button& b = gLoadouts[static_cast<size_t>(i)];
        const double x = right - (wanted - i) * kLoadoutWidth - (wanted - 1 - i) * kLoadoutGap;
        const bool plus = i == count;
        UpdateButton(b, x, y, kLoadoutWidth, kLoadoutHeight, !plus && i == active, plus ? kLime : kGrey, dt, now, "loadout");
        if (b.click.Update(b.hit)) {
            if (plus) packages::AddLoadout();
            else packages::UseLoadout(i);
        }
    }
}

Obj Silhouette(const std::string& slot) {
    for (const char* known : {"ball", "hat", "bfx", "arms", "bounce"})
        if (slot == known) return ux::Picture(("slot-" + slot + ".png").c_str());
    return ux::Picture("slot-extra.png");
}

// Text widths measured per slot, kept across rebuilds (switching local / public rebuilds the column: without them each
// name was drawn at a guessed width for a frame, then at its real one: the column twitched, reported).
std::vector<std::pair<std::string, double>> gNameWidths;

void BuildSlots(const std::vector<std::string>& slots) {
    for (auto& s : gSlots) {
        if (s.textWidth <= 0) continue;
        bool kept = false;
        for (auto& [slot, width] : gNameWidths)
            if (slot == s.slot) {
                width = s.textWidth;
                kept = true;
            }
        if (!kept) gNameWidths.emplace_back(s.slot, s.textWidth);
    }
    for (auto& s : gSlots) {
        for (ux::Node* n : {&s.outer, &s.ring, &s.back, &s.pic, &s.nameBase, &s.nameBubble, &s.nameText, &s.hit, &s.join}) Drop(*n);
        for (ux::Node& n : s.rim) Drop(n);
    }
    for (auto& s : gSlots) {
        DropShine(s.slotShine);
    }
    gSlots.clear();
    for (auto& fx : gFx) DropFx(fx);
    gFxRow = -1;
    Obj tree = gScreen.Tree(), canvas = gScreen.Canvas();
    for (const std::string& slot : slots) {
        SlotTab t;
        t.slot = slot;
        t.rim[0] = ux::MakeBox(tree, canvas, Color{0, 0, 0, 0}, 6, 0, 0, 6, 9);          // (only its outline drawn)
        for (int k = 1; k < 3; ++k) {
            t.rim[k] = ux::MakePicture(tree, canvas, 0, 9);
            t.rim[k].Picture(ux::Picture("pm-fade-left.png"));
        }
        t.nameBase = ux::MakeBox(tree, canvas, kDark, 6, 0, 0, 6, 10);
        t.nameBubble = ux::MakeBubble(tree, canvas, 6, 0, 0, 6, 11);
        t.nameText = ux::MakeText(tree, canvas, packages::LabelOf(slot), kFullFont, kGrey, 12);
        t.slotShine = MakeShine(tree, canvas);
        t.outer = ux::MakeBox(tree, canvas, kBlack, 14, 14, 14, 14, 14);
        t.ring = ux::MakeBox(tree, canvas, kRing, 12, 12, 12, 12, 15);
        t.join = ux::MakeBox(tree, canvas, kLime, 0, 0, 0, 0, 15);      // (made after the ring: drawn over it)
        t.back = ux::MakeBox(tree, canvas, kSlotBack, 9, 9, 9, 9, 16);
        t.pic = ux::MakePicture(tree, canvas, 0, 17);
        t.hit = ux::MakeHit(tree, canvas, 18);
        for (const auto& [known, width] : gNameWidths)
            if (known == slot) t.textWidth = width;
        gSlots.push_back(std::move(t));
    }
    gWiggleSlot = -1;
}

// --- the hover effect ---------------------------------------------------------------------------------------------------
void BuildFx(HoverFx& fx) {
    Obj tree = gScreen.Tree(), canvas = gScreen.Canvas();
    // the glow, smoke, thread and wash under the tabs (the tabs cover them); the sparks over the name tabs
    for (ux::Node* n : {&fx.glow, &fx.smokeA, &fx.smokeB, &fx.thread, &fx.wash}) *n = ux::MakePicture(tree, canvas, 0, 5);
    fx.glow.Picture(ux::Picture("fx-glow.png"));
    fx.thread.Picture(ux::Picture("fx-thread.png"));
    fx.wash.Picture(ux::Picture("fx-wash.png"));
    for (int i = 0; i < kFxSparks; ++i) {
        fx.puffs.push_back(ux::MakePicture(tree, canvas, 0, 38));
        fx.puffs.back().Picture(ux::Picture("fx-puff.png"));
        fx.sparks.push_back(ux::MakePicture(tree, canvas, 0, 39));
        fx.sparks.back().Picture(ux::Picture("fx-spark.png"));
    }
    for (ux::Node* n : {&fx.glow, &fx.smokeA, &fx.smokeB, &fx.thread, &fx.wash}) n->Opacity(0);
    for (auto& n : fx.sparks) n.Opacity(0);
    for (auto& n : fx.puffs) n.Opacity(0);
}

// A new row hovered: its effect starts, with fresh paths for its sparks.
void StartFx(int row, double now) {
    HoverFx& fx = gFx[gFxNext];
    gFxNext = 1 - gFxNext;
    gFx[gFxNext].endAt = std::min(gFx[gFxNext].endAt, now);   // the last row's (reported: it played on under the new one)
    if (!fx.glow.Get()) BuildFx(fx);
    fx.row = row;
    fx.at = now;
    fx.endAt = 1e9;
    fx.plan.clear();
    auto u = [](double a, double b) { return std::uniform_real_distribution<double>(a, b)(gFxRandom); };
    for (int i = 0; i < kFxSparks; ++i) {
        Spark s{};
        s.edge = u(0, 1) < 0.6;
        s.t0 = i * 0.045 + u(0, 0.03);
        s.life = u(0.9, 1.25);
        s.startIn = u(0, 18);                      // edge sparks: this far left of the slot
        s.side = u(0, 1) < 0.5 ? -1 : 1;           // over the tab's top or under its bottom
        s.jitter = s.edge ? u(-2, 2) : u(-6, 6);
        s.along = u(30, 95);                       // edge sparks: this far past the tab's end
        s.dist = u(55, 100);                       // end sparks: this far from the tab's end
        s.amp = u(3, 7) * (u(0, 1) < 0.5 ? -1 : 1);
        s.f = u(0.9, 1.6);
        s.ph = u(0, 6.28);
        s.drift = u(0.1, 0.4);
        s.r = u(0.8, 1.25);
        // each its own: most gone soon, very few seen the whole way (Lvcid: they all faded out alike)
        s.shown = 0.3 + 0.7 * std::pow(u(0, 1), 4);
        s.whiteFor = u(14, 34);
        fx.plan.push_back(s);
    }
}

// A row's effect this frame: `slotLeft` the slot's outer left edge, the name tab's left end, top and height; `hover`
// the row's (the smoke follows it).
void UpdateFx(HoverFx& fx, double slotLeft, double tabLeft, double cy, double tabTop, double tabH, double hover, double now) {
    const double since = now - fx.at;
    const double fade = 1 - ux::Smooth((now - fx.endAt) / 0.18);      // a new row hovered: this one goes, quickly
    const double shimmer = since > 0 ? fade * ux::Smooth(since / 0.35) * std::exp(-std::max(0.0, since - 0.35) / 0.9) : 0;
    // the thread: a quick snipe (Lvcid: the slow run was too much), lit for a moment as its wash shoots out
    const double snipe = since > 0 ? fade * ux::Smooth(since / 0.04) * std::exp(-std::max(0.0, since - 0.04) / 0.14) : 0;
    const double tabLen = slotLeft - tabLeft;
    const double rx = tabLen + 76, ry = 0.9 * tabH * (1 + 0.06 * std::sin(now * 2.1));
    fx.glow.Box(slotLeft - rx, cy - ry, rx, 2 * ry);
    fx.glow.Tint(kLime);
    fx.glow.Opacity(static_cast<float>(0.22 * shimmer));
    // the smoke: 32 frames, each cross-faded into the next
    const double p = std::fmod(now / kSmokeLoop, 1.0) * kSmokeFrames;
    const int f0 = static_cast<int>(p) % kSmokeFrames;
    const double frac = p - std::floor(p);
    char name[32];
    std::snprintf(name, sizeof name, "fx-smoke%02d.png", f0);
    fx.smokeA.Picture(ux::Picture(name));
    std::snprintf(name, sizeof name, "fx-smoke%02d.png", (f0 + 1) % kSmokeFrames);
    fx.smokeB.Picture(ux::Picture(name));
    for (ux::Node* n : {&fx.smokeA, &fx.smokeB}) {
        n->Box(slotLeft - rx * 1.19, cy - ry * 1.48, rx * 1.19, ry * 2.96);
        n->Tint(kLime);
    }
    fx.smokeA.Opacity(static_cast<float>(0.34 * hover * (1 - frac)));
    fx.smokeB.Opacity(static_cast<float>(0.34 * hover * frac));
    // the thread, and the one wash sweeping out along it
    fx.thread.Box(tabLeft - 113, cy - 12, tabLen + 113, 24);
    fx.thread.Tint(kWhite);
    fx.thread.Opacity(static_cast<float>(0.12 * snipe));     // very faint (Lvcid: the laser was too much)
    const double run = since / 0.22;
    const double washX = slotLeft - run * (tabLen + 113) * 1.15;
    fx.wash.Box(washX - 75, cy - 20, 150, 40);
    fx.wash.Tint(kWhite);
    fx.wash.Opacity(run > 0 && run < 1 ? static_cast<float>(0.15 * fade * std::sin(3.14159265 * run)) : 0.0f);
    // the sparks
    for (size_t i = 0; i < fx.plan.size() && i < fx.sparks.size(); ++i) {
        const Spark& s = fx.plan[i];
        // they move as before but are gone sooner (Lvcid: the speed's right, they lasted too long)
        const double moved = (since - s.t0) / s.life, age = moved / s.shown;
        if (age <= 0 || age >= 1) {
            fx.sparks[i].Opacity(0);
            fx.puffs[i].Opacity(0);
            continue;
        }
        const double sx = s.edge ? slotLeft - 2 - s.startIn : tabLeft + 4;
        const double sy = s.edge ? (s.side < 0 ? tabTop - 2 : tabTop + tabH + 2) + s.jitter : cy + s.jitter;
        const double dist = s.edge ? tabLen + s.along : s.dist;
        const double go = (1 - std::exp(-3.2 * moved)) / (1 - std::exp(-3.2));   // a quicker start, the same run
        const double px = sx - dist * go;
        const double py = sy + s.amp * std::min(1.0, go * 4) * std::sin(s.ph + go * 6.28318 * s.f) + (cy - sy) * s.drift * go;
        // its brightness over its life, as Lvcid drew it: a soft rise, a short peak, a long steady fall, a short faint tail
        const double shape = age < 0.28 ? ux::Smooth(age / 0.15)
                             : age < 0.75 ? 1 - 0.88 * ux::Smooth((age - 0.28) / 0.47)
                                          : 0.12 * (1 - ux::Smooth((age - 0.75) / 0.25));
        const double life = fade * shape * 0.9;
        const double size = 32 * s.r * (1 - 0.5 * age);          // the picture: a core of radius 1 in 32
        fx.sparks[i].Box(px - size / 2, py - size / 2, size, size);
        // white near the (white) name tab, turning lime the further from it it gets, and with its age, each its own way
        const double dx = std::max({tabLeft - px, 0.0, px - slotLeft}), dy = std::max({tabTop - py, 0.0, py - (tabTop + tabH)});
        const double away = std::hypot(dx, dy);
        fx.sparks[i].Tint(ux::Mix(kWhite, kPale, ux::Smooth(0.65 * away / s.whiteFor + 0.35 * age)));
        fx.sparks[i].Opacity(static_cast<float>(life));
        const double puff = fade * ux::Smooth((age - 0.25) / 0.45) * std::pow(1 - age, 1.1) * 0.6;   // a softer end
        const double puffSize = 2 * ((1.5 + 5.5 * age) * 1.6 + 4);
        fx.puffs[i].Box(px - puffSize / 2, py - puffSize / 2, puffSize, puffSize);
        fx.puffs[i].Tint(kLime);
        fx.puffs[i].Opacity(static_cast<float>(puff * 0.45));
    }
}

void HideFx(HoverFx& fx) {
    for (ux::Node* n : {&fx.glow, &fx.smokeA, &fx.smokeB, &fx.thread, &fx.wash}) n->Opacity(0);
    for (auto& n : fx.sparks) n.Opacity(0);
    for (auto& n : fx.puffs) n.Opacity(0);
}

// --- the panel's theme (a Cosmetic Kit setting: the game's light grey, charcoal, Ballest night) ---------------------
// Only the page's own light surfaces are recoloured, on this page's widgets (never the game's assets, so nothing else in
// the game changes): the grey background (BackgroundPanel*, the MI_Halftone dots: its brush tinted, which multiplies
// the dots' own shade), the section cards (Borders filled D9D9D9 with a faint white edge) and the section headings
// (texts 2B2B2B), wherever they are under the panel, the host's own copies of them included. Found by their game colours,
// each widget remembers its own so the game's look comes back exactly. Tiles, rows, balls, lime and every hover
// animation stay the game's (their colours are dark already, or animated).
struct Themed {
    eng::Weak widget;
    int kind;                                       // 0 the background's tint, 1 a card, 2 a heading
    Color original, originalEdge;
};
std::vector<Themed> gThemed;
int gThemeShown = 0;
double gThemeLooked = -100;
eng::Weak gThemePage;

struct ThemeLook {
    Color panel, card, cardEdge, text;
};
ThemeLook ThemeOf(int theme) {
    if (theme == 2) return {ux::Hex("#1B1822"), ux::Hex("#24212B"), ux::Hex("#FFFFFF", 0.05f), ux::Hex("#E2E0E8")};
    return {ux::Hex("#1A1A1A"), ux::Hex("#222222"), ux::Hex("#FFFFFF", 0.05f), ux::Hex("#E1E2E4")};
}
const Color kGamePanel = ux::Hex("#C4C4C4");       // the background's shade with the game's (white) tint, measured
const Color kGameCard = {0.693872f, 0.693872f, 0.693872f, 1}, kGameHeading = {0.024158f, 0.024158f, 0.024158f, 1};

bool Close(const Color& a, const Color& b, float e = 0.01f) {
    return std::fabs(a.r - b.r) < e && std::fabs(a.g - b.g) < e && std::fabs(a.b - b.b) < e;
}
bool ReadColor(Obj o, std::vector<const char*> path, Color* out) {
    eng::Prop last;
    const int at = o ? eng::NestedOffset(eng::ClassOf(o), path, &last) : -1;
    if (at < 0 || last.size != static_cast<int32_t>(sizeof(Color))) return false;
    std::memcpy(out, o + at, sizeof(Color));
    return true;
}

void ThemeOne(Themed& t, const ThemeLook* look) {        // look null: the game's own back
    Obj o = eng::Get(t.widget);
    if (!o) return;
    if (t.kind == 0) {
        Color tint = t.original;
        if (look) tint = {look->panel.r / kGamePanel.r, look->panel.g / kGamePanel.g, look->panel.b / kGamePanel.b, t.original.a};
        w::WriteSlateColor(o, {"Brush", "TintColor"}, tint);
        Color now{1, 1, 1, 1};
        ReadColor(o, {"ColorAndOpacity"}, &now);
        eng::Call(o, "SetColorAndOpacity", now);          // (redraws it with the new tint)
    } else if (t.kind == 1) {
        w::WriteSlateColor(o, {"Background", "OutlineSettings", "Color"}, look ? look->cardEdge : t.originalEdge);
        eng::Call(o, "SetBrushColor", look ? look->card : t.original);
    } else {
        w::SetTextColor(o, look ? look->text : t.original);
    }
}

// Every widget under `w` (panels' children, borders' content), to `f`.
template <class F>
void WalkWidgets(Obj widget, F&& f, int depth = 0) {
    if (!widget || depth > 14) return;
    f(widget);
    const int32_t n = eng::FindFunction(eng::ClassOf(widget), "GetChildrenCount") ? eng::Call(widget, "GetChildrenCount").ReturnAs<int32_t>(0) : 0;
    for (int32_t i = 0; i < n && i < 300; ++i) WalkWidgets(eng::Call(widget, "GetChildAt", i).ReturnObj(), f, depth + 1);
    if (n == 0 && eng::FindFunction(eng::ClassOf(widget), "GetContent")) WalkWidgets(eng::Call(widget, "GetContent").ReturnObj(), f, depth + 1);
}

void ApplyTheme(Obj page, double now) {
    const int theme = packages::Config().theme;
    if (eng::Get(gThemePage) != page) {             // a new page (map change): what was themed went with the old one
        gThemed.clear();
        gThemePage = eng::MakeWeak(page);
        gThemeShown = 0;
    }
    if (theme != gThemeShown) {
        const ThemeLook look = ThemeOf(theme);
        for (auto& t : gThemed) ThemeOne(t, theme ? &look : nullptr);
        if (!theme) gThemed.clear();
        gThemeShown = theme;
        gThemeLooked = -100;
    }
    if (!theme || now - gThemeLooked < 0.5) return;   // twice a second: new sections and the host's copies of them
    gThemeLooked = now;
    const ThemeLook look = ThemeOf(theme);
    Obj image = eng::FindClass("Image"), border = eng::FindClass("Border"), text = eng::FindClass("TextBlock");
    auto known = [](Obj o) {
        return std::any_of(gThemed.begin(), gThemed.end(), [&](const Themed& t) { return eng::Get(t.widget) == o; });
    };
    WalkWidgets(Panel(page), [&](Obj o) {
        if (known(o)) return;
        Themed t{eng::MakeWeak(o), -1, {}, {}};
        if (eng::IsA(o, image) && eng::ObjName(o).rfind("BackgroundPanel", 0) == 0) {
            if (!ReadColor(o, {"Brush", "TintColor", "SpecifiedColor"}, &t.original)) return;
            t.kind = 0;
        } else if (eng::IsA(o, border)) {
            if (!ReadColor(o, {"BrushColor"}, &t.original) || !Close(t.original, kGameCard)) return;
            ReadColor(o, {"Background", "OutlineSettings", "Color", "SpecifiedColor"}, &t.originalEdge);
            t.kind = 1;
        } else if (eng::IsA(o, text)) {
            if (!ReadColor(o, {"ColorAndOpacity", "SpecifiedColor"}, &t.original) || !Close(t.original, kGameHeading, 0.004f)) return;
            t.kind = 2;
        } else {
            return;
        }
        gThemed.push_back(t);
        ThemeOne(gThemed.back(), &look);
    });
    gThemed.erase(std::remove_if(gThemed.begin(), gThemed.end(), [](const Themed& t) { return !eng::Get(t.widget); }), gThemed.end());
}

// The slot tabs this frame; returns the bottom of the last one.
double UpdateSlots(const ux::Rect& tab, bool local, double dt, double now) {
    const std::vector<std::string> slots = packages::ShownSlots(local);
    const int style = packages::Config().labelStyle;
    std::string key = std::to_string(style);
    for (const auto& s : slots) key += "|" + s;
    if (key != gSlotsKey) {
        BuildSlots(slots);
        gSlotsKey = key;
        // (the slot shown went away (Cosmetic Kit Plus turned off, say): back to balls, not a list with no tab)
        const std::string shown = cosmetics::ShownSlot();
        if (std::find(slots.begin(), slots.end(), shown) == slots.end()) cosmetics::ShowSlot("ball");
    }
    const Looks L = LooksFor(style);
    const double right = tab.Right() - 2, top0 = tab.y;
    const size_t n = gSlots.size();
    std::vector<double> centers(n);
    for (size_t i = 0; i < n; ++i) centers[i] = top0 + kBase / 2 + static_cast<double>(i) * (kBase + kGap);
    const double columnBottom = n ? centers.back() + kBase / 2 + kGrow : top0;
    double mx = 0, my = 0;
    const bool mouse = !manager::IsOpen() && ux::Mouse(&mx, &my);      // nothing hovers under the plugin manager
    const bool inside = mouse && mx >= right - 230 && mx <= right + 10 && my >= top0 - kGap && my <= columnBottom + kGap;
    if (inside && !gInside) gArmed = true;         // the text wiggles on the first hover after the pointer comes in
    gInside = inside;
    if (inside) gPy = my;
    gG = ux::Approach(gG, inside ? 1.0 : 0.0, dt, kEase);
    // The row the pointer is on (each row is a slot and half the gaps around it) is the hovered one: it settles to fully
    // hovered over time and the others settle back, wherever on the row the pointer rests. (Hover by distance alone left
    // a slot half hovered when the pointer stopped off its middle: reported.) Neighbours still swell a little by
    // distance, dock-style.
    int row = -1;
    if (inside && n) row = std::clamp(static_cast<int>(std::floor((gPy - top0 + kGap / 2) / (kBase + kGap))), 0, static_cast<int>(n) - 1);
    std::vector<double> hover(n), size(n);
    std::vector<bool> fading(n, false);             // the white going (the lime bubble shows through it as it goes)
    for (size_t i = 0; i < n; ++i) {
        SlotTab& t = gSlots[i];
        const bool on = static_cast<int>(i) == row;
        t.hover = ux::Approach(t.hover, on ? 1.0 : 0.0, dt, kSlotEase);
        // the open slot's white goes back to lime about as long as the hover effect plays (Lvcid); the others quickly
        const bool open_ = t.slot == cosmetics::ShownSlot();
        t.lit = ux::Approach(t.lit, on ? 1.0 : 0.0, dt, on ? kNameEase : open_ ? 0.3 : 0.16);
        fading[i] = !on && t.lit > 0.001;
        hover[i] = t.lit;
        const double d = std::fabs(centers[i] - gPy);
        // The two names next to the hovered one grow part of the way, the nearer to the pointer the more.
        // Both aim at a few fixed steps and get there over time: following the pointer's exact distance changed the
        // texts' scale a hair every pixel the mouse moved, and their letters shimmered as they snapped (reported).
        auto step = [](double v) { return std::round(v * 4) / 4; };
        const double near = inside && !on ? kNameNear * step(ux::Smooth(1 - d / (1.6 * (kBase + kGap)))) : 0;
        t.grow = ux::Approach(t.grow, on ? 1.0 : near, dt, kNameEase);
        t.swell = ux::Approach(t.swell, inside ? 0.55 * step(ux::Smooth(1 - d / kSlotRange)) : 0.0, dt, kSlotEase);
        size[i] = kBase + kGrow * std::max(t.hover, t.swell * gG);
    }
    if (row != gFxRow) {                           // a new row hovered: its effect starts
        if (row >= 0) StartFx(row, now);
        gFxRow = row;
    }
    for (auto& fx : gFx)
        if (fx.row >= static_cast<int>(n)) fx.row = -1;
    for (auto& fx : gFx)
        if (fx.row < 0 && fx.glow.Get()) HideFx(fx);
    if (gArmed && row >= 0 && hover[static_cast<size_t>(row)] > 0.3) {
        gWiggleSlot = row;
        gWiggleAt = now;
        gArmed = false;
    }
    const std::string open = cosmetics::ShownSlot();
    double y = top0;
    for (size_t i = 0; i < n; ++i) {
        SlotTab& t = gSlots[i];
        const double s = size[i], top = y, cy = top + s / 2, left = right - s, h = hover[i];
        y += s + kGap;
        const bool sel = t.slot == open;
        // the item slot: what is worn (or the slot's silhouette), in a ring: lime and black when open, grey otherwise;
        // white as the pointer comes near
        Obj picture = cosmetics::WornPicture(t.slot);
        if (!picture) picture = Silhouette(t.slot);
        t.pic.Picture(picture);
        // The picture inset in its slot, as the game's own tiles show it (the whole cosmetic, never cut by the corners).
        const double inset = s * 0.1;
        t.pic.Box(left + inset, top + inset, s - 2 * inset, s - 2 * inset);
        t.back.Box(left, top, s, s);
        // The hovered slot's shine, from its outer edge out (drawn for an 80 edge), over its name tab (Lvcid: the slot's only).
        const double edge = sel ? s + 10 : s + 6, edgeLeft = left + s / 2 - edge / 2;
        UpdateShine(t.slotShine, "slot", edgeLeft, top + s / 2 - edge / 2, edge, edge, t.hover, now);
        if (sel) {
            t.outer.Show(true);
            t.outer.Box(left - 5, top - 5, s + 10, s + 10);
            t.outer.Tint(ux::Mix(kBlack, kWhite, h));
            t.ring.Box(left - 3, top - 3, s + 6, s + 6);
            const double flash = std::clamp((now - t.flashAt) / 0.22, 0.0, 1.0);
            const double lime = flash < 1 ? std::sin(3.14159265 * flash) : 0;     // white, lime, white again
            t.ring.Tint(ux::Mix(kLime, kWhite, h * (1 - lime)));
        } else {
            t.outer.Show(false);
            const double ring = 2 + h;
            t.ring.Box(left - ring, top - ring, s + 2 * ring, s + 2 * ring);
            t.ring.Tint(ux::Mix(kRing, kWhite, h));
        }
        // the name: small (sideways in style 1), straightened while the pointer is in the column, full when hovered
        const Look rest = Blend(sel ? L.med : L.small, sel ? L.medIn : L.smallIn, gG);
        const Look k = Blend(rest, L.full, std::max(h, t.grow));       // its size and angle
        const double up = (k.rot + 90) / 90;
        const double measured = t.nameText.TextWidth();
        if (measured > 0) t.textWidth = measured;
        const double full = t.textWidth > 0 ? t.textWidth : 0.62 * kFullFont * static_cast<double>(packages::LabelOf(t.slot).size());
        const double textLen = full * k.font / kFullFont;
        const double along = textLen + 2 * k.pad + kNear;
        const double visW = k.thick + (along - k.thick) * up, bh = along + (k.thick - along) * up;
        const double bw = visW + kOver, x = left + kOver - bw, ytop = cy - bh / 2;
        t.nameBase.Box(x, ytop, bw, bh);
        t.nameBase.Tint(ux::Mix(sel ? kLime : kDark, kWhite, h));      // hovered: plain white (Lvcid, over the bubble)
        t.nameBase.Opacity(1);
        t.nameBubble.Box(x, ytop, bw, bh);
        // round tabs (Lvcid): the left end's corners half the name's height (its right end is under the slot)
        const double corner = std::min(visW, bh) / 2;
        if (std::fabs(corner - t.radius) > 0.2) {
            t.radius = corner;
            ux::RoundBrush(t.nameBase.Get(), "Background", corner, 0, 0, corner);
            ux::RoundBrush(t.nameBubble.Get(), "Brush", corner, 0, 0, corner);
            ux::RoundBrush(t.rim[0].Get(), "Background", corner + 1.6, 0, 0, corner + 1.6);
        }
        // the open slot's name joined to its ring: over the black round the ring, as tall as the name, in its colour
        // (Lvcid: no gap between them)
        if (sel) {
            const double jh = std::min(bh, s - 6);
            t.join.Show(true);
            t.join.Box(left - 6, cy - jh / 2, 7, jh);
            t.join.Tint(ux::Mix(kLime, kWhite, h));
        } else {
            t.join.Show(false);
        }
        // hovered: the plugin manager's lit card edge round the name's left end (Lvcid): a thin lime ring just outside
        // its round end (an outline only: its right side is under the name), and from where the round ends, a thin
        // line along its top and bottom fading out (racing out as the hover comes in). (Stacked filled bands were tried:
        // their square ends showed as dark steps along the edges: reported.)
        {
            constexpr double kRim = 1.6;
            const double lit = ux::Smooth(h), out = ux::Smooth(std::min(1.0, h * 1.6));
            const double reach = std::min(46.0, std::max(0.0, visW - corner - 4)) * out;
            const Color lime{kLime.r, kLime.g, kLime.b, static_cast<float>(0.9 * lit)};
            for (ux::Node& piece : t.rim) piece.Show(lit > 0.01);
            if (lit > 0.01) {
                t.rim[0].Box(x - kRim, ytop - kRim, corner + kRim + 1, bh + 2 * kRim);
                t.rim[0].Outline(static_cast<float>(kRim), lime);
                t.rim[1].Box(x + corner, ytop - kRim, std::max(0.5, reach), kRim);
                t.rim[2].Box(x + corner, ytop + bh, std::max(0.5, reach), kRim);
                t.rim[1].Tint(lime);
                t.rim[2].Tint(lime);
            }
        }
        for (auto& fx : gFx)
            if (fx.row == static_cast<int>(i)) UpdateFx(fx, edgeLeft, x, cy, ytop, bh, t.hover, now);
        // the open slot's name at rest: the bubble button in lime, as the open mode's tab (plain white while hovered)
        // untinted: the game's own lime. The open one's under its white; a hover leaving the others fades back through it
        // (white, then the lime bubble, then their rest: Lvcid)
        const double afterglow = fading[i] ? 4 * h * (1 - h) : 0;
        t.nameBubble.Tint(Color{1, 1, 1, static_cast<float>(sel ? 1 - h : afterglow)});
        t.nameBubble.Outline(0, {1, 1, 1, 0});
        Color fg = ux::Mix(rest.fg, L.full.fg, h);                    // its colours follow the hover only
        if (!sel && h > 0 && h < 1)
            fg = h < 0.5 ? ux::Mix(rest.fg, kLight, h * 2) : ux::Mix(kLight, kBlack, std::min(1.0, (h - 0.5) * 4));
        t.nameText.Tint(fg);
        const double wiggle = static_cast<int>(i) == gWiggleSlot ? ux::Wiggle(now - gWiggleAt) * h : 0;
        t.nameText.Center(x + (visW - kNear * up) / 2, cy);
        t.nameText.Transform(k.font / kFullFont, k.rot + wiggle);
        // clicks: the slot's row of the column
        t.hit.Box(right - 230, top - kGap / 2, 230, s + kGap);
        if (t.click.Update(t.hit)) {
            if (sel) t.flashAt = now;
            else cosmetics::ShowSlot(t.slot);
        }
    }
    const double bottom = n ? y - kGap : top0;
    // The column's shade (Lvcid's pick, "soft oval"): darkest just inside the tabs' edge, gone by the screen's left edge,
    // from 5 above the first slot to 6 below the last, so it stretches with the column as slots grow and settle.
    if (!gShadow.Get()) {
        gShadow = ux::MakePicture(gScreen.Tree(), gScreen.Canvas(), 0, 0);
        gShadow.Picture(ux::Picture("tab-shadow.png"));
    }
    const double shadeTop = top0 - 5, shadeBottom = bottom + 6, middle = (shadeTop + shadeBottom) / 2;
    const double reach = (shadeBottom - shadeTop) * 190.0 / 309.0;     // its half height, as the mockup's
    gShadow.Box(tab.Right() - 251, middle - reach, 251, 2 * reach);     // the picture: 235 left of its middle to 16 right
    gShadow.Opacity(n && gShadow.hasPicture == 1 ? 1.0f : 0.0f);
    return bottom;
}

// The custom section's button: the cosmetic browser.
void SectionClicks() {
    const std::string click = cosmetics::TakeSectionClick();
    if (click.empty()) return;
    hostlog::Info("customize: " + click);
    if (click == "browser") {
        OpenBrowser();
    }
}

void TearDown() {
    ShowGameTabs();
    RestoreBack();
    if (gScreen.Ready()) gScreen.Show(false);
}

}  // namespace

void Frame() {
    const double now = game::Seconds();
    const double dt = gLast < 0 ? 0 : std::min(0.1, now - gLast);
    gLast = now;
    if (!packages::KitOn()) {
        TearDown();
        return;
    }
    Obj page = cosmetics::Page();
    if (!page || !cosmetics::PageOnScreen()) {
        TearDown();
        return;
    }
    SectionClicks();
    if (!gScreen.Ready()) {
        ResetAll();
        gScreen.zOrder = 60;
        if (!gScreen.Make()) return;
    }
    gScreen.Show(true);
    HideGameTabs(page);
    const ux::Rect tab = BallsTab(page);
    if (!tab.ok) return;                            // not laid out yet
    if (!gLoggedLayout) {
        gLoggedLayout = true;
        const ux::Rect panel = ux::Bounds(Panel(page));
        double sw = 0, sh = 0;
        ux::ScreenSize(&sw, &sh);
        hostlog::Info("customize: balls tab at " + std::to_string(int(tab.x)) + "," + std::to_string(int(tab.y)) + " " +
                      std::to_string(int(tab.w)) + "x" + std::to_string(int(tab.h)) + ", panel at " + std::to_string(int(panel.x)) + "," +
                      std::to_string(int(panel.y)) + " " + std::to_string(int(panel.w)) + "x" + std::to_string(int(panel.h)) + ", screen " +
                      std::to_string(int(sw)) + "x" + std::to_string(int(sh)) + " units");
    }
    const bool local = cosmetics::LocalMode();
    UpdateStrip(tab, page, dt, now);
    UpdateLoadouts(tab, local, dt, now);
    const double bottom = UpdateSlots(tab, local, dt, now);
    ApplyTheme(page, now);
    if (local) ShiftBack(bottom + kBackGap);
    else ShiftBack(-1e9);
}

}  // namespace customize
