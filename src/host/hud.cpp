#include "hud.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>

#include "cosmetics.hpp"
#include "engine.hpp"
#include "game.hpp"
#include "log.hpp"
#include "race.hpp"
#include "ui.hpp"
#include "widgets.hpp"

using eng::Obj;
namespace w = ui::widgets;

namespace hud {
namespace {

struct Found {
    std::string key, name, className;
    eng::Weak widget;
};
std::vector<Found> gFound;
eng::Weak gRaceUi;
double gNextDiscover = 0, gNextApply = 0;
int gGeneration = -1;

struct Layout {
    double x = 0, y = 0, scale = 1;
    int mode = kNormal;
};
std::map<std::string, Layout> gLayouts;
std::vector<std::string> gToRestore;            // cleared layouts whose element still shows them
std::string gBlink;
bool gEditing = false;

// What the game had, before a layout changed it: kept per widget, and per slot (a widget moved to another slot has
// other originals).
struct Original {
    eng::Weak widget;
    Obj slot = nullptr;
    int kind = 0;                               // 0 canvas position, 1 padding, 2 render translation
    double position[2] = {};
    float padding[4] = {};
    float opacity = 1;
    bool opacityTouched = false, scaleTouched = false, moved = false;
    bool forced = false;
    uint8_t visibility = 0;                     // before being forced visible
};
std::map<int32_t, Original> gOriginals;         // by the widget's object slot

struct Tint {
    std::string key, part;
    eng::Weak widget;
    int type = 0;                               // 0 border, 1 image, 2 text
    float original[4] = {}, applied[4] = {-1, -1, -1, -1};
};
std::vector<Tint> gTints;

constexpr float kOffWhileEditing = 0.25f, kBlinkDim = 0.35f;
constexpr double kBlinkSeconds = 0.3;
constexpr uint8_t kVisibilityCollapsed = 1, kVisibilityHidden = 2, kHitTestInvisible = 3;

struct V2 {
    double x, y;
};

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string ClassShort(Obj o) {
    std::string c = eng::ObjName(eng::ClassOf(o));
    if (c.size() > 2 && c.compare(c.size() - 2, 2, "_C") == 0) c.resize(c.size() - 2);
    return c;
}

bool AllDigits(const std::string& s) { return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos; }

// Names the game gives at runtime (WBP_X_C_2147481234, Widget_12345) change from run to run: no layout can find them.
bool StableName(const std::string& name) {
    const size_t us = name.rfind('_');
    if (us == std::string::npos) return true;
    const std::string tail = name.substr(us + 1);
    if (AllDigits(tail) && tail.size() >= 5) return false;
    return !(AllDigits(tail) && us >= 2 && name.compare(us - 2, 2, "_C") == 0);
}

// Named in the designer: not "<Class>" or "<Class>_<n>".
bool DesignerNamed(Obj o) {
    const std::string name = eng::ObjName(o), cls = ClassShort(o);
    if (name == cls) return false;
    return !(name.size() > cls.size() + 1 && name.compare(0, cls.size() + 1, cls + "_") == 0 && AllDigits(name.substr(cls.size() + 1)));
}

bool SkippedContainer(const std::string& name) {
    const std::string n = Lower(name);
    return name == "Menus_Root" || n.find("modal") != std::string::npos || n.find("tower") != std::string::npos ||
           n.find("ftu") != std::string::npos;
}

bool InList(const std::string& cls, std::initializer_list<const char*> list) {
    for (const char* c : list)
        if (cls == c) return true;
    return false;
}

void Visit(Obj widget, const std::string& prefix, int depth, bool parentIsCanvas, bool parentListed, bool inSkipped) {
    static Obj userClass = eng::FindClass("UserWidget"), panelClass = eng::FindClass("PanelWidget"), canvasClass = eng::FindClass("CanvasPanel");
    if (!widget || depth > 20) return;
    const std::string name = eng::ObjName(widget), cls = eng::ObjName(eng::ClassOf(widget));
    const bool isUser = eng::IsA(widget, userClass), isPanel = eng::IsA(widget, panelClass);
    if (depth > 0 && cls == "WBP_PlayerUI_C") return;               // walked on its own
    if (InList(cls, {"WBP_LeaderboardEntry_C", "WBP_TowerTracker_C"})) return;
    const bool skipped = inSkipped || (isPanel && SkippedContainer(name));
    bool listed = false;
    if (depth > 0 && StableName(name)) {
        if (skipped)
            listed = isUser && InList(cls, {"WBP_Leaderboard_C", "WBP_EditorLeaderboardReplacement_C", "WBP_CheckpointCounter_C",
                                             "WBP_IngameGoalsGroup_C", "WBP_PersonalStanding_C", "WBP_NextGoal_C"});
        else
            listed = parentIsCanvas || isUser || (isPanel && DesignerNamed(widget)) || (parentListed && (isPanel || DesignerNamed(widget)));
    }
    if (listed) {
        const std::string key = prefix + "/" + name;
        if (std::none_of(gFound.begin(), gFound.end(), [&](const Found& f) { return f.key == key; }))
            gFound.push_back({key, name, cls, eng::MakeWeak(widget)});
    }
    if (isUser && depth > 0) return;                                 // a composite moves as a whole
    if (!isPanel) return;
    const int32_t n = eng::Call(widget, "GetChildrenCount").ReturnAs<int32_t>(0);
    const bool canvas = eng::IsA(widget, canvasClass);
    for (int32_t i = 0; i < n && i < 200; ++i)
        Visit(eng::Call(widget, "GetChildAt", i).ReturnObj(), prefix, depth + 1, canvas, listed, skipped);
}

Obj RaceUi() {
    Obj ui = eng::ReadObj(game::PlayerController(), "DONOTACCESS_UseGetter_CachedRaceUIManager");
    return ui && eng::IsLive(ui) ? ui : nullptr;
}

Obj RootOf(Obj userWidget) { return eng::ReadObj(eng::ReadObj(userWidget, "WidgetTree"), "RootWidget"); }

void Discover() {
    gFound.clear();
    Obj raceUi = RaceUi();
    gRaceUi = eng::MakeWeak(raceUi);
    if (!raceUi) return;
    Visit(RootOf(eng::ReadObj(raceUi, "WBP_PlayerUI")), "PlayerUI", 0, false, false, false);
    Visit(RootOf(raceUi), "RaceUI", 0, false, false, false);
    // HUD panels the walk leaves out, as the race UI's own variables (measured: the header, countdown and
    // leaderboard sit in Menus_Root; the medal row is inside the header).
    for (const char* variable : {"WBP_Header", "WBP_IngameGoalsGroup", "WBP_Countdown_1", "WBP_Leaderboard_0"}) {
        Obj widget = eng::ReadObj(raceUi, variable);
        const std::string key = std::string("RaceUI/") + variable;
        if (widget && std::none_of(gFound.begin(), gFound.end(), [&](const Found& f) { return f.key == key; }))
            gFound.push_back({key, variable, eng::ObjName(eng::ClassOf(widget)), eng::MakeWeak(widget)});
    }
}

bool Shown(Obj widget) {
    static Obj userClass = eng::FindClass("UserWidget");
    for (int i = 0; widget && i < 40; ++i) {
        if (!eng::Call(widget, "IsVisible").ReturnBool()) return false;
        Obj parent = eng::Call(widget, "GetParent").ReturnObj();
        if (!parent) {
            Obj owner = eng::OuterOf(eng::OuterOf(widget));          // a tree's root: its user widget
            if (!owner || !eng::IsA(owner, userClass)) return true;
            parent = owner;
        }
        widget = parent;
    }
    return true;
}

Obj WidgetOf(const std::string& key) {
    for (const auto& f : gFound)
        if (f.key == key) return eng::Get(f.widget);
    return nullptr;
}

Original& OriginalOf(Obj widget) {
    const int32_t index = eng::MakeWeak(widget).index;
    auto it = gOriginals.find(index);
    if (it == gOriginals.end() || eng::Get(it->second.widget) != widget) {
        Original o;
        o.widget = eng::MakeWeak(widget);
        o.opacity = eng::Call(widget, "GetRenderOpacity").ReturnAs<float>(1);
        it = gOriginals.insert_or_assign(index, o).first;
    }
    return it->second;
}

void CaptureSlot(Obj widget, Original& o) {
    static Obj canvasSlot = eng::FindClass("CanvasPanelSlot");
    Obj slot = eng::ReadObj(widget, "Slot");
    o.slot = slot;
    o.kind = 2;
    if (slot && eng::IsA(slot, canvasSlot)) {
        struct {
            V2 min, max;
        } anchors = eng::Call(slot, "GetAnchors").ReturnAs<decltype(anchors)>();
        if (std::fabs(anchors.min.x - anchors.max.x) < 0.01 && std::fabs(anchors.min.y - anchors.max.y) < 0.01) {
            const V2 p = eng::Call(slot, "GetPosition").ReturnAs<V2>();
            o.position[0] = p.x;
            o.position[1] = p.y;
            o.kind = 0;
            return;
        }
    }
    if (slot && eng::FindProp(eng::ClassOf(slot), "Padding") && eng::ReadBytes(slot, "Padding", o.padding, sizeof o.padding)) o.kind = 1;
}

V2 RenderTransformPart(Obj widget, const char* part) {
    V2 v{0, 0};
    static Obj widgetClass = eng::FindClass("Widget");
    const int at = eng::NestedOffset(widgetClass, {"RenderTransform", part});
    if (at >= 0) std::memcpy(&v, widget + at, sizeof v);
    return v;
}

bool Near(double a, double b, double tolerance = 0.01) { return std::fabs(a - b) < tolerance; }

void Move(Obj widget, Original& o, double x, double y) {
    Obj slot = eng::ReadObj(widget, "Slot");
    if (slot != o.slot) CaptureSlot(widget, o);
    if (o.kind == 0) {
        const V2 now = eng::Call(slot, "GetPosition").ReturnAs<V2>();
        const V2 want{o.position[0] + x, o.position[1] + y};
        if (!Near(now.x, want.x) || !Near(now.y, want.y)) eng::Call(slot, "SetPosition", want);
    } else if (o.kind == 1) {
        float now[4] = {};
        eng::ReadBytes(slot, "Padding", now, sizeof now);
        const float want[4] = {static_cast<float>(o.padding[0] + x), static_cast<float>(o.padding[1] + y), static_cast<float>(o.padding[2] - x),
                               static_cast<float>(o.padding[3] - y)};
        if (std::memcmp(now, want, sizeof now) != 0) {
            eng::Params p(eng::FunctionOn(slot, "SetPadding"));
            p.SetArg(0, want, sizeof want);
            eng::Invoke(slot, p);
        }
    } else {
        const V2 now = RenderTransformPart(widget, "Translation");
        if (!Near(now.x, x) || !Near(now.y, y)) eng::Call(widget, "SetRenderTranslation", V2{x, y});
    }
    o.moved = x != 0 || y != 0;
}

void SetOpacity(Obj widget, float opacity) {
    if (!Near(eng::Call(widget, "GetRenderOpacity").ReturnAs<float>(1), opacity, 0.001)) eng::Call(widget, "SetRenderOpacity", opacity);
}

void Place(Obj widget, const Layout& l, bool blink) {
    Original& o = OriginalOf(widget);
    if (l.x != 0 || l.y != 0 || o.moved) Move(widget, o, l.x, l.y);
    if (l.scale != 1 || o.scaleTouched) {
        const V2 now = RenderTransformPart(widget, "Scale");
        if (!Near(now.x, l.scale, 0.001) || !Near(now.y, l.scale, 0.001)) eng::Call(widget, "SetRenderScale", V2{l.scale, l.scale});
        o.scaleTouched = l.scale != 1;
    }
    const bool dimmed = l.mode == kOff || blink;
    if (dimmed || o.opacityTouched) {
        float opacity = l.mode == kOff ? (gEditing ? kOffWhileEditing : 0.0f) : o.opacity;
        if (blink && std::fmod(game::Seconds(), 2 * kBlinkSeconds) >= kBlinkSeconds) opacity = (l.mode == kOff ? kOffWhileEditing : o.opacity) * kBlinkDim;
        SetOpacity(widget, opacity);
        o.opacityTouched = dimmed;
    }
    const bool force = l.mode == kOn || blink;
    const uint8_t visibility = eng::Call(widget, "GetVisibility").ReturnAs<uint8_t>(0);
    if (force && (visibility == kVisibilityCollapsed || visibility == kVisibilityHidden)) {
        if (!o.forced) o.visibility = visibility;
        o.forced = true;
        w::SetVisibility(widget, kHitTestInvisible);
    } else if (!force && o.forced) {
        if (visibility == kHitTestInvisible) w::SetVisibility(widget, o.visibility);
        o.forced = false;
    }
}

// A border's colour is its brush's tint (the input display's keys are a 20% grey rounded box, measured from its
// blueprint, whose SetTint changes that tint): the brush goes back through SetBrush with the tint changed.
bool BorderTint(Obj border, const float* color, float* previous) {
    const eng::Prop background = eng::FindProp(eng::ClassOf(border), "Background");
    const int tint = eng::NestedOffset(eng::ClassOf(border), {"Background", "TintColor", "SpecifiedColor"});
    const int rule = eng::NestedOffset(eng::ClassOf(border), {"Background", "TintColor", "ColorUseRule"});
    if (!background || tint < 0 || rule < 0) return false;
    std::vector<uint8_t> brush(static_cast<size_t>(background.size));
    if (!eng::ReadBytes(border, "Background", brush.data(), brush.size())) return false;
    if (previous) std::memcpy(previous, brush.data() + (tint - background.offset), 16);
    std::memcpy(brush.data() + (tint - background.offset), color, 16);
    brush[static_cast<size_t>(rule - background.offset)] = 0;          // ESlateColorStylingMode::UseColor_Specified
    eng::Params set(eng::FunctionOn(border, "SetBrush"));
    return set.SetArg(0, brush.data(), brush.size()) && eng::Invoke(border, set);
}

void Restore(Obj widget) {
    Original& o = OriginalOf(widget);
    Place(widget, Layout{}, false);
    if (o.opacityTouched) SetOpacity(widget, o.opacity);
    o.opacityTouched = o.scaleTouched = o.moved = false;
}

}  // namespace

void HideGameFrame();

void Frame() {
    if (gGeneration != game::Generation()) {        // a new map: everything found is gone with the old one
        gGeneration = game::Generation();
        gFound.clear();
        gOriginals.clear();
        gTints.clear();
        gRaceUi = {};
        gNextDiscover = 0;
    }
    HideGameFrame();
    const double now = game::Seconds();
    if (race::OnTrack() && (now >= gNextDiscover || (!eng::Get(gRaceUi) && RaceUi()))) {
        gNextDiscover = now + 1;
        Discover();
    }
    if (now < gNextApply) return;
    gNextApply = now + 0.05;
    for (const auto& key : gToRestore)
        if (Obj widget = WidgetOf(key)) Restore(widget);
    gToRestore.clear();
    for (const auto& [key, layout] : gLayouts)
        if (Obj widget = WidgetOf(key)) Place(widget, layout, key == gBlink);
    if (!gBlink.empty() && !gLayouts.count(gBlink))
        if (Obj widget = WidgetOf(gBlink)) Place(widget, Layout{}, true);
    // Plugin windows.
    for (auto& hw : ui::HudWindows()) {
        auto it = gLayouts.find(hw.key);
        const Layout l = it == gLayouts.end() ? Layout{} : it->second;
        float opacity = l.mode == kOff ? (gEditing ? kOffWhileEditing : 0.0f) : 1.0f;
        if (hw.key == gBlink && std::fmod(now, 2 * kBlinkSeconds) >= kBlinkSeconds) opacity = (l.mode == kOff ? kOffWhileEditing : 1.0f) * kBlinkDim;
        hw.window->hudX = static_cast<float>(l.x);
        hw.window->hudY = static_cast<float>(l.y);
        hw.window->hudScale = static_cast<float>(l.scale);
        hw.window->hudOpacity = opacity;
    }
}

std::vector<Element> Elements() {
    std::vector<Element> out;
    if (!race::OnTrack()) return out;
    for (const auto& f : gFound) {
        Obj widget = eng::Get(f.widget);
        if (!widget) continue;
        Element e{f.key, f.name, f.className, "", Shown(widget), false};
        Obj parent = eng::Call(widget, "GetParent").ReturnObj();
        e.parentShown = e.shown || !parent || Shown(parent);
        e.opacity = eng::Call(widget, "GetRenderOpacity").ReturnAs<float>(1);
        out.push_back(e);
    }
    for (const auto& hw : ui::HudWindows()) out.push_back({hw.key, hw.key, "Window", hw.label, true, true, hw.window ? hw.window->hudOpacity : 1});
    return out;
}

void SetLayout(const std::string& key, double x, double y, double scale, int mode) {
    gLayouts[key] = Layout{x, y, std::clamp(scale, 0.1, 10.0), mode};
    gToRestore.erase(std::remove(gToRestore.begin(), gToRestore.end(), key), gToRestore.end());
}

void ClearLayout(const std::string& key) {
    if (gLayouts.erase(key)) gToRestore.push_back(key);
}

void SetEditing(bool on) { gEditing = on; }

void SetBlink(const std::string& key) {
    if (key == gBlink) return;
    if (!gBlink.empty() && !gLayouts.count(gBlink)) gToRestore.push_back(gBlink);
    gBlink = key;
}

bool SetPartColor(const std::string& key, const std::string& part, float r, float g, float b, float a) {
    Obj widget = WidgetOf(key);
    Obj p = widget ? eng::ReadObj(widget, part) : nullptr;
    if (!p) return false;
    auto it = std::find_if(gTints.begin(), gTints.end(), [&](const Tint& t) { return t.key == key && t.part == part && eng::Get(t.widget) == p; });
    if (it == gTints.end()) {
        Tint t;
        t.key = key;
        t.part = part;
        t.widget = eng::MakeWeak(p);
        static Obj border = eng::FindClass("Border"), image = eng::FindClass("Image"), text = eng::FindClass("TextBlock");
        if (eng::IsA(p, border)) {
            t.type = 0;
            const int tint = eng::NestedOffset(eng::ClassOf(p), {"Background", "TintColor", "SpecifiedColor"});
            if (tint < 0) return false;
            std::memcpy(t.original, p + tint, sizeof t.original);
        } else if (eng::IsA(p, image)) {
            t.type = 1;
            eng::ReadBytes(p, "ColorAndOpacity", t.original, sizeof t.original);
        } else if (eng::IsA(p, text)) {
            t.type = 2;
            const ui::Color c = w::TextColor(p);
            t.original[0] = c.r, t.original[1] = c.g, t.original[2] = c.b, t.original[3] = c.a;
        } else {
            return false;
        }
        gTints.push_back(t);
        it = gTints.end() - 1;
    }
    const float want[4] = {r, g, b, a};
    if (std::memcmp(it->applied, want, sizeof want) == 0) return true;
    std::memcpy(it->applied, want, sizeof want);
    if (it->type == 0) BorderTint(p, want, nullptr);
    else if (it->type == 1) eng::Call(p, "SetColorAndOpacity", ui::Color{r, g, b, a});
    else w::SetTextColor(p, ui::Color{r, g, b, a});
    return true;
}

void ResetPartColor(const std::string& key, const std::string& part) {
    for (auto it = gTints.begin(); it != gTints.end(); ++it) {
        if (it->key != key || it->part != part) continue;
        if (Obj p = eng::Get(it->widget)) {
            const ui::Color c{it->original[0], it->original[1], it->original[2], it->original[3]};
            if (it->type == 0) BorderTint(p, it->original, nullptr);
            else if (it->type == 1) eng::Call(p, "SetColorAndOpacity", c);
            else w::SetTextColor(p, c);
        }
        gTints.erase(it);
        return;
    }
}

namespace {
std::set<int> gHiders;
// The game's race UI is hidden part by part, all but the footer (the plugin manager's footer lives in it, reported
// missing when the whole UI was hidden): every widget beside the footer's line of parents is hidden, and each one's
// own visibility kept to give back.
struct HiddenPart {
    eng::Weak widget;
    uint8_t visibility = 0;
};
std::vector<HiddenPart> gHiddenParts;
eng::Weak gHiddenIn;                    // the race UI they are in
double gNextHide = 0;
constexpr uint8_t kHidden = 2;          // ESlateVisibility::Hidden

void RestoreGame() {
    for (const auto& part : gHiddenParts)
        if (Obj widget = eng::Get(part.widget)) w::SetVisibility(widget, part.visibility);
    gHiddenParts.clear();
    gHiddenIn = {};
}

void HidePart(Obj widget) {
    for (const auto& part : gHiddenParts)
        if (eng::Get(part.widget) == widget) {
            w::SetVisibility(widget, kHidden);          // the game may have shown it again
            return;
        }
    gHiddenParts.push_back({eng::MakeWeak(widget), eng::Call(widget, "GetVisibility").ReturnAs<uint8_t>(0)});
    w::SetVisibility(widget, kHidden);
}

// Every panel from the footer up to the race UI's root, the footer first. A user widget's root has no parent: the
// line goes on from the user widget holding that tree.
std::vector<Obj> FooterLine(Obj raceUi) {
    std::vector<Obj> line;
    Obj footer = w::FindFirst(raceUi, eng::FindClass("WBP_Footer_C"));
    for (Obj x = footer; x && x != raceUi && line.size() < 64;) {
        line.push_back(x);
        Obj parent = eng::Call(x, "GetParent").ReturnObj();
        if (!parent) {
            Obj tree = eng::OuterOf(x);
            parent = tree ? eng::OuterOf(tree) : nullptr;       // the user widget whose tree this is
        }
        x = parent;
    }
    return line;
}

// The player's own ball, hidden for plugins that asked (Race::HideBall): its actor, the game's special skin actor and
// the host's cosmetic models on it, each one's own hidden state kept to give back.
std::set<int> gBallHiders;
struct HiddenActor {
    eng::Weak actor;
    bool wasHidden = false;
};
std::vector<HiddenActor> gHiddenActors;
double gNextBallHide = 0, gNextBallScan = 0, gNextAttachScan = 0;
eng::Weak gBall;                        // the player's ball, once found

void RestoreBall() {
    for (const auto& h : gHiddenActors)
        if (Obj a = eng::Get(h.actor)) eng::Call(a, "SetActorHiddenInGame", static_cast<uint8_t>(h.wasHidden));
    gHiddenActors.clear();
}

void HideActor(Obj actor) {
    for (const auto& h : gHiddenActors)
        if (eng::Get(h.actor) == actor) {
            eng::Call(actor, "SetActorHiddenInGame", uint8_t{1});     // shown again since (a cosmetic rebuilt, say)
            return;
        }
    bool hidden = false;
    eng::ReadBool(actor, "bHidden", &hidden);
    gHiddenActors.push_back({eng::MakeWeak(actor), hidden});
    hostlog::Info("hud: hid " + eng::ObjName(actor) + ", with the player's ball");
    eng::Call(actor, "SetActorHiddenInGame", uint8_t{1});
}

void HideBallFrame() {
    if (gBallHiders.empty()) return RestoreBall();
    const double now = game::Seconds();
    if (now < gNextBallHide) return;
    gNextBallHide = now + 0.25;
    // The player's ball is the map's BP_RollingBall_C, not the controller's pawn: that can be the fly-over camera
    // (measured: BP_CameraFlyOver_C in the ghost viewer). Found by a scan of every object, so kept and looked for
    // again only when it's gone, at most every 2 s.
    Obj pawn = race::PlayedBallActor();     // the controlled ball first (the editor's own unused ball is one too)
    if (pawn && pawn != eng::Get(gBall)) gBall = eng::MakeWeak(pawn);
    if (!pawn) pawn = eng::Get(gBall);
    if (!pawn && now >= gNextBallScan) {
        gNextBallScan = now + 2;
        if (Obj cls = eng::FindClass("BP_RollingBall_C"))
            eng::ForEachObject([&](Obj o) {
                if (eng::IsDefaultObject(o) || eng::ClassOf(o) != cls || !eng::IsLive(o)) return true;
                pawn = o;
                return false;
            });
        gBall = eng::MakeWeak(pawn);
    }
    if (!pawn) return;
    // The ball, the game's actor for special skins (CustomSkinChild), and models the host built on it (cosmetics).
    HideActor(pawn);
    if (Obj skin = eng::ReadObj(pawn, "CustomSkinChild")) HideActor(skin);
    for (Obj a : cosmetics::ModelActorsOn(pawn)) HideActor(a);
    if (now >= gNextAttachScan) {           // actors the game attached to it (its skin actor, BP_LBall05_C)
        gNextAttachScan = now + 2;
        static Obj actorCls = eng::FindClass("Actor");
        std::vector<Obj> attached;
        if (actorCls)
            eng::ForEachObject([&](Obj o) {
                if (eng::IsDefaultObject(o) || !eng::IsLive(o) || !eng::IsA(o, actorCls)) return true;
                if (eng::Call(o, "GetAttachParentActor").ReturnObj() == pawn) attached.push_back(o);
                return true;
            });
        for (Obj a : attached) HideActor(a);
    }
}

// The player's ball frozen for plugins that asked (Race::FreezeBall): its input off (APawn::DisableInput) and its body
// out of the physics (SetSimulatePhysics false), so it can't roll, jump, fall or respawn. Given back as it was: input,
// physics and both velocities.
struct V3 {
    double x = 0, y = 0, z = 0;
};
std::set<int> gBallFreezers;
struct FrozenBall {
    eng::Weak ball, root, controller;
    bool wasSimulating = false, inputOff = false;
    V3 lin, ang;
};
std::optional<FrozenBall> gFrozen;
double gNextFreeze = 0;
const uint8_t kNoBoneName[8] = {};

void Thaw() {
    if (!gFrozen) return;
    Obj ball = eng::Get(gFrozen->ball);
    Obj root = eng::Get(gFrozen->root);
    if (ball && gFrozen->inputOff) eng::Call(ball, "EnableInput", eng::Get(gFrozen->controller));
    if (root && gFrozen->wasSimulating) {
        eng::Call(root, "SetSimulatePhysics", uint8_t{1});
        eng::Call(root, "SetPhysicsLinearVelocity", gFrozen->lin, uint8_t{0}, kNoBoneName);
        eng::Call(root, "SetPhysicsAngularVelocityInRadians", gFrozen->ang, uint8_t{0}, kNoBoneName);
    }
    if (ball) hostlog::Info("hud: player's ball " + eng::ObjName(ball) + " given back (input, physics)");
    gFrozen.reset();
}

// A run that can reach a leaderboard: racing, not practice, not the track editor's test run. The ball is never frozen
// in one (as Race::SetPaused refuses to pause one).
bool CountingRun() { return race::Active() && !race::Practice() && !race::EditorTesting(); }
bool gFreezeRefused = false;

void FreezeBallFrame() {
    if (gBallFreezers.empty()) return Thaw();
    const double now = game::Seconds();
    if (now < gNextFreeze) return;
    gNextFreeze = now + 0.1;
    if (CountingRun()) {
        if (gFrozen) {
            // frozen before this run started counting: given back, and the run kept off the leaderboards
            race::TaintRun("the ball was frozen by a plugin");
            Thaw();
        }
        if (!gFreezeRefused) hostlog::Warn("hud: not freezing the ball in a run that counts");
        gFreezeRefused = true;
        return;
    }
    gFreezeRefused = false;
    Obj ball = race::PlayedBallActor();
    if (!ball) return;
    if (race::Active()) race::TaintRun("the ball was frozen by a plugin");     // (practice or a test run: never uploads anyway)
    if (gFrozen && eng::Get(gFrozen->ball) != ball) Thaw();      // another ball (a new map or run): the old one back first
    Obj root = eng::Call(ball, "K2_GetRootComponent").ReturnObj();
    if (!root) return;
    const bool simulating = eng::Call(root, "IsSimulatingPhysics", kNoBoneName).ReturnBool();
    Obj controller = game::PlayerController();
    if (!gFrozen) {
        FrozenBall f;
        f.ball = eng::MakeWeak(ball);
        f.root = eng::MakeWeak(root);
        f.controller = eng::MakeWeak(controller);
        gFrozen = f;
        hostlog::Info("hud: froze the player's ball " + eng::ObjName(ball));
    }
    if (simulating) {                       // (again after a test-play restart turns its physics back on)
        gFrozen->wasSimulating = true;
        gFrozen->lin = eng::Call(root, "GetPhysicsLinearVelocity", kNoBoneName).ReturnAs<V3>();
        gFrozen->ang = eng::Call(root, "GetPhysicsAngularVelocityInRadians", kNoBoneName).ReturnAs<V3>();
        eng::Call(root, "SetSimulatePhysics", uint8_t{0});
    }
    if (controller && !gFrozen->inputOff) {
        eng::Call(ball, "DisableInput", controller);
        gFrozen->inputOff = true;
        gFrozen->controller = eng::MakeWeak(controller);
    }
}
}  // namespace

void FreezeBall(int owner, bool frozen) {
    if (frozen) gBallFreezers.insert(owner);
    else gBallFreezers.erase(owner);
    if (gBallFreezers.empty()) Thaw();
    gNextFreeze = 0;
}

void HideBall(int owner, bool hidden) {
    if (hidden) gBallHiders.insert(owner);
    else gBallHiders.erase(owner);
    if (gBallHiders.empty()) RestoreBall();
    gNextBallHide = 0;
}

eng::Weak gEditorPlayerUi;
double gNextPlayerUiScan = 0;

void HideEditorFrame();
void HideGameFrame() {
    HideBallFrame();
    FreezeBallFrame();
    HideEditorFrame();
    if (gHiders.empty()) return RestoreGame();
    const double now = game::Seconds();
    Obj raceUi = RaceUi();
    if (eng::Get(gHiddenIn) != raceUi) {                // another map, another UI
        gHiddenParts.clear();
        gHiddenIn = eng::MakeWeak(raceUi);
    }
    if (now >= gNextHide && (!raceUi || race::EditorTesting())) {
        // Editor test-play has no race UI manager: its HUD is a WBP_PlayerUI of its own (measured 2026-10-06).
        gNextHide = now + 0.25;
        Obj ui = eng::Get(gEditorPlayerUi);
        if (!ui && now >= gNextPlayerUiScan) {
            gNextPlayerUiScan = now + 2;
            Obj cls = eng::FindClass("WBP_PlayerUI_C");
            if (cls) eng::ForEachObject([&](Obj o) {
                if (!eng::IsA(o, cls) || eng::IsDefaultObject(o)) return true;
                // (the blueprints' own templates are instances too: only a live one, in the transient package)
                if (eng::PathOf(o).rfind("/Engine/Transient.", 0) != 0) return true;
                ui = o;
                return false;
            });
            gEditorPlayerUi = eng::MakeWeak(ui);
        }
        if (ui) HidePart(ui);
        if (!raceUi) return;
        gNextHide = 0;
    }
    if (!raceUi || now < gNextHide) return;
    gNextHide = now + 0.25;
    const std::vector<Obj> line = FooterLine(raceUi);
    Obj root = RootOf(raceUi);
    if (line.empty()) {                                 // no footer: all of it
        if (root) HidePart(root);
        return;
    }
    for (size_t k = 1; k < line.size(); ++k) {
        Obj panel = line[k];
        const int32_t n = eng::Call(panel, "GetChildrenCount").ReturnAs<int32_t>(0);
        for (int32_t i = 0; i < n; ++i) {
            Obj child = eng::Call(panel, "GetChildAt", i).ReturnObj();
            if (child && child != line[k - 1]) HidePart(child);
        }
    }
}


// --- the track editor's own screens around a test run (HideEditor): everything in the editor's widget except the
// line down to its player UI (the race clock), so a test run's finish shows only the world and the clock.
std::set<int> gEditorHiders;
struct EditorPart {
    eng::Weak widget;
    uint8_t visibility = 0;
};
std::vector<EditorPart> gEditorParts;
double gNextEditorHide = 0;

Obj LiveOf(const char* className) {
    Obj cls = eng::FindClass(className), found = nullptr;
    if (cls) eng::ForEachObject([&](Obj o) {
        if (!eng::IsA(o, cls) || eng::IsDefaultObject(o)) return true;
        if (eng::PathOf(o).rfind("/Engine/Transient.", 0) != 0) return true;
        found = o;
        return false;
    });
    return found;
}

void RestoreEditor() {
    for (const auto& part : gEditorParts)
        if (Obj widget = eng::Get(part.widget)) w::SetVisibility(widget, part.visibility);
    gEditorParts.clear();
}

void HideEditorPart(Obj widget) {
    for (const auto& part : gEditorParts)
        if (eng::Get(part.widget) == widget) {
            w::SetVisibility(widget, kHidden);
            return;
        }
    gEditorParts.push_back({eng::MakeWeak(widget), eng::Call(widget, "GetVisibility").ReturnAs<uint8_t>(0)});
    w::SetVisibility(widget, kHidden);
}

eng::Weak gEditorWidget, gEditorUi;
void HideEditorFrame() {
    if (gEditorHiders.empty()) {
        if (!gEditorParts.empty()) RestoreEditor();
        return;
    }
    const double now = game::Seconds();
    if (now < gNextEditorHide) return;
    gNextEditorHide = now + 0.2;
    Obj editor = eng::Get(gEditorWidget);
    if (!editor || !eng::IsLive(editor)) {
        editor = LiveOf("W_MapEditor_C");
        gEditorWidget = eng::MakeWeak(editor);
        gEditorUi = {};
    }
    if (!editor) return;
    Obj ui = eng::Get(gEditorUi);
    if (!ui) {
        ui = w::FindFirst(editor, eng::FindClass("WBP_PlayerUI_C"));
        gEditorUi = eng::MakeWeak(ui);
    }
    std::vector<Obj> line;
    for (Obj x = ui; x && x != editor && line.size() < 64;) {
        line.push_back(x);
        x = eng::Call(x, "GetParent").ReturnObj();
    }
    if (line.empty()) {
        if (Obj root = RootOf(editor)) HideEditorPart(root);
        return;
    }
    for (size_t k = 1; k < line.size(); ++k) {
        Obj panel = line[k];
        const int32_t n = eng::Call(panel, "GetChildrenCount").ReturnAs<int32_t>(0);
        for (int32_t i = 0; i < n; ++i) {
            Obj child = eng::Call(panel, "GetChildAt", i).ReturnObj();
            if (child && child != line[k - 1]) HideEditorPart(child);
        }
    }
    // the editor's own widgets added to the viewport outside its tree (the pause / finish menu, the header)
    for (const char* cls : {"WBP_Editor_Header_C"})
        if (Obj other = LiveOf(cls)) HideEditorPart(other);
}

void HideEditor(int owner, bool hidden) {
    if (hidden) gEditorHiders.insert(owner);
    else gEditorHiders.erase(owner);
    if (gEditorHiders.empty()) RestoreEditor();
}

void HideGame(int owner, bool hidden) {
    if (hidden) gHiders.insert(owner);
    else gHiders.erase(owner);
    if (gHiders.empty()) RestoreGame();
}

void RemoveOwner(int owner) {
    HideGame(owner, false);
    HideEditor(owner, false);
    HideBall(owner, false);
    FreezeBall(owner, false);
}

}  // namespace hud
