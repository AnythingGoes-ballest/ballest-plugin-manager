// Custom campaigns in the game's own track selection. Read from the game's blueprints (AssetDumper + kismet_pseudo,
// game build of 2026-10-06) and measured in a sandboxed slot:
//
// - The play page is WBP_MainMenu_UIManager_C.WBP_Play_1 (WBP_0_Play_C), one of the UI manager's WidgetSwitcher
//   pages. Its sidebar tab bar is PLAYLISTTABS, a StackBox. InitializePlaylistTabs clears it and adds, made with
//   WidgetBlueprintLibrary.Create(self, WBP_SubPageLabel_C), one tab per playlist (circuit 0, trials 1), then hub
//   (Index 2) and collabs (Index 3), each with SetPadding(0, 8, 0, 8); the result is its "Used Tabs" array. Measured
//   on the four tabs: Font Size 24, TextPadding 16/8/16/8, TextJustification Right, Transform Policy ToLower,
//   ActionType 9, TabType 1 (playlist tabs and collabs) or 0 (hub).
// - WBP_SubPageLabel_C is a WBP_ButtonBase_C (the game's menu button). Its own blueprint does the hover (HitBox
//   OnHovered: keyboard focus, hover colours and animation, a sound), the click (OnClicked broadcast, click sound and
//   animation; if not bActive, InactiveButtonClicked(PlaylistData, Index)) and SetIsActive (the white "selected" look).
//   The play page binds InactiveButtonClicked of its Used Tabs only (at Construct) to: ActiveTabIndex = Index;
//   SwitchToPageMatchingTabIndex(), which SetIsActive(i == ActiveTabIndex) on every used tab and, for 0..3, sets the
//   header (UpdateHeader), the body (Body_WS 0 = LevelsPage, 1 = the hub) and hides the info panel. Tab left/right
//   input steps ActiveTabIndex by one, wrapping at the Used Tabs count.
// - So the host's tab is one more WBP_SubPageLabel_C, made and padded the same way and added to PLAYLISTTABS after the
//   game's; its clicks are read from its HitBox (pressed, then released while hovered), as the footer reads buttons.
//   Choosing it sets ActiveTabIndex to the Used Tabs count (no game tab has it, so SwitchToPageMatchingTabIndex turns
//   every game tab off and touches nothing else), then SetIsActive(true) on ours, and shows the host's page: a third
//   child of Body_WS (a ScrollBox in its 800 px column), with the header (TXT_PageHeader / TXT_PageSubheader) set.
//   When the game switches tabs (a click, or tab left/right from ours: ActiveTabIndex no longer ours), ours is turned
//   off and the game has already put its own page back.
// - Back: the UI manager's back button (BndEvt..._BackButtonClicked) and the back input (InputAction_TryExecuteBack-
//   Function) both enter its graph through ProcessEvent and switch the menu back to the main page. Both are wrapped
//   (their UFunction's native entry, as cosmetics.cpp does): on a campaign's screen the back goes to the campaign list
//   instead; anywhere else, the game's own back runs.
#include "campaigns.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "cosmetics.hpp"
#include "engine.hpp"
#include "game.hpp"
#include "hub.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "testchannel.hpp"
#include "ui.hpp"
#include "widgets.hpp"

using eng::Obj;
using ui::Color;
namespace w = ui::widgets;

namespace campaigns {
namespace {

const char* const kFont = "/Game/UI/Fonts/CocogoosePro.CocogoosePro";
const char* const kTabLabel = "custom campaigns";
constexpr Color kWhite{1, 1, 1, 1};
constexpr Color kSoft{0.72f, 0.72f, 0.75f, 1};
constexpr Color kBlack{0, 0, 0, 1};
constexpr Color kCard{0.012f, 0.012f, 0.016f, 1};             // the game's dark track cards
constexpr Color kCardHover{0.0f, 0.107f, 0.552f, 1};          // the sidebar's blue (sRGB 0,92,196 in a screenshot)
constexpr Color kBar{0.62f, 0.62f, 0.64f, 1};                 // the "Previous Season" bar's light grey
constexpr Color kBarHover{0.9f, 0.9f, 0.92f, 1};
constexpr Color kNoImage{0.02f, 0.022f, 0.03f, 1};
constexpr float kListTop = 34;          // the list's gap below the page header (see Build)

struct Course {
    std::string title, image, info, detail;
};
// A campaign's colours (Campaigns::SetTheme): "key=#RRGGBB" pairs, sRGB as designers write them, kept linear as UMG
// takes them. A key not given falls back as ThemeOf says.
using Theme = std::map<std::string, Color>;
const char* const kThemeKeys[] = {"card", "hover", "pressed", "text", "body", "hovertext", "accent", "number", "coursecard", "coursehover"};

struct Campaign {
    int id = 0, owner = -1;
    std::string title, image, description, note;
    Theme theme;
    std::vector<Course> courses;
    bool opened = false;
    int picked = -1;
};
std::vector<Campaign> gCampaigns;
int gNextId = 1;

enum class View { None, List, Campaign };
View gView = View::None;
int gShownId = 0;                   // the campaign whose screen is shown (View::Campaign)
bool gDirty = false;                // the page shows something that changed: build it again

// Widgets of the play page now (rebuilt whenever the game makes a new one).
eng::Weak gManager, gPlay, gTab, gPage;
int gOurIndex = 4;                  // ActiveTabIndex while ours is chosen: the Used Tabs count
bool gTabWasPressed = false;
float gHeaderSize = 0;              // TXT_PageHeader's own font size, put back when the page is left

// Buttons on the page.
enum class Action { Back, Open, Pick };
struct TextLook {
    eng::Weak text;
    Color normal, hover;
};
struct CardLook {                   // a card's colours in its three states, and its border
    Color normal, hover, pressed;
    bool bordered = false;
    Color border{};
};
struct Clickable {
    eng::Weak button;
    Action action;
    int value;
    CardLook look;
    std::vector<TextLook> texts;
    int state = 0;                  // shown now: 0 normal, 1 hovered, 2 pressed
    bool wasPressed = false;
};
std::vector<Clickable> gClicks;
int gForceHover = -1;               // test: the card shown hovered (posted mouse moves don't reach Slate)

// Requests (from tests, plugins, and the back wrapper, which runs inside the game's own call).
bool gTabClick = false;
int gOpenIndex = -1, gPickIndex = -1;
volatile bool gBackPending = false;
volatile bool gCampaignOnScreen = false;    // read by the back wrapper: a campaign's screen is on screen
int gShowId = 0;
double gShowSince = 0, gPlayOnScreenSince = -1;
bool gShowCalledPlay = false;

Campaign* Find(int id) {
    for (auto& c : gCampaigns)
        if (c.id == id) return &c;
    return nullptr;
}
Campaign* Owned(int owner, int id) {
    Campaign* c = Find(id);
    return c && c->owner == owner ? c : nullptr;
}

// --- the game's objects -----------------------------------------------------------------------------------------

Obj Manager() {
    static double lastLook = -100;
    if (Obj m = eng::Get(gManager)) return m;
    if (game::Seconds() - lastLook < 1) return nullptr;
    lastLook = game::Seconds();
    Obj cls = eng::FindClass("WBP_MainMenu_UIManager_C");
    if (!cls) return nullptr;
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::PathOf(o).rfind("/Engine/Transient", 0) == 0) found = o;
        return found == nullptr;
    });
    gManager = eng::MakeWeak(found);
    return found;
}

void CopyProp(Obj from, Obj to, const char* name, size_t size) {
    std::vector<uint8_t> bytes(size);
    if (eng::ReadBytes(from, name, bytes.data(), size)) eng::WriteBytes(to, name, bytes.data(), size);
}

// A textured image (a file, kept for the session, or a game texture), or null.
Obj Texture(const std::string& path) {
    static std::map<std::string, eng::Weak> loaded;
    if (path.empty()) return nullptr;
    if (Obj t = eng::Get(loaded[path])) return t;
    Obj t = path.rfind("/Game/", 0) == 0 ? cosmetics::LoadAsset(eng::Widen(path)) : cosmetics::LoadTexture(eng::Widen(path));
    if (!t) hostlog::Warn("campaigns: image could not be loaded: " + path);
    loaded[path] = eng::MakeWeak(t);
    return t;
}

// UObject property of type FText set the way the game sets its tabs' (KismetSystemLibrary.SetTextPropertyByName).
void SetTextProperty(Obj object, const char* property, const std::string& value) {
    Obj system = eng::FindCdo("KismetSystemLibrary"), strings = eng::FindCdo("KismetStringLibrary");
    const std::wstring wide = eng::Widen(property);
    const eng::FString fs{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
    const eng::Params name = eng::Call(strings, "Conv_StringToName", fs);
    const eng::Params text = eng::MakeText(value);
    size_t nameSize = 0, textSize = 0;
    const uint8_t* n = name.Return(&nameSize);
    const uint8_t* t = text.Return(&textSize);
    if (!n || !t) return;
    eng::Params set(eng::FunctionOn(system, "SetTextPropertyByName"));
    set.SetArg(0, object);
    set.SetArg(1, n, nameSize);
    set.SetArg(2, t, textSize);
    if (eng::Invoke(system, set)) eng::ReleaseText(t);
}

// The header's font size, through SetFont (a live text block takes a new font only through its setter).
float FontSize(Obj text) {
    eng::Prop last;
    const int off = text ? eng::NestedOffset(eng::ClassOf(text), {"Font", "Size"}, &last) : -1;
    float size = 0;
    if (off >= 0 && last.size == sizeof size) std::memcpy(&size, text + off, sizeof size);
    return size;
}
void SetFontSize(Obj text, float size) {
    const int font = text ? eng::NestedOffset(eng::ClassOf(text), {"Font"}) : -1;
    const int sizeAt = text ? eng::NestedOffset(eng::ClassOf(text), {"Font", "Size"}) : -1;
    if (font < 0 || sizeAt < font) return;
    eng::Params p(eng::FunctionOn(text, "SetFont"));
    const int bytes = p.SizeOf("InFontInfo");
    if (bytes <= sizeAt - font) return;
    std::vector<uint8_t> info(text + font, text + font + bytes);
    std::memcpy(info.data() + (sizeAt - font), &size, sizeof size);
    p.SetArg(0, info.data(), info.size());
    eng::Invoke(text, p);
}

// --- the tab --------------------------------------------------------------------------------------------------------

void RemoveTab() {
    if (Obj tab = eng::Get(gTab)) eng::Call(tab, "RemoveFromParent");
    gTab = {};
}

// Our tab is the tab bar's last child; made again when the game rebuilt its tabs (or made a new play page).
void EnsureTab(Obj play) {
    Obj box = eng::ReadObj(play, "PLAYLISTTABS");
    const auto used = eng::ReadObjArray(play, "Used Tabs");
    if (!box || used.empty()) return;
    gOurIndex = static_cast<int>(used.size());
    const auto slots = eng::ReadObjArray(box, "Slots");
    Obj last = slots.empty() ? nullptr : eng::ReadObj(slots.back(), "Content");
    Obj tab = eng::Get(gTab);
    if (tab && last == tab) return;
    // Wait for the game's own tabs to be in the bar (they are added after a 0.2 s delay at Construct).
    for (Obj t : used) {
        Obj slot = eng::ReadObj(t, "Slot");
        if (!slot || eng::ReadObj(slot, "Parent") != box) return;
    }
    if (tab) {
        eng::Call(tab, "RemoveFromParent");     // in the middle of a rebuilt bar: back to the end
    } else {
        Obj cls = eng::FindClass("WBP_SubPageLabel_C");
        Obj reference = used.back();
        if (!cls || eng::ClassOf(reference) != cls) {
            static bool warned = false;
            if (!warned) hostlog::Warn("campaigns: the play page's tabs are not as measured; no custom campaigns tab");
            warned = true;
            return;
        }
        tab = eng::Call(eng::FindCdo("WidgetBlueprintLibrary"), "Create", play, cls, game::PlayerController()).ReturnObj();
        if (!tab) return;
        CopyProp(reference, tab, "Font Size", 4);
        CopyProp(reference, tab, "MinWidth", 4);
        CopyProp(reference, tab, "TextPadding", 16);
        CopyProp(reference, tab, "TextJustification", 1);
        CopyProp(reference, tab, "Transform Policy", 1);
        CopyProp(reference, tab, "TypeSelection", 4);
        CopyProp(reference, tab, "ActionType", 1);
        CopyProp(reference, tab, "BGMaterial", 8);
        // "custom campaigns" is wider than the game's labels: at their 24 points it overflows the 320 px bar
        // (measured: "ustom campaigns" shown). A smaller size and less left padding fit it.
        float size = 24;
        eng::ReadBytes(reference, "Font Size", &size, sizeof size);
        size *= 0.8f;
        eng::WriteBytes(tab, "Font Size", &size, sizeof size);
        w::Margin textPadding{16, 8, 16, 8};
        eng::ReadBytes(reference, "TextPadding", &textPadding, sizeof textPadding);
        textPadding.left = 4;
        eng::WriteBytes(tab, "TextPadding", &textPadding, sizeof textPadding);
        const uint8_t tabType = 0;          // not a playlist tab (GetPlaylistTabs looks for TabType 1)
        eng::WriteBytes(tab, "TabType", &tabType, 1);
        eng::WriteBytes(tab, "Index", &gOurIndex, sizeof gOurIndex);
        SetTextProperty(tab, "DefaultText", kTabLabel);     // what its PreConstruct shows
    }
    if (!w::AddChild(box, tab)) return;
    w::Margin padding{0, 8, 0, 8};
    eng::ReadBytes(used.back(), "Padding", &padding, sizeof padding);
    eng::Call(tab, "SetPadding", padding);
    w::SetText(eng::ReadObj(tab, "ButtonText"), kTabLabel);
    eng::Call(tab, "SetIsActive", gView != View::None);
    gTab = eng::MakeWeak(tab);
    gTabWasPressed = false;
    hostlog::Info("campaigns: tab placed (after " + std::to_string(used.size()) + " game tabs)");
}

void TrackTab() {
    Obj tab = eng::Get(gTab);
    Obj hit = tab ? eng::ReadObj(tab, "HitBox") : nullptr;
    if (!hit) return;
    const bool pressed = eng::Call(hit, "IsPressed").ReturnBool();
    if (gTabWasPressed && !pressed && eng::Call(hit, "IsHovered").ReturnBool()) gTabClick = true;
    gTabWasPressed = pressed;
}

// --- the page -------------------------------------------------------------------------------------------------------

Obj Text(Obj tree, const std::string& s, float size, Color c, const char* typeface = "Regular", bool wrap = false) {
    Obj t = w::Spawn("TextBlock", tree);
    if (!t) return nullptr;
    w::SetFontFace(t, kFont, typeface);
    w::SetFontSize(t, size);
    w::SetText(t, s);
    w::SetTextColor(t, c);
    if (wrap) eng::Call(t, "SetAutoWrapText", true);
    w::SetVisibility(t, w::kHitTestInvisible);
    return t;
}

// A picture fitted inside a width x height box, keeping its proportions, centred (the texture's own size, read from it).
Obj Picture(Obj tree, const std::string& path, float width, float height) {
    Obj box = w::Spawn("SizeBox", tree), inner = w::Spawn("SizeBox", tree), image = w::Spawn("Image", tree);
    if (!box || !inner || !image) return nullptr;
    eng::Call(box, "SetWidthOverride", width);
    eng::Call(box, "SetHeightOverride", height);
    Obj texture = Texture(path);
    float fitW = width, fitH = height;
    if (texture) {
        eng::Call(image, "SetBrushFromTexture", texture, uint8_t{0});
        const int32_t sx = eng::Call(texture, "Blueprint_GetSizeX").ReturnAs<int32_t>(0);
        const int32_t sy = eng::Call(texture, "Blueprint_GetSizeY").ReturnAs<int32_t>(0);
        if (sx > 0 && sy > 0) {
            const float scale = std::min(width / static_cast<float>(sx), height / static_cast<float>(sy));
            fitW = static_cast<float>(sx) * scale;
            fitH = static_cast<float>(sy) * scale;
        }
    }
    eng::Call(image, "SetColorAndOpacity", texture ? kWhite : kNoImage);
    w::SetVisibility(image, w::kHitTestInvisible);
    eng::Call(inner, "SetWidthOverride", fitW);
    eng::Call(inner, "SetHeightOverride", fitH);
    w::AddChild(inner, image);
    if (Obj slot = w::AddChild(box, inner)) {
        eng::Call(slot, "SetHorizontalAlignment", w::kAlignCenter);
        eng::Call(slot, "SetVerticalAlignment", w::kAlignCenter);
    }
    return box;
}

// The theme's colour for a key, else its fallback.
Color Look(const Theme& t, const char* key, Color fallback) {
    const auto it = t.find(key);
    return it == t.end() ? fallback : it->second;
}

// A clickable card: a rounded button holding `content` (in a rounded accent border when the look has one), added to
// the page's list. Its colour follows its state exactly: the button's own hover and press tints are set to none.
void AddButton(Obj tree, Obj list, Obj content, Action action, int value, const CardLook& look, std::vector<TextLook> texts, double radius,
               w::Margin inner, float gapBelow) {
    Obj button = w::Spawn("Button", tree);
    if (!button || !content) return;
    w::RoundButton(button, radius);
    for (const char* state : {"Normal", "Hovered", "Pressed"}) w::WriteSlateColor(button, {"WidgetStyle", state, "TintColor"}, kWhite);
    w::Unfocusable(button);
    eng::Call(button, "SetBackgroundColor", look.normal);
    if (Obj slot = w::AddChild(button, content)) {
        eng::Call(slot, "SetPadding", inner);
        eng::Call(slot, "SetHorizontalAlignment", w::kAlignFill);
    }
    Obj outer = button;
    if (look.bordered) {
        constexpr float kBorder = 3;
        if (Obj border = w::Spawn("Border", tree)) {
            w::RoundCorners(border, radius + kBorder);
            eng::Call(border, "SetBrushColor", look.border);
            eng::Call(border, "SetPadding", w::Margin{kBorder, kBorder, kBorder, kBorder});
            if (Obj slot = w::AddChild(border, button)) {
                eng::Call(slot, "SetHorizontalAlignment", w::kAlignFill);
                eng::Call(slot, "SetVerticalAlignment", w::kAlignFill);
            }
            outer = border;
        }
    }
    if (Obj slot = eng::Call(list, "AddChildToVerticalBox", outer).ReturnObj()) eng::Call(slot, "SetPadding", w::Margin{8, 0, 8, gapBelow});
    Clickable c;
    c.button = eng::MakeWeak(button);
    c.action = action;
    c.value = value;
    c.look = look;
    c.texts = std::move(texts);
    gClicks.push_back(std::move(c));
}

struct RowColors {
    Color number, title, body, numberHover, titleHover, bodyHover;
};

// A card's row: [number] [picture] [title over text, filling] [right-hand text]. Its texts go to `texts`, to be
// recoloured on hover.
Obj CardRow(Obj tree, const std::string& number, const std::string& image, float imageW, float imageH, const std::string& title,
            float titleSize, const std::string& below, const std::string& right, const RowColors& k, std::vector<TextLook>& texts) {
    Obj row = w::Spawn("HorizontalBox", tree), column = w::Spawn("VerticalBox", tree);
    if (!row || !column) return nullptr;
    auto text = [&](const std::string& s, float size, Color normal, Color hover, bool wrap) {
        Obj t = Text(tree, s, size, normal, "Regular", wrap);
        texts.push_back({eng::MakeWeak(t), normal, hover});
        return t;
    };
    float gap = 0;
    if (!number.empty()) {
        Obj box = w::Spawn("SizeBox", tree);
        eng::Call(box, "SetWidthOverride", 56.0f);
        Obj n = text(number, 40, k.number, k.numberHover, false);
        eng::Call(n, "SetJustification", uint8_t{1});       // ETextJustify::Center
        w::AddChild(box, n);
        w::AddToRow(row, box, 0);
        gap = 12;
    }
    w::AddToRow(row, Picture(tree, image, imageW, imageH), gap);
    eng::Call(column, "AddChildToVerticalBox", text(title, titleSize, k.title, k.titleHover, false));
    if (!below.empty())
        if (Obj slot = eng::Call(column, "AddChildToVerticalBox", text(below, 20, k.body, k.bodyHover, true)).ReturnObj())
            eng::Call(slot, "SetPadding", w::Margin{0, 6, 0, 0});
    w::FillSlot(w::AddToRow(row, column, 24));
    if (!right.empty()) w::AddToRow(row, text(right, 30, k.title, k.titleHover, false), 16);
    return row;
}

// A campaign's card look and text colours: on the list (course = false) or for its courses.
void ThemeOf(const Theme& t, bool course, CardLook* look, RowColors* colors) {
    const Color card = Look(t, "card", kCard), hover = Look(t, "hover", kCardHover);
    look->normal = course ? Look(t, "coursecard", card) : card;
    look->hover = course ? Look(t, "coursehover", hover) : hover;
    const Color h = look->hover;
    look->pressed = Look(t, "pressed", Color{h.r * 0.75f, h.g * 0.75f, h.b * 0.75f, h.a});
    look->bordered = t.count("accent") > 0;
    look->border = Look(t, "accent", kWhite);
    const Color text = Look(t, "text", kWhite), body = Look(t, "body", kSoft);
    const Color hoverText = Look(t, "hovertext", text);
    colors->title = text;
    colors->body = body;
    colors->number = Look(t, "number", text);
    colors->titleHover = hoverText;
    colors->bodyHover = t.count("hovertext") ? hoverText : body;
    colors->numberHover = t.count("hovertext") ? hoverText : colors->number;
}

void SetHeader(Obj play, const std::string& title, const std::string& sub) {
    Obj header = eng::ReadObj(play, "TXT_PageHeader");
    if (!header) return;
    if (gHeaderSize <= 0) gHeaderSize = FontSize(header);
    w::SetText(header, title);
    w::SetText(eng::ReadObj(play, "TXT_PageSubheader"), sub);
    // The header is the game's 116 point title; a long campaign title is made smaller to stay in its column.
    const float base = gHeaderSize > 0 ? gHeaderSize : 116;
    const size_t fits = 10;
    float size = title.size() > fits ? base * static_cast<float>(fits) / static_cast<float>(title.size()) : base;
    if (size < base * 0.45f) size = base * 0.45f;
    if (FontSize(header) != size) SetFontSize(header, size);
}

void RestoreHeader(Obj play) {
    Obj header = play ? eng::ReadObj(play, "TXT_PageHeader") : nullptr;
    if (header && gHeaderSize > 0 && FontSize(header) != gHeaderSize) SetFontSize(header, gHeaderSize);
}

// The page in the play page's body: a ScrollBox, the third child of Body_WS.
Obj EnsurePage(Obj play) {
    Obj body = eng::ReadObj(play, "Body_WS");
    Obj tree = eng::ReadObj(play, "WidgetTree");
    if (!body || !tree) return nullptr;
    Obj page = eng::Get(gPage);
    if (page && eng::ReadObj(eng::ReadObj(page, "Slot"), "Parent") == body) return page;
    page = w::Spawn("ScrollBox", tree);
    if (!page || !w::AddChild(body, page)) return nullptr;
    gPage = eng::MakeWeak(page);
    return page;
}

void Build(Obj play) {
    Obj page = EnsurePage(play);
    Obj tree = eng::ReadObj(play, "WidgetTree");
    if (!page || !tree) return;
    eng::Call(page, "ClearChildren");
    gClicks.clear();
    Obj list = w::Spawn("VerticalBox", tree);
    if (!list) return;
    // Below the page header: the game's 116 point title has descenders ("g", "p") that reach below its box, into the
    // body's top (measured: the "g" of "Campaigns" over the first card), so the list starts lower.
    if (Obj slot = w::AddChild(page, list)) eng::Call(slot, "SetPadding", w::Margin{0, kListTop, 8, 24});
    const Campaign* c = gView == View::Campaign ? Find(gShownId) : nullptr;
    if (gView == View::Campaign && !c) gView = View::List;      // its plugin went
    if (gView == View::List) {
        SetHeader(play, "Campaigns", "custom");
        for (size_t i = 0; i < gCampaigns.size(); ++i) {
            const Campaign& k = gCampaigns[i];
            CardLook look;
            RowColors colors;
            ThemeOf(k.theme, false, &look, &colors);
            std::vector<TextLook> texts;
            Obj row = CardRow(tree, "", k.image, 256, 144, k.title, 40, k.description, "", colors, texts);
            AddButton(tree, list, row, Action::Open, static_cast<int>(i), look, std::move(texts), 14, w::Margin{14, 14, 24, 14}, 14);
        }
        return;
    }
    SetHeader(play, c->title, "campaign");
    Obj back = w::Spawn("HorizontalBox", tree);
    w::AddToRow(back, Text(tree, "<  custom campaigns", 24, kBlack, "Regular"), 0);
    CardLook bar{kBar, kBarHover, kBar};
    AddButton(tree, list, back, Action::Back, 0, bar, {}, 2, w::Margin{14, 4, 14, 4}, c->note.empty() ? 14.0f : 6.0f);
    if (!c->note.empty())
        if (Obj slot = eng::Call(list, "AddChildToVerticalBox", Text(tree, c->note, 24, kBlack, "Regular", true)).ReturnObj())
            eng::Call(slot, "SetPadding", w::Margin{14, 0, 8, 12});
    if (c->courses.empty())
        if (Obj slot = eng::Call(list, "AddChildToVerticalBox", Text(tree, "no courses yet", 24, kBlack)).ReturnObj())
            eng::Call(slot, "SetPadding", w::Margin{14, 0, 8, 12});
    for (size_t i = 0; i < c->courses.size(); ++i) {
        const Course& k = c->courses[i];
        CardLook look;
        RowColors colors;
        ThemeOf(c->theme, true, &look, &colors);
        std::vector<TextLook> texts;
        Obj row = CardRow(tree, std::to_string(i + 1), k.image, 192, 108, k.title, 34, k.detail, k.info, colors, texts);
        AddButton(tree, list, row, Action::Pick, static_cast<int>(i), look, std::move(texts), 14, w::Margin{10, 10, 24, 10}, 12);
    }
}

void Activate(Obj play, View view, int id) {
    Obj tab = eng::Get(gTab);
    Obj body = eng::ReadObj(play, "Body_WS");
    if (!tab || !body) return;
    const bool entering = gView == View::None;
    gView = view;
    gShownId = id;
    if (entering) {
        const auto used = eng::ReadObjArray(play, "Used Tabs");
        gOurIndex = static_cast<int>(used.size());
        eng::WriteBytes(play, "ActiveTabIndex", &gOurIndex, sizeof gOurIndex);
        eng::Call(play, "SwitchToPageMatchingTabIndex");    // every game tab off (no game tab has our index)
        eng::Call(tab, "SetIsActive", true);
        // The game gives the chosen tab keyboard focus (SwitchToPageMatchingTabIndex: Hitbox.SetKeyboardFocus), which
        // takes the focus look off the tab that had it.
        if (Obj hit = eng::ReadObj(tab, "HitBox")) eng::Call(hit, "SetKeyboardFocus");
    }
    Build(play);
    if (Obj page = eng::Get(gPage)) eng::Call(body, "SetActiveWidget", page);
    eng::Call(play, "SwitchInfoPanelPage", uint8_t{3});     // EInfoPanel 3: hidden, as the game's own tabs do
    if (view == View::Campaign)
        if (Campaign* c = Find(id)) c->opened = true;
    gDirty = false;
    hostlog::Info(std::string("campaigns: ") + (view == View::List ? "list shown" : "campaign " + std::to_string(id) + " shown"));
}

// Our tab unselected and without the focus look. A simulated tab switch doesn't move the focus away the way a real
// click does, and the look is the HoverAnim played forward (measured: clearing bHasFocus left it white), so the tab's
// own OnRemovedFromFocusPath runs: bHasFocus off, colours by bActive, HoverAnim played back.
void TabOff() {
    Obj tab = eng::Get(gTab);
    if (!tab) return;
    eng::Call(tab, "SetIsActive", false);
    bool focused = false;
    if (eng::ReadBool(tab, "bHasFocus", &focused) && focused) eng::Call(tab, "OnRemovedFromFocusPath");
}

// The circuit tab, as a click on it does (the play page is shown again after it was left from ours).
void ShowCircuit(Obj play) {
    const auto used = eng::ReadObjArray(play, "Used Tabs");
    if (used.empty()) return;
    eng::Call(used.front(), "SimulateButtonClicked");
    if (Obj hit = eng::ReadObj(used.front(), "HitBox")) eng::Call(hit, "SetKeyboardFocus");
    TabOff();
}

// The page was left: by the game's own tab switch, or the play page went (back to the menu, or a race).
void Leave(Obj play) {
    if (gView == View::None) return;
    gView = View::None;
    gShownId = 0;
    gClicks.clear();
    TabOff();
    RestoreHeader(play);
    hostlog::Info("campaigns: page left");
}

void TrackClicks(Obj play) {
    for (size_t i = 0; i < gClicks.size(); ++i) {
        Clickable& c = gClicks[i];
        Obj b = eng::Get(c.button);
        if (!b) continue;
        const bool pressed = eng::Call(b, "IsPressed").ReturnBool();
        const bool hovered = eng::Call(b, "IsHovered").ReturnBool() || gForceHover == static_cast<int>(i);
        const int state = pressed ? 2 : hovered ? 1 : 0;
        if (state != c.state) {
            eng::Call(b, "SetBackgroundColor", state == 2 ? c.look.pressed : state == 1 ? c.look.hover : c.look.normal);
            for (const auto& t : c.texts) w::SetTextColor(eng::Get(t.text), state == 0 ? t.normal : t.hover);
            c.state = state;
        }
        const bool clicked = c.wasPressed && !pressed && hovered;
        c.wasPressed = pressed;
        if (!clicked) continue;
        if (c.action == Action::Back) gBackPending = true;
        if (c.action == Action::Open) gOpenIndex = c.value;
        if (c.action == Action::Pick) gPickIndex = c.value;
    }
    (void)play;
}

// --- back -----------------------------------------------------------------------------------------------------------
using NativeFunction = void (*)(Obj context, uint8_t* frame, void* result);
NativeFunction gBackOriginal = nullptr;
const char* const kBackButton = "BndEvt__WBP_MainMenu_UIManager_WBP_BackButton_K2Node_ComponentBoundEvent_0_BackButtonClicked__DelegateSignature";
// Where a back enters through ProcessEvent (measured: the menu's back button, and Escape / the pad's back, which is
// IA_Pause on the menu's player controller; that event calls InputAction_TryExecuteBackFunction from its own graph,
// which doesn't pass the native entry, so the input event itself is wrapped). On a campaign's screen these are skipped
// and the host shows the campaign list.
struct Wrapped {
    const char* cls;
    const char* name;
    Obj fn = nullptr;               // the UFunction wrapped now (classes of the menu map can be loaded again)
    int calls = 0;
};
Wrapped gWrapped[] = {{"WBP_MainMenu_UIManager_C", kBackButton},
                      {"WBP_MainMenu_UIManager_C", "InputAction_TryExecuteBackFunction"},
                      {"BP_MainMenuController_C", "InpActEvt_IA_Pause_K2Node_EnhancedInputActionEvent_1"}};
bool gWrapFailed = false;

// Runs inside the game's own call, outside the host's frame: only reads and sets flags.
void BackHooked(Obj context, uint8_t* frame, void* result) {
    Obj node = nullptr;
    if (frame) std::memcpy(&node, frame + layout::kFFrameFunctionOffset, sizeof node);
    for (auto& w : gWrapped) {
        if (w.fn != node) continue;
        ++w.calls;
        if (!gCampaignOnScreen) break;
        gBackPending = true;            // handled in the next frame: the campaign list again
        return;
    }
    gBackOriginal(context, frame, result);
}

void WrapBack() {
    static double lastLook = -100;
    if (gWrapFailed || game::Seconds() - lastLook < 2) return;
    lastLook = game::Seconds();
    for (auto& w : gWrapped) {
        Obj fn = eng::FindFunction(eng::FindClass(w.cls), w.name);
        if (!fn || fn == w.fn) continue;
        NativeFunction e = nullptr;
        std::memcpy(&e, fn + layout::kUFunctionNativeFunctionOffset, sizeof e);
        if (e == &BackHooked) {
            w.fn = fn;
            continue;
        }
        // All are Blueprint functions: the same interpreter entry in the game exe. Anything else is not as measured.
        if (!e || (gBackOriginal && e != gBackOriginal) || !eng::InImage(reinterpret_cast<void*>(e))) {
            hostlog::Warn(std::string("campaigns: ") + w.name + " is not as measured; back from a campaign goes to the main menu");
            gWrapFailed = true;
            return;
        }
        gBackOriginal = e;
        const NativeFunction hooked = &BackHooked;
        std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
        w.fn = fn;
        hostlog::Info(std::string("campaigns: watching back: ") + w.cls + "." + w.name);
    }
}

}  // namespace

// --- the plugin side ------------------------------------------------------------------------------------------------

int Register(int owner, const std::string& title, const std::string& image, const std::string& description) {
    Campaign c;
    c.id = gNextId++;
    c.owner = owner;
    c.title = title;
    c.image = image;
    c.description = description;
    gCampaigns.push_back(c);
    if (gView == View::List) gDirty = true;
    hostlog::Info("campaigns: '" + title + "' registered (id " + std::to_string(c.id) + ")");
    return c.id;
}

bool Remove(int owner, int id) {
    for (auto it = gCampaigns.begin(); it != gCampaigns.end(); ++it)
        if (it->id == id && it->owner == owner) {
            gCampaigns.erase(it);
            if (gView != View::None) gDirty = true;
            return true;
        }
    return false;
}

int AddCourse(int owner, int id, const std::string& title, const std::string& image) {
    Campaign* c = Owned(owner, id);
    if (!c) return -1;
    c->courses.push_back({title, image, "", ""});
    if (gView == View::Campaign && gShownId == id) gDirty = true;
    return static_cast<int>(c->courses.size()) - 1;
}

bool SetCourseInfo(int owner, int id, int course, const std::string& info) {
    Campaign* c = Owned(owner, id);
    if (!c || course < 0 || course >= static_cast<int>(c->courses.size())) return false;
    if (c->courses[course].info == info) return true;
    c->courses[course].info = info;
    if (gView == View::Campaign && gShownId == id) gDirty = true;
    return true;
}

bool SetCourseDetail(int owner, int id, int course, const std::string& detail) {
    Campaign* c = Owned(owner, id);
    if (!c || course < 0 || course >= static_cast<int>(c->courses.size())) return false;
    if (c->courses[course].detail == detail) return true;
    c->courses[course].detail = detail;
    if (gView == View::Campaign && gShownId == id) gDirty = true;
    return true;
}

bool SetNote(int owner, int id, const std::string& note) {
    Campaign* c = Owned(owner, id);
    if (!c) return false;
    if (c->note == note) return true;
    c->note = note;
    if (gView == View::Campaign && gShownId == id) gDirty = true;
    return true;
}

namespace {
float Linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
bool ParseColor(const std::string& v, Color* out) {
    if ((v.size() != 7 && v.size() != 9) || v[0] != '#') return false;
    float ch[4] = {0, 0, 0, 1};
    for (size_t i = 0; i * 2 + 1 < v.size(); ++i) {
        const std::string pair = v.substr(1 + i * 2, 2);
        char* end = nullptr;
        const long n = std::strtol(pair.c_str(), &end, 16);
        if (!end || *end) return false;
        ch[i] = static_cast<float>(n) / 255.0f;
    }
    *out = {Linear(ch[0]), Linear(ch[1]), Linear(ch[2]), ch[3]};     // alpha stays as given
    return true;
}
bool Separator(char c) { return c == ' ' || c == ';' || c == ',' || c == '\t'; }
}  // namespace

bool SetTheme(int owner, int id, const std::string& theme, std::string* error) {
    Campaign* c = Owned(owner, id);
    if (!c) {
        *error = "no campaign " + std::to_string(id) + " of this plugin";
        return false;
    }
    Theme parsed;
    size_t i = 0;
    while (i < theme.size()) {
        while (i < theme.size() && Separator(theme[i])) ++i;
        size_t end = i;
        while (end < theme.size() && !Separator(theme[end])) ++end;
        if (end == i) break;
        const std::string pair = theme.substr(i, end - i);
        i = end;
        const size_t eq = pair.find('=');
        const std::string key = eq == std::string::npos ? pair : pair.substr(0, eq);
        bool known = false;
        for (const char* k : kThemeKeys) known = known || key == k;
        Color color{};
        if (!known || eq == std::string::npos || !ParseColor(pair.substr(eq + 1), &color)) {
            *error = "\"" + pair + "\" is not key=#RRGGBB or key=#RRGGBBAA with a known key";
            return false;
        }
        parsed[key] = color;
    }
    c->theme = parsed;
    if (gView != View::None) gDirty = true;
    return true;
}

bool TakeOpened(int owner, int id) {
    Campaign* c = Owned(owner, id);
    if (!c || !c->opened) return false;
    c->opened = false;
    return true;
}

int TakePicked(int owner, int id) {
    Campaign* c = Owned(owner, id);
    if (!c) return -1;
    const int picked = c->picked;
    c->picked = -1;
    return picked;
}

bool Show(int owner, int id) {
    if (!Owned(owner, id)) return false;
    gShowId = id;
    gShowSince = game::Seconds();
    gShowCalledPlay = false;
    return true;
}

bool Shown(int id) { return gCampaignOnScreen && gShownId == id; }

bool ListShown() {
    Obj page = eng::Get(gPage);
    return gView == View::List && page && hub::WidgetOnScreen(page);
}

void RemoveOwner(int owner) {
    for (auto it = gCampaigns.begin(); it != gCampaigns.end();) {
        if (it->owner == owner) {
            it = gCampaigns.erase(it);
            if (gView != View::None) gDirty = true;
        } else {
            ++it;
        }
    }
    if (gShowId && !Find(gShowId)) gShowId = 0;
}

// --- every frame ----------------------------------------------------------------------------------------------------

void Frame() {
    WrapBack();
    Obj manager = Manager();
    Obj play = manager ? eng::ReadObj(manager, "WBP_Play_1") : nullptr;
    if (play != eng::Get(gPlay)) {          // a new menu (or none): its widgets are new
        gPlay = eng::MakeWeak(play);
        gTab = gPage = {};
        gView = View::None;
        gClicks.clear();
        gHeaderSize = 0;
        gPlayOnScreenSince = -1;
    }
    gCampaignOnScreen = false;
    if (!play) return;
    const bool onScreen = hub::WidgetOnScreen(play);
    if (onScreen && gPlayOnScreenSince < 0) gPlayOnScreenSince = game::Seconds();
    if (!onScreen) gPlayOnScreenSince = -1;

    if (gCampaigns.empty()) {               // nothing registered: no tab
        if (gView != View::None) {
            Leave(play);
            if (onScreen) ShowCircuit(play);
        }
        RemoveTab();
        gBackPending = false;
        return;
    }
    EnsureTab(play);
    TrackTab();

    if (gView != View::None) {
        int32_t active = -1;
        eng::ReadBytes(play, "ActiveTabIndex", &active, sizeof active);
        if (!onScreen || active != gOurIndex) Leave(play);
    } else if (onScreen) {
        // Back on the play page after leaving it from ours: its body still shows our page. The circuit tab, as a
        // click on it would.
        Obj body = eng::ReadObj(play, "Body_WS");
        Obj page = eng::Get(gPage);
        if (body && page && eng::Call(body, "GetActiveWidget").ReturnObj() == page) ShowCircuit(play);
    }

    // A plugin asked for its campaign's screen: the play page first (from the main menu), then the screen.
    if (gShowId) {
        if (!Find(gShowId) || game::Seconds() - gShowSince > 10) {
            if (Find(gShowId)) hostlog::Warn("campaigns: couldn't show campaign " + std::to_string(gShowId) + " (no play page)");
            gShowId = 0;
        } else if (onScreen && eng::Get(gTab) && game::Seconds() - gPlayOnScreenSince > 0.6) {
            Activate(play, View::Campaign, gShowId);
            gShowId = 0;
        } else if (!onScreen && !gShowCalledPlay && ui::menu::Shown()) {
            eng::Call(manager, "DoPlay");
            gShowCalledPlay = true;
        }
    }

    if (gTabClick) {
        gTabClick = false;
        if (onScreen) Activate(play, View::List, 0);
    }
    if (gView != View::None && onScreen) {
        TrackClicks(play);
        if (gBackPending) {
            gBackPending = false;
            if (gView == View::Campaign) Activate(play, View::List, 0);
        }
        if (gOpenIndex >= 0) {
            const int i = gOpenIndex;
            gOpenIndex = -1;
            if (gView == View::List && i < static_cast<int>(gCampaigns.size())) Activate(play, View::Campaign, gCampaigns[i].id);
        }
        if (gPickIndex >= 0) {
            const int i = gPickIndex;
            gPickIndex = -1;
            if (Campaign* c = gView == View::Campaign ? Find(gShownId) : nullptr)
                if (i < static_cast<int>(c->courses.size())) {
                    c->picked = i;
                    hostlog::Info("campaigns: course " + std::to_string(i) + " of campaign " + std::to_string(c->id) + " chosen");
                }
        }
        if (gDirty) {
            gDirty = false;
            Build(play);
            if (Obj page = eng::Get(gPage)) eng::Call(eng::ReadObj(play, "Body_WS"), "SetActiveWidget", page);
        }
    } else {
        gBackPending = false;
        gOpenIndex = gPickIndex = -1;
    }
    gCampaignOnScreen = gView == View::Campaign && onScreen;
}

// campaigns [status] | tab | open <i> | pick <i> | back | show <id> | gametab <i> | hover [<card>]
std::string Command(const std::string& args) {
    std::string verb = args, rest;
    if (const size_t sp = args.find(' '); sp != std::string::npos) {
        verb = args.substr(0, sp);
        rest = args.substr(sp + 1);
    }
    if (verb.empty() || verb == "status") {
        std::string s = std::to_string(gCampaigns.size()) + " campaign(s)";
        for (const auto& c : gCampaigns) s += " [" + std::to_string(c.id) + " '" + c.title + "' " + std::to_string(c.courses.size()) + " courses]";
        s += eng::Get(gTab) ? "; tab placed" : "; tab absent";
        s += gView == View::None ? "; page not shown" : gView == View::List ? "; list shown" : "; campaign " + std::to_string(gShownId) + " shown";
        if (gCampaignOnScreen || ListShown()) s += " (on screen)";
        s += "; back calls:";
        for (const auto& w : gWrapped)
            if (w.fn) s += " " + std::string(w.name).substr(0, 24) + "=" + std::to_string(w.calls);
        return s;
    }
    if (!testchannel::MutationsAllowed()) return "refused (not a complete sandbox)";
    if (verb == "tab") return (gTabClick = true), "tab clicked";
    if (verb == "open") return (gOpenIndex = std::atoi(rest.c_str())), "open " + rest;
    if (verb == "pick") return (gPickIndex = std::atoi(rest.c_str())), "pick " + rest;
    if (verb == "back") {                   // the menu's own back button, through ProcessEvent (as a real click)
        Obj manager = Manager();
        if (!manager) return "no menu";
        return eng::Call(manager, kBackButton, int32_t{0}).Invoked() ? "back pressed" : "back failed";
    }
    if (verb == "hover") return (gForceHover = rest.empty() ? -1 : std::atoi(rest.c_str())), "hover " + (rest.empty() ? std::string("off") : rest);
    if (verb == "gametab") {                // one of the game's own tabs, clicked (as its button does)
        Obj manager = Manager();
        Obj play = manager ? eng::ReadObj(manager, "WBP_Play_1") : nullptr;
        const auto used = play ? eng::ReadObjArray(play, "Used Tabs") : std::vector<Obj>{};
        const size_t i = static_cast<size_t>(std::atoi(rest.c_str()));
        if (i >= used.size()) return "no game tab " + rest;
        eng::Call(used[i], "SimulateButtonClicked");
        return "game tab " + rest + " clicked";
    }
    if (verb == "show") {
        const int id = std::atoi(rest.c_str());
        Campaign* c = Find(id);
        return c && Show(c->owner, id) ? "show " + rest : "no campaign " + rest;
    }
    return "unknown: campaigns [status] | tab | open <i> | pick <i> | back | show <id> | gametab <i> | hover [<card>]";
}

}  // namespace campaigns
