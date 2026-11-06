#include "createpage.hpp"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "game.hpp"
#include "log.hpp"
#include "widgets.hpp"

using eng::Obj;
namespace w = ui::widgets;

namespace createpage {
namespace {

constexpr uint8_t kVisible = 0;

struct Tab {
    int owner = -1;
    std::string label;
    eng::Weak create, button, page;
    bool wasPressed = false, open = false, openRequested = false, removed = false;
    std::vector<std::pair<eng::Weak, uint8_t>> hidden;     // the game's content while ours shows, with its visibility
};
std::vector<Tab> gTabs;
eng::Weak gCreate;
std::string gNote;                      // why the last build failed (for Status)
ULONGLONG gLastSearch = 0;

Obj CreatePageWidget() {
    if (Obj c = eng::Get(gCreate)) return c;
    if (GetTickCount64() - gLastSearch < 500) return nullptr;
    gLastSearch = GetTickCount64();
    // the Create page exists only on the main menu: no search (a scan of every object) anywhere else
    Obj controller = game::PlayerController();
    if (!controller) return nullptr;
    const eng::Params level = eng::Call(eng::FindCdo("GameplayStatics"), "GetCurrentLevelName", controller, uint8_t{1});
    const uint8_t* name = level.Return();
    if (!name || eng::ReadFString(name).find("MainMenu") == std::string::npos) return nullptr;
    Obj cls = eng::FindClass("WBP_4_Create_C"), found = nullptr;
    if (!cls) return nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::PathOf(o).find("/Engine/Transient") == 0) found = o;
        return found == nullptr;
    });
    gCreate = eng::MakeWeak(found);
    return found;
}

// A widget of the page's tree by its name (widgets that aren't the blueprint's variables have no property).
Obj Named(Obj userWidget, const char* name) {
    if (!userWidget) return nullptr;
    if (Obj o = eng::FindProp(eng::ClassOf(userWidget), name) ? eng::ReadObj(userWidget, name) : nullptr) return o;
    const std::wstring w(name, name + std::strlen(name));
    const eng::Params n = eng::Call(eng::FindCdo("KismetStringLibrary"), "Conv_StringToName",
                                    eng::FString{w.c_str(), static_cast<int32_t>(w.size() + 1), static_cast<int32_t>(w.size() + 1)});
    size_t size = 0;
    const uint8_t* fname = n.Return(&size);
    if (!fname) return nullptr;
    eng::Params get(eng::FunctionOn(userWidget, "GetWidgetFromName"));
    get.SetArg(0, fname, size);
    return eng::Invoke(userWidget, get) ? get.ReturnObj() : nullptr;
}

// The content column (VerticalBox_0: the "+new" bar and SizeBox_1, which holds the page switcher), found from the
// switcher up (measured: CommonActivatableWidgetSwitcher_1 < SizeBox_1 < VerticalBox_0).
Obj ContentColumn(Obj create) {
    if (Obj c = Named(create, "VerticalBox_0")) return c;
    Obj sizeBox = Named(create, "SizeBox_1");
    if (!sizeBox)
        if (Obj switcher = Named(create, "CommonActivatableWidgetSwitcher_1")) sizeBox = eng::Call(switcher, "GetParent").ReturnObj();
    Obj column = sizeBox ? eng::Call(sizeBox, "GetParent").ReturnObj() : nullptr;
    return column && eng::ObjName(eng::ClassOf(column)) == "VerticalBox" ? column : nullptr;
}

bool ReadActive(Obj label) {
    bool on = false;
    return label && eng::ReadBool(label, "bActive", &on) && on;
}

Obj NewLabel() {
    Obj controller = game::PlayerController(), cls = eng::FindClass("WBP_SubPageLabel_C");
    if (!controller || !cls) return nullptr;
    return eng::Call(eng::FindCdo("WidgetBlueprintLibrary"), "Create", controller, cls, controller).ReturnObj();
}

bool Build(Tab& t, Obj create) {
    Obj tabs = Named(create, "PLAYLISTTABS"), theirs = Named(create, "WBP_SubPagePublished");
    Obj content = ContentColumn(create), tree = eng::ReadObj(create, "WidgetTree");
    if (!tabs || !theirs || !content || !tree) {
        gNote = std::string("missing") + (tabs ? "" : " PLAYLISTTABS") + (theirs ? "" : " WBP_SubPagePublished") +
                (content ? "" : " VerticalBox_0") + (tree ? "" : " WidgetTree");
        return false;
    }
    Obj label = NewLabel();
    if (!label) {
        gNote = "no WBP_SubPageLabel_C made";
        return false;
    }
    // looks as "published" does (copied before it's built: its PreConstruct reads them)
    for (const char* name : {"TextJustification", "TextPadding", "TabType"}) {
        const eng::Prop p = eng::FindProp(eng::ClassOf(theirs), name);
        if (!p || p.size > 64) continue;
        uint8_t bytes[64];
        if (eng::ReadBytes(theirs, name, bytes, static_cast<size_t>(p.size))) eng::WriteBytes(label, name, bytes, static_cast<size_t>(p.size));
    }
    const eng::Params text = eng::MakeText(t.label);
    if (const uint8_t* ftext = text.Return()) {
        eng::WriteBytes(label, "DefaultText", ftext, 16);
    }
    Obj slot = eng::Call(tabs, "AddChildToStackBox", label).ReturnObj();
    if (!slot) slot = w::AddChild(tabs, label);
    if (!slot) {
        gNote = "the label couldn't be added to PLAYLISTTABS";
        return false;
    }
    if (Obj theirSlot = eng::ReadObj(Named(create, "WBP_SubPageFiles"), "Slot")) {
        w::Margin padding{};
        if (eng::ReadBytes(theirSlot, "Padding", &padding, sizeof padding)) {
            eng::Call(slot, "SetPadding", w::Margin{padding.left, padding.bottom, padding.right, 0});
        }
    }
    eng::Call(label, "SetIsActive", uint8_t{0});
    Obj page = w::Spawn("VerticalBox", tree);
    Obj pageSlot = page ? eng::Call(content, "AddChildToVerticalBox", page).ReturnObj() : nullptr;
    if (!pageSlot) {
        eng::Call(label, "RemoveFromParent");
        gNote = "the page couldn't be added to VerticalBox_0";
        return false;
    }
    gNote = "";
    w::FillSlot(pageSlot);
    w::SetVisibility(page, w::kCollapsed);
    t.create = eng::MakeWeak(create);
    t.button = eng::MakeWeak(label);
    t.page = eng::MakeWeak(page);
    t.open = t.wasPressed = false;
    t.hidden.clear();
    hostlog::Info("create page: plugin tab '" + t.label + "' added");
    return true;
}

bool Released(Obj label, bool* wasPressed) {
    Obj button = label ? eng::ReadObj(label, "Button_33") : nullptr;
    if (!button) return false;
    const bool pressed = eng::Call(button, "IsPressed").ReturnBool();
    const bool clicked = *wasPressed && !pressed && eng::Call(button, "IsHovered").ReturnBool();
    *wasPressed = pressed;
    return clicked;
}

void Show(Tab& t, bool open) {
    Obj create = eng::Get(t.create), page = eng::Get(t.page), label = eng::Get(t.button);
    Obj content = create ? ContentColumn(create) : nullptr;
    if (!create || !page || !content) return;
    if (open) {
        // every other tab of ours closes first
        for (auto& other : gTabs)
            if (&other != &t && other.open) Show(other, false);
        t.hidden.clear();
        // the game's own content in the column: the "+new" bar (BTN_Create) and the box of the page switcher; the
        // title above them stays
        for (Obj inside : {Named(create, "BTN_Create"), Named(create, "CommonActivatableWidgetSwitcher_1")}) {
            Obj child = inside;
            while (child) {
                Obj parent = eng::Call(child, "GetParent").ReturnObj();
                if (!parent || parent == content) break;
                child = parent;
            }
            if (!child || child == page || eng::Call(child, "GetParent").ReturnObj() != content) continue;
            const uint8_t was = eng::Call(child, "GetVisibility").ReturnAs<uint8_t>(kVisible);
            if (was == w::kCollapsed) continue;
            t.hidden.push_back({eng::MakeWeak(child), was});
            w::SetVisibility(child, w::kCollapsed);
        }
        w::SetVisibility(page, w::kSelfHitTestInvisible);
        for (const char* name : {"WBP_SubPageFiles", "WBP_SubPagePublished"})
            if (Obj theirs = Named(create, name)) eng::Call(theirs, "SetIsActive", uint8_t{0});
        eng::Call(label, "SetIsActive", uint8_t{1});
        eng::Call(label, "SetKeyboardFocus");       // the game's focus look leaves the label it was on
    } else {
        for (const auto& [weak, was] : t.hidden)
            if (Obj child = eng::Get(weak)) w::SetVisibility(child, was);
        t.hidden.clear();
        w::SetVisibility(page, w::kCollapsed);
        eng::Call(label, "SetIsActive", uint8_t{0});
    }
    t.open = open;
}

}  // namespace

void Frame() {
    bool any = false;
    for (const auto& t : gTabs) any = any || !t.removed;
    if (!any) return;
    Obj create = CreatePageWidget();
    if (!create) {
        for (auto& t : gTabs) t.open = false;
        return;
    }
    for (auto& t : gTabs) {
        if (t.removed) continue;
        if (eng::Get(t.create) != create || !eng::Get(t.button) || !eng::Get(t.page)) {
            if (Obj old = eng::Get(t.button)) eng::Call(old, "RemoveFromParent");
            if (Obj old = eng::Get(t.page)) eng::Call(old, "RemoveFromParent");
            t.button = t.page = t.create = {};
            t.open = false;
            if (!Build(t, create)) continue;
        }
        Obj label = eng::Get(t.button);
        if (Released(label, &t.wasPressed) || t.openRequested) {
            t.openRequested = false;
            Show(t, true);
        }
        // the game's own labels take the content back (their click handler marks them active)
        if (t.open && (ReadActive(Named(create, "WBP_SubPageFiles")) || ReadActive(Named(create, "WBP_SubPagePublished"))))
            Show(t, false);
    }
}

int AddTab(int owner, const std::string& label) {
    Tab t;
    t.owner = owner;
    t.label = label;
    gTabs.push_back(t);
    return static_cast<int>(gTabs.size()) - 1;
}

Obj TabPage(int tab) {
    if (tab < 0 || tab >= static_cast<int>(gTabs.size()) || gTabs[static_cast<size_t>(tab)].removed) return nullptr;
    const Tab& t = gTabs[static_cast<size_t>(tab)];
    return eng::Get(t.create) == eng::Get(gCreate) ? eng::Get(t.page) : nullptr;
}

bool TabOpen(int tab) {
    if (tab < 0 || tab >= static_cast<int>(gTabs.size())) return false;
    const Tab& t = gTabs[static_cast<size_t>(tab)];
    Obj create = eng::Get(t.create);
    return !t.removed && t.open && create && eng::Call(create, "IsVisible").ReturnBool();
}

void OpenTab(int tab) {
    if (tab >= 0 && tab < static_cast<int>(gTabs.size())) gTabs[static_cast<size_t>(tab)].openRequested = true;
}

void RemoveOwner(int owner) {
    for (auto& t : gTabs) {
        if (t.owner != owner || t.removed) continue;
        if (t.open) Show(t, false);
        if (Obj b = eng::Get(t.button)) eng::Call(b, "RemoveFromParent");
        if (Obj p = eng::Get(t.page)) eng::Call(p, "RemoveFromParent");
        t.removed = true;
    }
}

bool TabTapped(Obj userWidget) {
    for (auto& t : gTabs)
        if (!t.removed && userWidget && eng::Get(t.button) == userWidget) {
            t.openRequested = true;
            return true;
        }
    return false;
}

std::string Status() {
    std::string out;
    for (size_t i = 0; i < gTabs.size(); ++i) {
        const Tab& t = gTabs[i];
        out += "[" + std::to_string(i) + " '" + t.label + "'" + (t.removed ? " removed" : "") + (eng::Get(t.page) ? " built" : "") +
               (t.open ? " open" : "") + "]";
    }
    return (out.empty() ? "no create page tabs" : out) + (eng::Get(gCreate) ? " (page found)" : " (no page)") + (gNote.empty() ? "" : " " + gNote);
}

}  // namespace createpage
