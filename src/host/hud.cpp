#include "hud.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>

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

void Frame() {
    if (gGeneration != game::Generation()) {        // a new map: everything found is gone with the old one
        gGeneration = game::Generation();
        gFound.clear();
        gOriginals.clear();
        gTints.clear();
        gRaceUi = {};
        gNextDiscover = 0;
    }
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
        out.push_back(e);
    }
    for (const auto& hw : ui::HudWindows()) out.push_back({hw.key, hw.key, "Window", hw.label, true, true});
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

}  // namespace hud
