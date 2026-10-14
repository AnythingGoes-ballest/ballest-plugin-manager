// Main-menu entries: plugin buttons in the main menu's own list (play, create, customize, settings, quit). Measured
// tree: WBP_MainMenu_UIManager_C.WBP_MenuTextGroup (WBP_MenuTextGroup_C) holds a VerticalBox "Vbox" of five
// WBP_ButtonBase_C, the game's menu button (a Button "HitBox" over a TextBlock "ButtonText"); the group binds each of
// its own buttons' OnClicked to OnMenuAction. A plugin entry is another WBP_ButtonBase_C made the same way, inserted
// after "play" and styled from its neighbour, with nothing bound: its clicks are read from its HitBox (pressed, then
// released while hovered), as the footer reads its buttons. Rebuilt whenever the main menu is rebuilt.
#include <cstring>

#include "game.hpp"
#include "log.hpp"
#include "ui.hpp"
#include "widgets.hpp"

using eng::Obj;
namespace w = ui::widgets;

namespace ui {
namespace {

std::vector<std::unique_ptr<FooterButton>> gMenuButtons;
eng::Weak gGroup, gBox;
int gFrame = 0;
double gAdopted = 0;            // when the main menu's list was found: entries go in once its fly-in has played

Obj FindMainMenuGroup() {
    Obj cls = eng::FindClass("WBP_MainMenu_UIManager_C");
    if (!cls) return nullptr;
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) != cls || eng::IsDefaultObject(o)) return true;
        if (eng::PathOf(o).rfind("/Engine/Transient", 0) == 0) found = o;
        return found == nullptr;
    });
    return found ? eng::ReadObj(found, "WBP_MenuTextGroup") : nullptr;
}

void Forget() {
    for (auto& b : gMenuButtons) {
        b->button = b->text = {};
        b->shownLabel.clear();
        b->wasPressed = false;
    }
}

// Copies a property's bytes from the game's own button to ours (its look: font size, width, padding, colours).
void CopyProp(Obj from, Obj to, const char* name, size_t size) {
    std::vector<uint8_t> bytes(size);
    if (eng::ReadBytes(from, name, bytes.data(), size)) eng::WriteBytes(to, name, bytes.data(), size);
}

// A VerticalBoxSlot's layout, read from its properties and set through its setters (so Slate takes it).
struct SlotLayout {
    w::Margin padding{};
    struct {
        float value;
        uint8_t rule;
    } size{1, 0};
    uint8_t h = 0, v = 0;
};
SlotLayout LayoutOf(Obj slot) {
    SlotLayout l;
    eng::ReadBytes(slot, "Padding", &l.padding, sizeof l.padding);
    uint8_t size[8] = {};
    if (eng::ReadBytes(slot, "Size", size, sizeof size)) {
        std::memcpy(&l.size.value, size, 4);
        l.size.rule = size[4];
    }
    eng::ReadBytes(slot, "HorizontalAlignment", &l.h, 1);
    eng::ReadBytes(slot, "VerticalAlignment", &l.v, 1);
    return l;
}
void LayOut(Obj slot, const SlotLayout& l) {
    eng::Call(slot, "SetPadding", l.padding);
    uint8_t size[8] = {};
    std::memcpy(size, &l.size.value, 4);
    size[4] = l.size.rule;
    eng::Call(slot, "SetSize", size);
    eng::Call(slot, "SetHorizontalAlignment", l.h);
    eng::Call(slot, "SetVerticalAlignment", l.v);
}

void Build(FooterButton& b, int position) {
    Obj group = eng::Get(gGroup), box = eng::Get(gBox);
    if (!group || !box) return;
    const auto slots = eng::ReadObjArray(box, "Slots");
    if (slots.empty()) return;
    Obj reference = eng::ReadObj(slots.front(), "Content");         // "play"
    Obj cls = eng::FindClass("WBP_ButtonBase_C");
    if (!reference || !cls || eng::ClassOf(reference) != cls) {
        hostlog::Error("main menu layout not as measured; menu entries not placed");
        return;
    }
    Obj button = eng::Call(eng::FindCdo("WidgetBlueprintLibrary"), "Create", group, cls, game::PlayerController()).ReturnObj();
    if (!button) return;
    CopyProp(reference, button, "Font Size", 4);
    CopyProp(reference, button, "MinWidth", 4);
    CopyProp(reference, button, "TextPadding", 16);
    CopyProp(reference, button, "TextJustification", 1);
    CopyProp(reference, button, "Transform Policy", 1);
    CopyProp(reference, button, "TypeSelection", 4);
    const int32_t index = 100 + position;          // outside the group's own button indices
    eng::WriteBytes(button, "Index", &index, sizeof index);
    // After "play" (and after earlier plugin entries). UMG has no InsertChildAt to call (not a UFUNCTION, measured),
    // so the entry is added at the end and the game's own entries that should follow it are taken out and added back
    // after it, each with its slot's layout (their widgets, and what is bound to them, stay the same).
    struct Moved {
        Obj content;
        SlotLayout layout;
    };
    std::vector<Moved> moved;
    for (size_t i = 1 + static_cast<size_t>(position); i < slots.size(); ++i)
        if (Obj content = eng::ReadObj(slots[i], "Content")) moved.push_back({content, LayoutOf(slots[i])});
    for (const auto& m : moved) eng::Call(box, "RemoveChild", m.content);
    Obj refSlot = slots.front();
    Obj slot = eng::Call(box, "AddChildToVerticalBox", button).ReturnObj();
    if (!slot) return;
    // The new slot laid out like the reference's (VerticalBoxSlot: Padding, Size, alignments).
    LayOut(slot, LayoutOf(refSlot));
    for (const auto& m : moved) {
        Obj back = eng::Call(box, "AddChildToVerticalBox", m.content).ReturnObj();
        if (!back) continue;
        LayOut(back, m.layout);
    }
    // A moved entry taken out during its fly-in animation stays where that animation left it (measured: "quit",
    // the last to fly in, came back invisible), so all of them are shown fully.
    for (const auto& m : moved) eng::Call(m.content, "SetRenderOpacity", 1.0f);
    Obj text = eng::ReadObj(button, "ButtonText");
    w::SetText(text, b.label);
    b.button = eng::MakeWeak(button);
    b.text = eng::MakeWeak(text);
    b.shownLabel = b.label;
    b.wasPressed = false;
    hostlog::Info("main menu entry '" + b.label + "' placed");
}

void Track(FooterButton& b) {
    Obj button = eng::Get(b.button), text = eng::Get(b.text);
    if (!button || !text) return;
    if (b.label != b.shownLabel) {
        w::SetText(text, b.label);
        b.shownLabel = b.label;
    }
    Obj hit = eng::ReadObj(button, "HitBox");
    if (!hit) return;
    const bool pressed = eng::Call(hit, "IsPressed").ReturnBool();
    b.hovered = eng::Call(hit, "IsHovered").ReturnBool();
    if (b.wasPressed && !pressed && b.hovered) {
        b.clickPending = true;
        hostlog::Info("main menu entry '" + b.label + "' clicked");
    }
    b.wasPressed = pressed;
}

}  // namespace

FooterButton* AddMenuButton(int owner, const std::string& label) {
    gMenuButtons.push_back(std::make_unique<FooterButton>());
    gMenuButtons.back()->owner = owner;
    gMenuButtons.back()->label = label;
    return gMenuButtons.back().get();
}

namespace menu {

void Frame() {
    if (gMenuButtons.empty()) return;
    if (!eng::Get(gGroup) || ++gFrame % 30 == 0) {
        Obj group = FindMainMenuGroup();
        if (group != eng::Get(gGroup)) {
            gGroup = eng::MakeWeak(group);
            gBox = eng::MakeWeak(group ? eng::ReadObj(group, "Vbox") : nullptr);
            gAdopted = game::Seconds();
            Forget();
        }
    }
    if (!eng::Get(gBox) || game::Seconds() - gAdopted < 2.5) return;
    int position = 0;
    for (auto& b : gMenuButtons) {
        Obj button = eng::Get(b->button);
        if (!button || eng::ReadObj(eng::ReadObj(button, "Slot"), "Parent") != eng::Get(gBox)) Build(*b, position);
        Track(*b);
        ++position;
    }
}

bool Shown() {
    Obj group = eng::Get(gGroup);
    if (!group) group = FindMainMenuGroup();
    return group && eng::Call(group, "IsVisible").ReturnAs<uint8_t>(0) != 0;
}

void RemoveOwner(int owner) {
    for (auto it = gMenuButtons.begin(); it != gMenuButtons.end();) {
        if ((*it)->owner != owner) {
            ++it;
            continue;
        }
        if (Obj button = eng::Get((*it)->button)) eng::Call(button, "RemoveFromParent");
        it = gMenuButtons.erase(it);
    }
}

bool SimulateClick(const std::string& label) {
    for (auto& b : gMenuButtons)
        if (b->label == label) return b->clickPending = true;
    return false;
}

std::string Status() {
    std::string s;
    for (auto& b : gMenuButtons) s += " menu[" + b->label + "]=" + (eng::Get(b->button) ? "placed" : "absent");
    return s;
}

}  // namespace menu
}  // namespace ui
