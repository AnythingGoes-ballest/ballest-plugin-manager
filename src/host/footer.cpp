// Footer entries: plugin buttons in the game's own footer bar and panels above it. The footer exists on the main
// menu (WBP_MainMenu_UIManager_C) and inside maps (WBP_RaceUIManager_C); whichever is live is used, and the
// entries are rebuilt whenever the game replaces it. Measured tree: SizeBox > Overlay > [Image, HorizontalBox];
// the HorizontalBox holds master volume, music, an empty Overlay (the flexible gap), Discord, language.
#include "game.hpp"
#include "log.hpp"
#include "ui.hpp"
#include "widgets.hpp"

using eng::Obj;
namespace w = ui::widgets;

namespace ui {
namespace {

std::vector<std::unique_ptr<FooterButton>> gButtons;
std::vector<std::unique_ptr<Panel>> gPanels;
int gFrame = 0;

struct Footer {
    eng::Weak footer;
    eng::Weak tree;         // the footer's WidgetTree: outer of everything built here
    eng::Weak gap;          // the empty Overlay before Discord: buttons sit at its right edge
    eng::Weak menuRoot;     // the owning menu's root panel (Overlay or CanvasPanel), logged as a layout check
    eng::Weak refText;      // a footer text block whose font and colour are copied
} gFooter;

Obj FindLiveFooter() {
    Obj cls = eng::FindClass("WBP_Footer_C");
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) != cls || eng::IsDefaultObject(o)) return true;
        const std::string path = eng::PathOf(o);
        if (path.rfind("/Engine/Transient", 0) == 0 &&
            (path.find("WBP_MainMenu_UIManager_C") != std::string::npos || path.find("WBP_RaceUIManager_C") != std::string::npos))
            found = o;
        return found == nullptr;
    });
    return found;
}

void ForgetButton(FooterButton& b) {
    b.button = b.text = {};
    b.shownLabel.clear();
    b.wasPressed = b.shownHover = false;
}

void ForgetWidgets() {
    for (auto& b : gButtons) ForgetButton(*b);
    for (auto& p : gPanels) {
        if (Obj host = eng::Get(p->host)) eng::Call(host, "RemoveFromParent");     // still up from the last footer
        p->host = p->border = p->box = {};
        p->shownVisible = p->shownOnce = false;
        for (auto& b : p->buttons) ForgetButton(*b);
    }
}

bool Adopt(Obj footer) {
    Obj discord = eng::ReadObj(footer, "WBP_CTAButton_Discord");
    Obj row = eng::ReadObj(eng::ReadObj(discord, "Slot"), "Parent");
    Obj gap = nullptr;
    const auto slots = eng::ReadObjArray(row, "Slots");
    for (size_t i = 1; i < slots.size(); ++i)
        if (eng::ReadObj(slots[i], "Content") == discord) {
            Obj before = eng::ReadObj(slots[i - 1], "Content");
            if (eng::IsA(before, eng::FindClass("Overlay"))) gap = before;
        }
    Obj menu = eng::OuterOf(eng::OuterOf(footer));          // footer -> WidgetTree -> the owning menu
    Obj root = eng::ReadObj(eng::ReadObj(menu, "WidgetTree"), "RootWidget");
    if (!eng::IsA(root, eng::FindClass("Overlay")) && !eng::IsA(root, eng::FindClass("CanvasPanel"))) root = nullptr;
    Obj tree = eng::ReadObj(footer, "WidgetTree");
    Obj refText = w::FindFirst(discord, eng::FindClass("TextBlock"));
    hostlog::Info("footer " + eng::PathOf(footer) + ": menu root " + eng::ObjName(root) + ", reference text " + eng::ObjName(refText));
    if (!tree || !gap) {
        hostlog::Error("footer layout not as measured; footer entries not placed");
        return false;
    }
    gFooter = {eng::MakeWeak(footer), eng::MakeWeak(tree), eng::MakeWeak(gap), eng::MakeWeak(root), eng::MakeWeak(refText)};
    ForgetWidgets();
    return true;
}

// Text styled like the footer's own, owned by `tree` (the footer's, or a panel's).
Obj FooterText(const std::string& s, float sizeScale = 1.0f, Obj tree = nullptr) {
    Obj text = w::Spawn("TextBlock", tree ? tree : eng::Get(gFooter.tree));
    w::CopyFont(eng::Get(gFooter.refText), text, sizeScale);
    w::SetText(text, s);
    return text;
}

void BuildButton(FooterButton& b) {
    Obj tree = eng::Get(gFooter.tree), gap = eng::Get(gFooter.gap);
    Obj text = FooterText(b.label), button = w::Spawn("Button", tree);
    if (!text || !button || !gap) return;
    w::Transparent(button);
    eng::Call(button, "SetContent", text);
    // Buttons stack leftwards from the right edge of the gap before Discord.
    int placed = 0;
    for (auto& other : gButtons)
        if (other.get() != &b && eng::Get(other->button)) ++placed;
    if (!w::AddToOverlay(gap, button, w::kAlignEnd, w::kAlignCenter, {0, 0, 16.0f + 120.0f * placed, 0})) return;
    b.button = eng::MakeWeak(button);
    b.text = eng::MakeWeak(text);
    b.shownLabel = b.label;
    b.normalColor = w::TextColor(text);
    hostlog::Info("footer button '" + b.label + "' placed");
}

// Label changes, hover and clicks of a built footer or panel button. False if it is not built.
bool TrackButton(FooterButton& b) {
    Obj button = eng::Get(b.button), text = eng::Get(b.text);
    if (!button || !text) return false;
    if (b.label != b.shownLabel) {
        w::SetText(text, b.label);
        b.shownLabel = b.label;
    }
    // A click is a press that ends while the pointer is still over the button.
    const bool pressed = eng::Call(button, "IsPressed").ReturnBool();
    b.hovered = eng::Call(button, "IsHovered").ReturnBool();
    if (b.wasPressed && !pressed && b.hovered) {
        b.clickPending = true;
        hostlog::Info("footer button '" + b.label + "' clicked");
    }
    b.wasPressed = pressed;
    return true;
}

void SyncButton(FooterButton& b) {
    if (!eng::Get(b.button) || !eng::Get(b.text)) BuildButton(b);
    if (!TrackButton(b)) return;
    Obj text = eng::Get(b.text);
    if (b.hovered != b.shownHover) {
        w::SetTextColor(text, b.hovered ? Color{1, 1, 1, 1} : b.normalColor);
        b.shownHover = b.hovered;
    }
}

// A panel is an on-screen widget of its own in front of everything (the game's menus and plugin windows), placed
// bottom-right just above the footer. It is built while a footer is live and goes when the footer does.
void BuildPanel(Panel& p) {
    Obj host = nullptr, tree = nullptr, canvas = nullptr;
    if (!w::NewScreen(game::PlayerController(), &host, &tree, &canvas)) return;
    Obj border = w::Spawn("Border", tree), box = w::Spawn("VerticalBox", tree);
    if (!border || !box) return;
    eng::Call(border, "SetBrushColor", Color{0.008f, 0.008f, 0.012f, 1.0f});
    eng::Call(border, "SetPadding", w::Margin{20, 14, 20, 16});
    eng::Call(border, "SetContent", box);
    if (!w::AddToCanvas(canvas, border, 1, 1, {1, 1}, {-24, -56})) return;
    w::SetVisibility(border, w::kCollapsed);
    eng::Call(host, "AddToViewport", w::kPanelLayer);
    p.host = eng::MakeWeak(host);
    p.border = eng::MakeWeak(border);
    p.box = eng::MakeWeak(box);
    p.shownVisible = p.shownOnce = false;
}

// A panel button is a plain button with the footer's font, so it reads as clickable on the dark panel.
Obj BuildPanelButton(FooterButton& b, Obj tree) {
    Obj button = w::Spawn("Button", tree), text = FooterText(b.label, 1.0f, tree);
    if (!button || !text) return nullptr;
    w::Unfocusable(button);
    eng::Call(button, "SetBackgroundColor", Color{0.15f, 0.15f, 0.15f, 1});
    w::SetTextColor(text, {1, 1, 1, 1});
    eng::Call(button, "SetContent", text);
    b.button = eng::MakeWeak(button);
    b.text = eng::MakeWeak(text);
    b.shownLabel = b.label;
    b.wasPressed = false;
    return button;
}

void SyncPanel(Panel& p) {
    if (!eng::Get(p.border) || !eng::Get(p.box)) BuildPanel(p);
    Obj border = eng::Get(p.border), box = eng::Get(p.box);
    if (!border || !box) return;
    Obj tree = eng::OuterOf(box);               // the panel's own WidgetTree: everything in it is owned there
    if (!p.shownOnce || p.title != p.shownTitle || p.lines != p.shownLines || p.buttons.size() != p.shownButtons) {
        eng::Call(box, "ClearChildren");
        if (!p.title.empty()) {
            Obj title = FooterText(p.title, 1.25f, tree);
            w::SetTextColor(title, {1, 1, 1, 1});
            w::AddChild(box, title);
        }
        if (!p.buttons.empty()) {
            Obj row = w::Spawn("HorizontalBox", tree);
            for (size_t i = 0; i < p.buttons.size(); ++i) w::AddToRow(row, BuildPanelButton(*p.buttons[i], tree), i == 0 ? 0.0f : 8.0f);
            if (Obj slot = w::AddChild(box, row)) eng::Call(slot, "SetPadding", w::Margin{0, 8, 0, 8});
        }
        for (const auto& line : p.lines) {
            Obj text = FooterText(line, 1.0f, tree);
            w::SetTextColor(text, {0.82f, 0.82f, 0.82f, 1});    // the footer's own grey is too dim on a panel
            w::AddChild(box, text);
        }
        p.shownTitle = p.title;
        p.shownLines = p.lines;
        p.shownButtons = p.buttons.size();
        p.shownOnce = true;
    }
    for (auto& b : p.buttons) TrackButton(*b);
    if (p.visible != p.shownVisible) {
        w::SetVisibility(border, p.visible ? w::kSelfHitTestInvisible : w::kCollapsed);
        p.shownVisible = p.visible;
    }
}

}  // namespace

FooterButton* AddFooterButton(int owner, const std::string& label) {
    gButtons.push_back(std::make_unique<FooterButton>());
    gButtons.back()->owner = owner;
    gButtons.back()->label = label;
    return gButtons.back().get();
}

Panel* CreatePanel(int owner) {
    gPanels.push_back(std::make_unique<Panel>());
    gPanels.back()->owner = owner;
    return gPanels.back().get();
}

FooterButton* AddPanelButton(Panel* panel, const std::string& label) {
    panel->buttons.push_back(std::make_unique<FooterButton>());
    panel->buttons.back()->owner = panel->owner;
    panel->buttons.back()->label = label;
    return panel->buttons.back().get();
}

namespace footer {

void Frame() {
    if (gButtons.empty() && gPanels.empty()) return;
    // The footer is looked for again twice a second, and whenever the one in use is gone.
    if (!eng::Get(gFooter.footer) || ++gFrame % 30 == 0) {
        Obj live = FindLiveFooter();
        if (live != gFooter.footer.o || !eng::Get(gFooter.footer)) {
            if (!live || !Adopt(live)) {
                gFooter = Footer{};
                ForgetWidgets();
            }
        }
    }
    if (!eng::Get(gFooter.footer)) return;
    for (auto& b : gButtons) SyncButton(*b);
    for (auto& p : gPanels) SyncPanel(*p);
}

void HideOwner(int owner) {
    for (auto& p : gPanels)
        if (p->owner == owner) p->visible = false;
}

void RemoveOwner(int owner) {
    for (auto it = gButtons.begin(); it != gButtons.end();) {
        if ((*it)->owner != owner) {
            ++it;
            continue;
        }
        if (Obj button = eng::Get((*it)->button)) eng::Call(button, "RemoveFromParent");
        it = gButtons.erase(it);
    }
    for (auto it = gPanels.begin(); it != gPanels.end();) {
        if ((*it)->owner != owner) {
            ++it;
            continue;
        }
        if (Obj host = eng::Get((*it)->host)) eng::Call(host, "RemoveFromParent");
        it = gPanels.erase(it);
    }
}

bool SimulateClick(const std::string& label) {
    for (auto& b : gButtons)
        if (b->label == label) return b->clickPending = true;
    for (auto& p : gPanels)
        for (auto& b : p->buttons)
            if (b->label == label) return b->clickPending = true;
    return false;
}

std::string Status() {
    std::string s = std::string("footer=") + (eng::Get(gFooter.footer) ? "live" : "none");
    for (auto& b : gButtons) s += " button[" + b->label + "]=" + (eng::Get(b->button) ? "placed" : "absent");
    for (auto& p : gPanels)
        s += " panel[" + p->title + "]=" + (eng::Get(p->border) ? (p->shownVisible ? "shown" : "hidden") : "absent");
    return s;
}

}  // namespace footer
}  // namespace ui
