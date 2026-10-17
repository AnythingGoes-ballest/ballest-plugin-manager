#include "browser.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cosmetics.hpp"
#include "game.hpp"
#include "hoverfx.hpp"
#include "log.hpp"
#include "packages.hpp"
#include "plugins.hpp"
#include "ux.hpp"
#include "widgets.hpp"

using eng::Obj;
using ui::Color;
namespace w = ui::widgets;

namespace browser {
namespace {

// --- the look (a placeholder, in the Customize page's colours) ---------------------------------------------------------
const Color kLime = ux::Hex("#BEFF00"), kBlack = ux::Hex("#000000"), kWhite = ux::Hex("#FFFFFF");
const Color kWindow = ux::Hex("#141518"), kNav = ux::Hex("#0D0E10"), kCard = ux::Hex("#1D1F23"), kCardHover = ux::Hex("#2A2D33");
const Color kPictureBack = ux::Hex("#26282D"), kMuted = ux::Hex("#8B8E93"), kLine = ux::Hex("#26282D"), kField = ux::Hex("#202227");
const Color kNavText = ux::Hex("#D6D8DB"), kNew = ux::Hex("#F80042"), kBadge = ux::Hex("#2A2C31");
constexpr double kWidth = 1120, kHeight = 680, kNavWidth = 220, kItemHeight = 34, kItemGap = 6;
constexpr int kColumns = 5;
constexpr double kCardWidth = 158, kCardGap = 14, kCardHeight = 214;
constexpr size_t kMaxCards = 240;
constexpr double kEase = 0.06;

ux::Screen gScreen;
bool gOpen = false;
double gLast = -1;

struct NavButton {
    std::string view, label;
    ux::Node base, bubble, text, hit;
    ux::Click click;
    double hover = 0, wiggleAt = -100;
    bool wasHovered = false;
};
struct Frame_ {
    ux::Node dim, dimHit, edge, back, nav, fieldBack, field, title, count, note, closeBack, closeText, closeHit, grid;
    std::vector<ux::Node> lines;
    ux::Click dimClick, closeClick;
    eng::Weak scroll, content;
    bool built = false;
} gUi;
std::vector<NavButton> gNav;
hoverfx::Pair gNavFx;                   // the Customize page's slot effect, out of the hovered nav button
std::string gNavKey;
std::string gView = "all";
std::string gSearch, gSearchShown;
double gSearchReadAt = -100;

struct Card {
    std::string category, id;
    eng::Weak hit, back, badge, badgeIcon;
    bool wasPressed = false, hovered = false;
    int shownWorn = -1;
};
std::vector<Card> gCards;
std::string gGridKey;
struct Pill {
    std::string what;                   // "style 1", "style 2", "theme <n>"
    eng::Weak hit, back, text;
    bool wasPressed = false;
};
std::vector<Pill> gPills;



std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Capital(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// --- what a view shows -------------------------------------------------------------------------------------------------
bool Recent(const std::string& added) {
    if (added.size() < 10) return false;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const int y = std::atoi(added.substr(0, 4).c_str()), m = std::atoi(added.substr(5, 2).c_str()), d = std::atoi(added.substr(8, 2).c_str());
    const int then = y * 372 + m * 31 + d, now = t.wYear * 372 + t.wMonth * 31 + t.wDay;
    return now - then <= 7;
}

std::vector<const packages::Item*> Shown() {
    std::vector<const packages::Item*> out;
    std::vector<std::string> words;
    {
        std::string word;
        for (char c : Lower(gSearch) + " ") {
            if (c == ' ') {
                if (!word.empty()) words.push_back(word);
                word.clear();
            } else {
                word.push_back(c);
            }
        }
    }
    // (an extra's cosmetic shows when its slot does: Cosmetic Kit Plus running, or a plugin filled it)
    const std::vector<std::string> shown = packages::ShownSlots(true);
    for (const packages::Item& item : packages::Items()) {
        const bool extra = item.category != "ball" && item.category != "hat" && item.category != "bfx";
        if (extra && std::find(shown.begin(), shown.end(), item.category) == shown.end()) continue;
        if (gView != "all" && gView != "newest" && item.category != gView) continue;
        const std::string hay = Lower(item.name + " " + item.id + " " + item.author + " " + item.pack);
        if (!std::all_of(words.begin(), words.end(), [&](const std::string& word) { return hay.find(word) != std::string::npos; })) continue;
        out.push_back(&item);
    }
    if (gView == "newest")
        std::stable_sort(out.begin(), out.end(), [](const packages::Item* a, const packages::Item* b) { return a->added > b->added; });
    if (out.size() > kMaxCards) out.resize(kMaxCards);
    return out;
}

std::string ViewTitle() {
    if (gView == "all") return "All";
    if (gView == "newest") return "Newest";
    if (gView == "settings") return "Cosmetic Kit";
    return Capital(packages::LabelOf(gView));
}

std::string ViewNote(size_t shown) {
    if (gView == "settings") return "Settings for now, until Cosmetic Kit's own settings popup is designed.";
    if (shown == 0) return gSearch.empty() ? "Nothing here yet." : "Nothing matches the search.";
    return "Installed cosmetics. Click one to wear it.";
}

// --- the frame -----------------------------------------------------------------------------------------------------------
void Drop(ux::Node& n) {
    if (Obj o = n.Get()) eng::Call(o, "RemoveFromParent");
    n = {};
}

Obj SearchBox(Obj tree) {
    Obj input = w::Spawn("EditableTextBox", tree);
    if (!input) return nullptr;
    eng::WriteBool(input, "ClearKeyboardFocusOnCommit", false);
    for (const char* member : {"ForegroundColor", "FocusedForegroundColor"}) w::WriteSlateColor(input, {"WidgetStyle", member}, kWhite);
    w::WriteSlateColor(input, {"WidgetStyle", "TextStyle", "ColorAndOpacity"}, kWhite);
    w::WriteSlateColor(input, {"WidgetStyle", "BackgroundColor"}, {0, 0, 0, 0});
    eng::Prop face;
    const int at = eng::NestedOffset(eng::ClassOf(input), {"WidgetStyle", "TextStyle", "Font", "FontObject"}, &face);
    if (Obj font = ux::Font(); font && at >= 0 && face.size == static_cast<int32_t>(sizeof font)) std::memcpy(input + at, &font, sizeof font);
    w::SetFontSize(input, 11.5f, {"WidgetStyle", "TextStyle", "Font"});
    w::SetHintText(input, "search cosmetics");
    return input;
}

void BuildFrame() {
    Obj tree = gScreen.Tree(), canvas = gScreen.Canvas();
    gUi.dim = ux::MakeBox(tree, canvas, {0, 0, 0, 0.55f}, 0, 0, 0, 0, 0);
    gUi.dimHit = ux::MakeHit(tree, canvas, 1);
    gUi.edge = ux::MakeBox(tree, canvas, kBlack, 20, 20, 20, 20, 2);
    gUi.back = ux::MakeBox(tree, canvas, kWindow, 18, 18, 18, 18, 3);
    w::SetVisibility(gUi.back.Get(), 0);         // the window takes its clicks: only outside it closes the browser
    gUi.nav = ux::MakeBox(tree, canvas, kNav, 18, 0, 0, 18, 4);
    gUi.fieldBack = ux::MakeBox(tree, canvas, kField, 10, 10, 10, 10, 5);
    gUi.field = ux::Place(canvas, SearchBox(tree), 6);
    gUi.title = ux::MakeText(tree, canvas, "All", 30, kWhite, 6, "Regular", 0);
    gUi.count = ux::MakeText(tree, canvas, "", 13, kMuted, 6, "Regular", 0);
    gUi.note = ux::MakeText(tree, canvas, "", 13, kMuted, 6, "Regular", 0);
    gUi.closeBack = ux::MakeBox(tree, canvas, kBadge, 16, 16, 16, 16, 6);
    gUi.closeText = ux::MakeText(tree, canvas, "x", 18, kWhite, 7);
    gUi.closeHit = ux::MakeHit(tree, canvas, 8);
    Obj scroll = w::Spawn("ScrollBox", tree), content = w::Spawn("VerticalBox", tree);
    if (scroll && content) {
        w::AddChild(scroll, content);
        gUi.grid = ux::Place(canvas, scroll, 6);
        gUi.scroll = eng::MakeWeak(scroll);
        gUi.content = eng::MakeWeak(content);
    }
    gUi.built = true;
}

std::vector<std::pair<std::string, std::string>> Views() {
    std::vector<std::pair<std::string, std::string>> out = {{"all", "All"}, {"newest", "Newest"}, {"", ""}};
    for (const std::string& slot : packages::ShownSlots(true)) out.push_back({slot, Capital(packages::LabelOf(slot))});
    out.push_back({"", ""});
    out.push_back({"settings", "Settings"});
    return out;
}

void BuildNav() {
    for (auto& n : gNav)
        for (ux::Node* node : {&n.base, &n.bubble, &n.text, &n.hit}) Drop(*node);
    gNav.clear();
    for (auto& l : gUi.lines) Drop(l);
    gUi.lines.clear();
    Obj tree = gScreen.Tree(), canvas = gScreen.Canvas();
    for (const auto& [view, label] : Views()) {
        if (view.empty()) {
            gUi.lines.push_back(ux::MakeBox(tree, canvas, kLine, 1, 1, 1, 1, 5));
            continue;
        }
        NavButton b;
        b.view = view;
        b.label = label;
        b.base = ux::MakeBox(tree, canvas, kLime, 8, 8, 8, 8, 5);
        b.bubble = ux::MakeBubble(tree, canvas, 8, 8, 8, 8, 6);
        b.text = ux::MakeText(tree, canvas, label, 17, kNavText, 7, "Regular", 0);
        b.hit = ux::MakeHit(tree, canvas, 8);
        gNav.push_back(std::move(b));
    }
}

// --- the grid ------------------------------------------------------------------------------------------------------------
Obj PictureOf(const packages::Item& item) {
    if (Obj t = ux::LoadPicture(item.preview)) return t;
    if (Obj t = cosmetics::PreviewOf(item.category, item.id)) return t;
    const std::string c = item.category;
    const bool known = c == "ball" || c == "hat" || c == "bfx" || c == "arms" || c == "bounce";
    return ux::Picture(known ? ("slot-" + c + ".png").c_str() : "slot-extra.png");
}

Obj RoundedImage(Obj tree, Obj texture, double radius) {
    Obj image = w::Spawn("Image", tree);
    if (!image) return nullptr;
    ux::RoundBrush(image, "Brush", radius, radius, radius, radius);
    if (texture) eng::Call(image, "SetBrushResourceObject", texture);
    return image;
}

Obj Clip(Obj box) {
    if (box) eng::Call(box, "SetClipping", uint8_t{1});
    return box;
}

void InOverlay(Obj overlay, Obj child, uint8_t alignX, uint8_t alignY, w::Margin padding) {
    if (child) w::AddToOverlay(overlay, child, alignX, alignY, padding);
}

Obj MakeCard(Obj tree, const packages::Item& item, Card* card) {
    Obj overlay = w::Spawn("Overlay", tree);
    Obj back = ux::Rounded(tree, kCard, 10, {8, 8, 8, 8}), column = w::Spawn("VerticalBox", tree);
    if (!overlay || !back || !column) return nullptr;
    w::AddChild(back, column);
    // the picture, on a rounded patch
    Obj frame = w::Spawn("Overlay", tree);
    InOverlay(frame, ux::Rounded(tree, kPictureBack, 8, {0, 0, 0, 0}), w::kAlignFill, w::kAlignFill, {0, 0, 0, 0});
    InOverlay(frame, RoundedImage(tree, PictureOf(item), 8), w::kAlignFill, w::kAlignFill, {0, 0, 0, 0});
    ux::InBox(column, ux::Sized(tree, frame, kCardWidth - 16, kCardWidth - 16), {0, 0, 0, 0});
    ux::InBox(column, Clip(ux::Sized(tree, ux::TextBlock(tree, item.name, 15, kWhite, "Bold"), kCardWidth - 16, 20)), {0, 7, 0, 0});
    const std::string by = (item.author.empty() ? item.plugin : item.author) + " (plugin)";
    ux::InBox(column, Clip(ux::Sized(tree, ux::TextBlock(tree, by, 11, kMuted), kCardWidth - 50, 16)), {0, 1, 0, 0});
    InOverlay(overlay, back, w::kAlignFill, w::kAlignFill, {0, 0, 0, 0});
    // worn or not: a check in a lime circle when worn, else a dark one
    Obj badge = ux::Rounded(tree, kBadge, 13, {0, 0, 0, 0});
    Obj icon = RoundedImage(tree, ux::Picture("check.png"), 0);
    if (badge && icon) {
        w::AddChild(badge, ux::Sized(tree, icon, 15, 15));
        eng::Call(badge, "SetHorizontalAlignment", w::kAlignCenter);
        eng::Call(badge, "SetVerticalAlignment", w::kAlignCenter);
        InOverlay(overlay, ux::Sized(tree, badge, 26, 26), w::kAlignEnd, w::kAlignEnd, {0, 0, 8, 10});
    }
    if (Recent(item.added)) {
        Obj tag = ux::Rounded(tree, kNew, 5, {6, 2, 6, 2});
        if (tag) {
            w::AddChild(tag, ux::TextBlock(tree, "NEW", 10, kWhite, "Bold"));
            InOverlay(overlay, tag, w::kAlignLeft, w::kAlignLeft, {14, 14, 0, 0});
        }
    }
    Obj hit = w::Spawn("Button", tree);
    if (hit) {
        w::Transparent(hit);
        w::Unfocusable(hit);
        InOverlay(overlay, hit, w::kAlignFill, w::kAlignFill, {0, 0, 0, 0});
    }
    card->category = item.category;
    card->id = item.id;
    card->hit = eng::MakeWeak(hit);
    card->back = eng::MakeWeak(back);
    card->badge = eng::MakeWeak(badge);
    card->badgeIcon = eng::MakeWeak(icon);
    return ux::Sized(tree, overlay, kCardWidth, kCardHeight);
}

Obj MakePill(Obj tree, const std::string& label, const std::string& what, bool on) {
    Obj button = w::Spawn("Button", tree), back = ux::Rounded(tree, on ? kLime : kBadge, 10, {16, 6, 16, 7});
    Obj text = ux::TextBlock(tree, label, 15, on ? kBlack : kNavText);
    if (!button || !back || !text) return nullptr;
    w::Transparent(button);
    w::Unfocusable(button);
    w::AddChild(back, text);
    w::AddChild(button, back);
    gPills.push_back({what, eng::MakeWeak(button), eng::MakeWeak(back), eng::MakeWeak(text)});
    return button;
}

void FillSettings(Obj tree, Obj content) {
    const packages::Settings& s = packages::Config();
    ux::InBox(content, ux::TextBlock(tree, "slot tab names", 17, kWhite), {4, 4, 0, 6});
    if (Obj row = w::Spawn("HorizontalBox", tree)) {
        ux::InRow(row, MakePill(tree, "sideways", "style 1", s.labelStyle == 1), 0);
        ux::InRow(row, MakePill(tree, "horizontal", "style 2", s.labelStyle == 2), 8);
        ux::InBox(content, row, {4, 0, 0, 18});
    }
    ux::InBox(content, ux::TextBlock(tree, "Customize panel theme", 17, kWhite), {4, 0, 0, 6});
    if (Obj row = w::Spawn("HorizontalBox", tree)) {
        ux::InRow(row, MakePill(tree, "default", "theme 0", s.theme == 0), 0);
        ux::InRow(row, MakePill(tree, "charcoal", "theme 1", s.theme == 1), 8);
        ux::InRow(row, MakePill(tree, "ballest night", "theme 2", s.theme == 2), 8);
        ux::InBox(content, row, {4, 0, 0, 18});
    }
}

void FillGrid() {
    Obj content = eng::Get(gUi.content), tree = gScreen.Tree();
    if (!content || !tree) return;
    eng::Call(content, "ClearChildren");
    gCards.clear();
    gPills.clear();
    if (gView == "settings") {
        FillSettings(tree, content);
        return;
    }
    const std::vector<const packages::Item*> items = Shown();
    Obj row = nullptr;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i % kColumns == 0) {
            row = w::Spawn("HorizontalBox", tree);
            ux::InBox(content, row, {0, i ? static_cast<float>(kCardGap) : 0.0f, 0, 0});
        }
        Card card;
        Obj made = MakeCard(tree, *items[i], &card);
        if (!made) continue;
        ux::InRow(row, made, i % kColumns ? kCardGap : 0, false, w::kAlignLeft);
        gCards.push_back(std::move(card));
    }
}

void WatchCards() {
    for (Card& c : gCards) {
        Obj hit = eng::Get(c.hit);
        if (!hit) continue;
        const bool over = eng::Call(hit, "IsHovered").ReturnBool();
        if (over != c.hovered) {
            c.hovered = over;
            if (Obj back = eng::Get(c.back)) eng::Call(back, "SetBrushColor", over ? kCardHover : kCard);
        }
        const bool worn = cosmetics::WornItem(c.category) == c.id;
        if (Obj badge = eng::Get(c.badge); badge && c.shownWorn != static_cast<int>(worn)) {
            c.shownWorn = worn;
            eng::Call(badge, "SetBrushColor", worn ? kLime : kBadge);
            if (Obj icon = eng::Get(c.badgeIcon)) eng::Call(icon, "SetColorAndOpacity", worn ? kBlack : kMuted);
        }
        const bool pressed = over && eng::Call(hit, "IsPressed").ReturnBool();
        if (c.wasPressed && !pressed && over) {
            const bool on = cosmetics::WearItem(c.category, c.id);
            if (on) cosmetics::ShowSlot(c.category);
            hostlog::Info("browser: " + c.category + " " + c.id + (on ? " worn" : " could not be worn"));
        }
        c.wasPressed = pressed;
    }
}

void WatchPills() {
    for (Pill& p : gPills) {
        Obj hit = eng::Get(p.hit);
        if (!hit) continue;
        const bool pressed = eng::Call(hit, "IsPressed").ReturnBool();
        if (p.wasPressed && !pressed && eng::Call(hit, "IsHovered").ReturnBool()) {
            if (p.what == "style 1") packages::SetLabelStyle(1);
            if (p.what == "style 2") packages::SetLabelStyle(2);
            if (p.what.rfind("theme ", 0) == 0) packages::SetTheme(p.what.back() - '0');
            gGridKey.clear();               // the pills show the new choice
            return;
        }
        p.wasPressed = pressed;
    }
}

void UpdateNav(double wx, double wy, double dt, double now) {
    double y = wy + 74;
    size_t line = 0;
    gNavFx.Begin();
    // moving from one button to another: the last one's white goes quickly; leaving the nav: slowly
    const bool anyHovered = std::any_of(gNav.begin(), gNav.end(), [](const NavButton& n) { return n.click.hovered; });
    for (const auto& [view, label] : Views()) {
        if (view.empty()) {
            if (line < gUi.lines.size()) gUi.lines[line++].Box(wx + 22, y + 4, kNavWidth - 44, 2);
            y += 14;
            continue;
        }
        auto it = std::find_if(gNav.begin(), gNav.end(), [&](const NavButton& b) { return b.view == view; });
        if (it == gNav.end()) continue;
        NavButton& b = *it;
        const bool on = gView == view;
        const bool hovered = b.click.hovered;
        if (hovered && !b.wasHovered) b.wiggleAt = now;
        b.wasHovered = hovered;
        // hovered: plain white (as the Customize page's name tabs), back to its rest look about as long as the effect plays
        b.hover = ux::Approach(b.hover, hovered ? 1.0 : 0.0, dt, hovered ? kEase * 0.6 : anyHovered ? 0.045 : hoverfx::kDuration / 3);
        const double x = wx + 14, wd = kNavWidth - 28;
        gNavFx.GlowFrom(x + wd);
        gNavFx.Item(gScreen.Tree(), gScreen.Canvas(), 4, 9, view, hovered, x + wd * 0.6, x - 24, y + kItemHeight / 2, y, kItemHeight, b.hover,
                    now);   // from past its middle, off to the left
        b.base.Box(x, y, wd, kItemHeight);
        b.base.Show(true);
        b.base.Tint(ux::Mix(on ? kLime : Color{1, 1, 1, 0}, kWhite, b.hover));
        b.base.Opacity(1);
        b.bubble.Box(x, y, wd, kItemHeight);
        // leaving: white, then the lime bubble, then its rest
        const double afterglow = !hovered && !on ? 4 * b.hover * (1 - b.hover) : 0;
        b.bubble.Tint({1, 1, 1, static_cast<float>(on ? 1 - b.hover : afterglow)});
        b.text.Center(x + 12, y + kItemHeight / 2);
        b.text.Tint(ux::Mix(on ? kBlack : kNavText, kBlack, b.hover));
        b.text.Transform(1, ux::Wiggle(now - b.wiggleAt) * 0.6);
        b.hit.Box(x, y, wd, kItemHeight);
        if (b.click.Update(b.hit) && gView != view) {
            gView = view;
            hostlog::Info("browser: " + view);
        }
        y += kItemHeight + kItemGap;
    }
    gNavFx.Finish();
}

// The search box's text, read while it is being typed in (a few times a second: each read costs a little) and once
// more when the typing ends.
void ReadSearch() {
    static bool wasFocused = false;
    Obj field = gUi.field.Get();
    if (!field) return;
    const bool focused = eng::Call(field, "HasKeyboardFocus").ReturnBool();
    const bool ended = wasFocused && !focused;
    wasFocused = focused;
    const double now = game::Seconds();
    if (!ended && (!focused || now - gSearchReadAt < 0.25)) return;
    gSearchReadAt = now;
    gSearch = w::ReadText(field);
}

}  // namespace

void Open() {
    if (!packages::KitOn()) return;
    gOpen = true;
    gGridKey.clear();
    hostlog::Info("browser: opened");
}

void Close() {
    if (!gOpen) return;
    gOpen = false;
    if (gScreen.Ready()) gScreen.Show(false);
    hostlog::Info("browser: closed");
}

bool IsOpen() { return gOpen; }

void Frame() {
    const double now = game::Seconds();
    const double dt = gLast < 0 ? 0 : std::min(0.1, now - gLast);
    gLast = now;
    if (gOpen && (!packages::KitOn() || !cosmetics::PageOnScreen())) Close();
    if (!gOpen) return;
    if (!gScreen.Ready()) {
        gUi = {};
        gNav.clear();
        gNavKey.clear();
        gCards.clear();
        gPills.clear();
        gGridKey.clear();
        gScreen.zOrder = 150;
        if (!gScreen.Make()) return;
        BuildFrame();
    }
    gScreen.Show(true);
    double sw = 1920, sh = 1080;
    ux::ScreenSize(&sw, &sh);
    const double wx = std::floor((sw - kWidth) / 2), wy = std::floor((sh - kHeight) / 2);
    // Every common resolution and aspect fits the window at its size (the game's UI is 1080 units tall); a smaller
    // window or screen than that shrinks it to fit, about the middle. The dim covers well past the edges either way.
    gScreen.Scale(std::min({1.0, (sw - 32) / (kWidth + 4), (sh - 32) / (kHeight + 4)}));
    gUi.dim.Box(-sw, -sh, 3 * sw, 3 * sh);
    gUi.dimHit.Box(-sw, -sh, 3 * sw, 3 * sh);
    gUi.edge.Box(wx - 2, wy - 2, kWidth + 4, kHeight + 4);
    gUi.back.Box(wx, wy, kWidth, kHeight);
    gUi.nav.Box(wx, wy, kNavWidth, kHeight);
    gUi.fieldBack.Box(wx + 14, wy + 18, kNavWidth - 28, 38);
    gUi.field.Box(wx + 20, wy + 21, kNavWidth - 40, 32);
    std::string navKey = std::to_string(packages::PlusOn());
    for (const std::string& s : packages::ShownSlots(true)) navKey += "|" + s;
    if (navKey != gNavKey) {
        // (the view's slot went away (Cosmetic Kit Plus turned off): All)
        const auto views = Views();
        if (std::none_of(views.begin(), views.end(), [](const auto& v) { return v.first == gView; })) gView = "all";
        BuildNav();
        gNavKey = navKey;
    }
    UpdateNav(wx, wy, dt, now);
    ReadSearch();
    // the grid, built again when what it shows changes
    const std::vector<const packages::Item*> shown = gView == "settings" ? std::vector<const packages::Item*>{} : Shown();
    std::string key = gView + "|" + gSearch + "|" + std::to_string(packages::Items().size());
    for (const auto* item : shown) key += "|" + item->id;
    if (key != gGridKey) {
        gGridKey = key;
        FillGrid();
    }
    const double cx = wx + kNavWidth + 24;
    gUi.title.Text(ViewTitle());
    gUi.title.Center(cx, wy + 38);
    const double titleWidth = gUi.title.TextWidth();
    gUi.count.Text(gView == "settings" ? "" : std::to_string(shown.size()) + (shown.size() == 1 ? " item" : " items"));
    gUi.count.Center(cx + (titleWidth > 0 ? titleWidth : 120) + 14, wy + 42);
    gUi.note.Text(ViewNote(shown.size()));
    gUi.note.Center(cx, wy + 72);
    gUi.closeBack.Box(wx + kWidth - 50, wy + 18, 32, 32);
    gUi.closeText.Center(wx + kWidth - 34, wy + 33);
    gUi.closeHit.Box(wx + kWidth - 50, wy + 18, 32, 32);
    gUi.grid.Box(cx - 8, wy + 96, kWidth - kNavWidth - 32, kHeight - 112);
    WatchCards();
    if (!gOpen) return;
    WatchPills();
    if (gUi.closeClick.Update(gUi.closeHit) || gUi.dimClick.Update(gUi.dimHit)) Close();
}

}  // namespace browser
