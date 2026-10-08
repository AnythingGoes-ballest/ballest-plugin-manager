#include "leaderboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "engine.hpp"
#include "cosmetics.hpp"
#include "game.hpp"
#include "hub.hpp"
#include "log.hpp"
#include "widgets.hpp"

namespace leaderboard {
namespace {

using eng::Obj;
namespace w = ui::widgets;

std::string gNote;
int gNoteOwner = -1;

struct Titled {
    eng::Weak title;                            // the TXT_HEader text block
    std::string original, shown;
};
std::vector<Titled> gTitles;

std::string gOverallNote;
int gOverallNoteOwner = -1;
std::vector<Titled> gOverallLabels;

// The in-map leaderboard: a WBP_Leaderboard_C inside the race UI (menus have others, for the track picker).
Obj InMapBoard() {
    static double lastLook = -100;
    static eng::Weak cached;
    if (Obj board = eng::Get(cached)) return board;
    if (game::Seconds() - lastLook < 1) return nullptr;
    lastLook = game::Seconds();
    Obj cls = eng::FindClass("WBP_Leaderboard_C"), race = eng::FindClass("WBP_RaceUIManager_C");
    if (!cls || !race) return nullptr;
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) != cls || eng::IsDefaultObject(o)) return true;
        for (Obj outer = eng::OuterOf(o); outer; outer = eng::OuterOf(outer))
            if (eng::ClassOf(outer) == race && !eng::IsDefaultObject(outer)) {
                found = o;
                return false;
            }
        return true;
    });
    cached = eng::MakeWeak(found);
    return found;
}

// The main menu's overall leaderboard bar, if one is on screen.
Obj HeaderBoard() {
    static double lastLook = -100;
    static eng::Weak cached;
    if (Obj board = eng::Get(cached)) return board;
    if (game::Seconds() - lastLook < 1) return nullptr;
    lastLook = game::Seconds();
    Obj cls = eng::FindClass("WBP_HeaderSeasonScore_C");
    if (!cls) return nullptr;
    Obj found = nullptr;
    // the live bar (under /Engine/Transient), not the template in WBP_Header's class (measured: it's found first)
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::PathOf(o).rfind("/Engine/Transient", 0) == 0) found = o;
        return found == nullptr;
    });
    cached = eng::MakeWeak(found);
    return found;
}

// The bar's "overall" label: the text block named "TextBlock" in its widget tree (found once per bar).
Obj OverallLabel(Obj board) {
    static eng::Weak cachedBoard, cachedLabel;
    if (eng::Get(cachedBoard) == board)
        if (Obj label = eng::Get(cachedLabel)) return label;
    Obj cls = eng::FindClass("TextBlock");
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) != cls || eng::ObjName(o) != "TextBlock") return true;
        for (Obj outer = eng::OuterOf(o); outer; outer = eng::OuterOf(outer))
            if (outer == board) {
                found = o;
                return false;
            }
        return true;
    });
    cachedBoard = eng::MakeWeak(board);
    cachedLabel = eng::MakeWeak(found);
    return found;
}

int CountEntries(uint64_t handle);

// An int kept in a Blueprint's event graph: its variables aren't properties of the object but of the graph's function
// (ExecuteUbergraph_<class>), in a frame the object points to through its UberGraphFrame property.
bool GraphInt(Obj o, const char* function, const char* name, int32_t* out) {
    const eng::Prop frameProp = eng::FindProp(eng::ClassOf(o), "UberGraphFrame");
    Obj fn = eng::FindFunction(eng::ClassOf(o), function);
    const eng::Prop var = fn ? eng::FindProp(fn, name) : eng::Prop{};
    if (!frameProp || !var || var.size != 4) return false;
    uint8_t* frame = nullptr;
    std::memcpy(&frame, o + frameProp.offset, sizeof frame);
    if (!frame) return false;
    std::memcpy(out, frame + var.offset, sizeof *out);
    return true;
}

}  // namespace

int Players() {
    Obj board = InMapBoard();
    if (!board || !eng::Call(board, "IsVisible").ReturnBool()) return -1;
    eng::Prop handleProp;
    const int offset = eng::NestedOffset(eng::ClassOf(board), {"NativeActiveLeaderboardRecord", "LeaderboardHandle"}, &handleProp);
    if (offset < 0 || handleProp.size != 8) return -1;
    uint64_t handle = 0;
    std::memcpy(&handle, board + offset, sizeof handle);
    return CountEntries(handle);
}

int OverallPlayers() {
    Obj board = HeaderBoard();
    if (!board || !eng::Call(board, "IsVisible").ReturnBool()) return -1;
    int32_t id = 0;
    if (!GraphInt(board, "ExecuteUbergraph_WBP_HeaderSeasonScore", "Temp_int_Variable", &id) || id <= 0) return -1;
    return CountEntries(static_cast<uint64_t>(id));
}

void SetOverallNote(int owner, const std::string& note) {
    gOverallNote = note;
    gOverallNoteOwner = note.empty() ? -1 : owner;
}

namespace {
// Steam's entry count for a leaderboard, or -1.
int CountEntries(uint64_t handle) {
    if (!handle) return -1;
    Obj steam = eng::FindCdo("SIK_UserStatsLibrary");
    if (!steam) return -1;
    // Measured: the function takes the handle as a 4-byte LeaderboardID (the game's handles fit: 0x013BAB71).
    eng::Params p(eng::FunctionOn(steam, "GetLeaderboardEntryCount"));
    const int32_t idSize = p.SizeOf("LeaderboardID");
    if ((idSize != 4 && idSize != 8) || (idSize == 4 && handle > 0xFFFFFFFFull) || !p.SetArg(0, &handle, static_cast<size_t>(idSize))) return -1;
    eng::Invoke(steam, p);
    size_t size = 0;
    const uint8_t* r = p.Return(&size);
    if (!r || size < 4) return -1;
    int32_t count = 0;
    std::memcpy(&count, r, 4);
    return count > 0 ? count : -1;
}
}  // namespace

void SetTitleNote(int owner, const std::string& note) {
    gNote = note;
    gNoteOwner = note.empty() ? -1 : owner;
}

void ClearRowsOf(int owner);
void RemoveOwner(int owner) {
    ClearRowsOf(owner);
    if (owner == gNoteOwner) SetTitleNote(owner, "");
    if (owner == gOverallNoteOwner) SetOverallNote(owner, "");
}

// --- extra rows: a panel of the game's own leaderboard rows under the in-map leaderboard -------------------------------
namespace {
struct ExtraRow {
    std::string name;
    double time = 0;
    bool ghost = false, pinned = false;
};
std::vector<ExtraRow> gRows;
std::string gRowsTitle;
int gRowsOwner = -1, gClicked = -1, gPinClicked = -1;

struct BuiltRow {
    eng::Weak entry, hit, pinHit, pinBar;
    ExtraRow shown;
    bool fresh = true, wasPressed = false, pinWasPressed = false;
};
std::vector<BuiltRow> gBuilt;
std::wstring gIconFile;                 // the tab button's icon (a plugin's image), or the game's list icon

struct Margin4 {
    float left, top, right, bottom;
};
struct Vec2d {
    double x, y;
};

// The rows are a third view of the in-map leaderboard, after its world and friends ones (measured: their buttons,
// WBP_GlobalFilter and WBP_FriendFilter, are WBP_SimpleIconButton_C in HeaderGroup > WrapBox_2 > HorizontalBox_1; the
// board's list, EntriesScrollBox, sits in SizeBox_1 in VerticalBox_2, with AroundMeBox and Divider). A button of the
// game's own kind goes beside theirs, and a list beside SizeBox_1. The view open: the game's list collapsed, its
// buttons shown inactive, the title the rows' own. World or friends clicked: the game's view back as it was.
eng::Weak gTab, gTabBoard, gList, gListBox, gScroller, gTitleShown;
float gViewHeight = 0, gViewSet = 0;
double gSnapped = 0;                    // the whole-row offset the list was last put at    // the height the game's list had, and the whole rows of it the box was given
bool gTabOpen = false, gTabWasPressed = false, gGlobalWasPressed = false, gFriendWasPressed = false, gTestToggle = false;
struct Hidden {
    eng::Weak widget;
    uint8_t visibility = 0;
};
std::vector<Hidden> gHidden;
std::string gTitleBefore;

struct SlateChildSize {
    float value;
    uint8_t rule;
};

Obj TabIcon() {
    if (!gIconFile.empty())
        if (Obj t = cosmetics::LoadTexture(gIconFile)) return t;
    return cosmetics::LoadAsset(L"/Game/Art/UI/Textures/T_List_Icon.T_List_Icon");
}

bool BuildTab(Obj board) {
    Obj friends = eng::ReadObj(board, "WBP_FriendFilter"), scroll = eng::ReadObj(board, "EntriesScrollBox");
    Obj row = friends ? eng::Call(friends, "GetParent").ReturnObj() : nullptr;
    Obj entries = scroll ? eng::Call(scroll, "GetParent").ReturnObj() : nullptr;
    Obj body = entries ? eng::Call(entries, "GetParent").ReturnObj() : nullptr;
    Obj cls = eng::FindClass("WBP_SimpleIconButton_C"), tree = eng::ReadObj(board, "WidgetTree");
    if (!row || !body || !cls || !tree) return false;
    Obj tab = eng::Call(eng::FindCdo("WidgetBlueprintLibrary"), "Create", board, cls, game::PlayerController()).ReturnObj();
    if (!tab) return false;
    // looks as the friends button does (read before it's built: its PreConstruct uses them)
    const struct {
        const char* name;
        size_t size;
    } looks[] = {{"In User Specified Scale", 4}, {"bShowBackground", 1}, {"bOnWhite", 1}, {"InactiveBG", 16}, {"InactiveFG", 16},
                 {"BrandColor", 20},          {"LightGrey", 20},      {"GreyText", 20}};
    for (const auto& look : looks) {
        uint8_t bytes[32];
        if (eng::ReadBytes(friends, look.name, bytes, look.size)) eng::WriteBytes(tab, look.name, bytes, look.size);
    }
    Obj icon = TabIcon();
    if (icon) eng::WriteBytes(tab, "DesiredIcon", &icon, sizeof icon);
    Obj slot = eng::Call(row, "AddChildToHorizontalBox", tab).ReturnObj();
    if (!slot) return false;
    if (Obj theirs = eng::ReadObj(friends, "Slot")) {
        Margin4 padding{};
        SlateChildSize size{};
        uint8_t h = 0, v = 0;
        if (eng::ReadBytes(theirs, "Padding", &padding, sizeof padding)) eng::Call(slot, "SetPadding", padding);
        if (eng::ReadBytes(theirs, "Size", &size, sizeof size)) eng::Call(slot, "SetSize", size);
        if (eng::ReadBytes(theirs, "HorizontalAlignment", &h, 1)) eng::Call(slot, "SetHorizontalAlignment", h);
        if (eng::ReadBytes(theirs, "VerticalAlignment", &v, 1)) eng::Call(slot, "SetVerticalAlignment", v);
    }
    if (icon) eng::Call(tab, "UpdateIcon", icon);
    eng::Call(tab, "SetIsActive", uint8_t{0});
    // in a box as wide and as tall as the game's list was (measured: without them the board narrows to the rows' names,
    // and grows up the screen with 30 runs), scrolling past that, and clipped (measured: rows below its edge still
    // drew their names)
    Obj box = w::Spawn("SizeBox", tree), scroller = w::Spawn("ScrollBox", tree), list = w::Spawn("VerticalBox", tree);
    if (!box || !scroller || !list) return false;
    w::AddChild(scroller, list);
    w::AddChild(box, scroller);
    for (Obj o : {box, scroller}) eng::Call(o, "SetClipping", uint8_t{1});      // EWidgetClipping::ClipToBounds
    eng::Call(body, "AddChildToVerticalBox", box);
    w::SetVisibility(box, w::kCollapsed);
    gListBox = eng::MakeWeak(box);
    gScroller = eng::MakeWeak(scroller);
    gTab = eng::MakeWeak(tab);
    gTabBoard = eng::MakeWeak(board);
    gList = eng::MakeWeak(list);
    gBuilt.clear();
    gTabOpen = gTabWasPressed = gGlobalWasPressed = gFriendWasPressed = false;
    gHidden.clear();
    return true;
}

// The game's own list, hidden while the tab is open: the box around its rows, the rows around the player, the divider.
std::vector<Obj> GameList(Obj board) {
    std::vector<Obj> out;
    if (Obj scroll = eng::ReadObj(board, "EntriesScrollBox"))
        if (Obj entries = eng::Call(scroll, "GetParent").ReturnObj()) out.push_back(entries);
    for (const char* name : {"AroundMeBox", "Divider"})
        if (Obj o = eng::ReadObj(board, name)) out.push_back(o);
    return out;
}

void OpenTab(Obj board) {
    gTabOpen = true;
    gHidden.clear();
    for (Obj o : GameList(board)) gHidden.push_back({eng::MakeWeak(o), eng::Call(o, "GetVisibility").ReturnAs<uint8_t>(0)});
    Obj title = eng::ReadObj(board, "TXT_HEader");
    gTitleShown = eng::MakeWeak(title);
    gTitleBefore = title ? w::ReadText(title) : "";
    Obj box = eng::Get(gListBox);
    if (Obj scroll = eng::ReadObj(board, "EntriesScrollBox"))
        if (Obj entries = eng::Call(scroll, "GetParent").ReturnObj()) {
            const Vec2d size = eng::Call(entries, "GetDesiredSize").ReturnAs<Vec2d>();
            if (size.x > 1) eng::Call(box, "SetMinDesiredWidth", static_cast<float>(size.x));
        }
    float tallest = 0;
    for (Obj o : GameList(board))
        if (eng::Call(o, "GetVisibility").ReturnAs<uint8_t>(1) != w::kCollapsed)
            tallest += static_cast<float>(eng::Call(o, "GetDesiredSize").ReturnAs<Vec2d>().y);
    if (tallest < 1) eng::ReadBytes(board, "LB_MaxDesiredHeight", &tallest, sizeof tallest);
    if (tallest > 1) eng::Call(box, "SetMaxDesiredHeight", tallest);
    gViewHeight = tallest;
    gViewSet = 0;
    gSnapped = 0;
    w::SetVisibility(box, w::kSelfHitTestInvisible);
    eng::Call(eng::Get(gTab), "SetIsActive", uint8_t{1});
}

void CloseTab() {
    if (!gTabOpen) return;
    gTabOpen = false;
    for (const Hidden& h : gHidden)
        if (Obj o = eng::Get(h.widget)) w::SetVisibility(o, h.visibility);
    gHidden.clear();
    if (Obj title = eng::Get(gTitleShown)) w::SetText(title, gTitleBefore);
    w::SetVisibility(eng::Get(gListBox), w::kCollapsed);
    if (Obj tab = eng::Get(gTab)) eng::Call(tab, "SetIsActive", uint8_t{0});
}

void RemoveTab() {
    CloseTab();
    for (eng::Weak* weak : {&gTab, &gListBox})
        if (Obj o = eng::Get(*weak)) eng::Call(o, "RemoveFromParent");
    gTab = gList = gListBox = gTabBoard = eng::Weak{};
    gBuilt.clear();
}

// A button released while the mouse is still on it: a click (the game's own buttons, polled like ours).
bool Released(Obj button, bool* wasPressed) {
    Obj hit = button ? eng::ReadObj(button, "HitBox") : nullptr;
    if (!hit) return false;
    const bool pressed = eng::Call(hit, "IsPressed").ReturnBool();
    const bool clicked = *wasPressed && !pressed && eng::Call(hit, "IsHovered").ReturnBool();
    *wasPressed = pressed;
    return clicked;
}

void SetEntry(BuiltRow& row, int index, const ExtraRow& want) {
    Obj entry = eng::Get(row.entry);
    if (!entry) return;
    if (row.fresh) {
        // the game's own set-up, as an entry of someone else's (so the game's normal colours), then ours on top
        eng::Params setup(eng::FunctionOn(entry, "Setup Entry Widget"));
        setup.Set("ArrayIndex", static_cast<int32_t>(index));
        setup.Set("bWantToSeeScores", uint8_t{1});
        eng::Invoke(entry, setup);
        eng::Call(entry, "ShowGhostButton", uint8_t{0});
        eng::Call(entry, "Handle Load State", uint8_t{0});
        // the name clipped by the list's scroll box like the rest of the row (measured: below the box's edge only the
        // name still drew, its own clipping ignoring its parents')
        if (Obj name = eng::ReadObj(entry, "CT_Username")) eng::Call(name, "SetClipping", uint8_t{1});
        w::SetText(eng::ReadObj(entry, "Rank"), std::to_string(index + 1));
    }
    if (row.fresh || want.name != row.shown.name) w::SetText(eng::ReadObj(entry, "CT_Username"), want.name);
    if (row.fresh || want.time != row.shown.time)
        if (Obj time = eng::ReadObj(entry, "TimeText_v2")) eng::Call(time, "SetRaceScore", static_cast<int32_t>(want.time * 100000 + 0.5));   // hundred-thousandths (measured: 27.184 s = 2718409)
    if (row.fresh || want.ghost != row.shown.ghost) eng::Call(entry, "SetGhostIsActive", static_cast<uint8_t>(want.ghost));
    if (row.fresh || want.pinned != row.shown.pinned) {
        const ui::Color lime{0.55f, 0.85f, 0.0f, 1}, white{1, 1, 1, 1};
        w::SetTextColor(eng::ReadObj(entry, "Rank"), want.pinned ? lime : white);
        w::SetVisibility(eng::Get(row.pinBar), want.pinned ? w::kHitTestInvisible : w::kCollapsed);
    }
    row.shown = want;
    row.fresh = false;
}

void RowsFrame() {
    Obj board = gRows.empty() && !eng::Get(gTab) ? nullptr : InMapBoard();
    if (!board) return;
    if (gRows.empty()) {
        RemoveTab();
        return;
    }
    if (!eng::Get(gTab) || !eng::Get(gList) || eng::Get(gTabBoard) != board) {
        RemoveTab();
        if (!BuildTab(board)) return;
    }
    const bool toggled = std::exchange(gTestToggle, false);
    if (Released(eng::Get(gTab), &gTabWasPressed) || (toggled && !gTabOpen)) OpenTab(board);
    else if (toggled) {
        // closed without a click on world or friends (which light themselves): the one the board was on lit again
        CloseTab();
        int32_t active = 0;
        if (eng::ReadBytes(board, "ActiveIndex", &active, sizeof active)) eng::Call(board, "SetFilterActive", active);
    }
    const bool globalClicked = Released(eng::ReadObj(board, "WBP_GlobalFilter"), &gGlobalWasPressed);
    const bool friendClicked = Released(eng::ReadObj(board, "WBP_FriendFilter"), &gFriendWasPressed);
    if (globalClicked || friendClicked) CloseTab();
    if (!gTabOpen) return;
    // kept so while open: the game shows its list and its buttons again whenever it refreshes the board
    for (const Hidden& h : gHidden)
        if (Obj o = eng::Get(h.widget))
            if (eng::Call(o, "GetVisibility").ReturnAs<uint8_t>(0) != w::kCollapsed) w::SetVisibility(o, w::kCollapsed);
    for (const char* name : {"WBP_GlobalFilter", "WBP_FriendFilter"}) {
        Obj button = eng::ReadObj(board, name);
        bool active = false;
        if (button && eng::ReadBool(button, "Active", &active) && active) eng::Call(button, "SetIsActive", uint8_t{0});
    }
    if (Obj title = eng::Get(gTitleShown))
        if (w::ReadText(title) != gRowsTitle) w::SetText(title, gRowsTitle);
    Obj list = eng::Get(gList);
    Obj entryClass = eng::FindClass("WBP_LeaderboardEntry_C");
    if (!list || !entryClass) return;
    // as many rows as wanted: the game's own row widget, under a see-through button that takes the clicks
    while (gBuilt.size() > gRows.size()) {
        if (Obj o = eng::Get(gBuilt.back().hit)) eng::Call(eng::Call(o, "GetParent").ReturnObj(), "RemoveFromParent");
        gBuilt.pop_back();
    }
    Obj tree = eng::ReadObj(board, "WidgetTree");
    while (gBuilt.size() < gRows.size()) {
        Obj entry = eng::Call(eng::FindCdo("WidgetBlueprintLibrary"), "Create", board, entryClass, game::PlayerController()).ReturnObj();
        Obj over = w::Spawn("Overlay", tree), hit = w::Spawn("Button", tree), pinBox = w::Spawn("SizeBox", tree),
            pinHit = w::Spawn("Button", tree);
        Obj pinBar = w::Block(tree, 4, 1, ui::Color{0.55f, 0.85f, 0.0f, 1});
        if (!entry || !over || !hit || !pinBox || !pinHit || !pinBar) return;
        for (Obj b : {hit, pinHit}) {
            w::Unfocusable(b);
            w::Transparent(b);
        }
        eng::Call(pinBox, "SetWidthOverride", 64.0f);              // the rank's end of the row
        w::AddChild(pinBox, pinHit);
        w::AddToOverlay(over, entry, w::kAlignFill, w::kAlignFill, {0, 0, 0, 0});
        w::AddToOverlay(over, pinBar, w::kAlignLeft, w::kAlignFill, {0, 0, 0, 0});
        w::AddToOverlay(over, hit, w::kAlignFill, w::kAlignFill, {0, 0, 0, 0});
        w::AddToOverlay(over, pinBox, w::kAlignLeft, w::kAlignFill, {0, 0, 0, 0});
        if (Obj slot = eng::Call(list, "AddChildToVerticalBox", over).ReturnObj()) eng::Call(slot, "SetPadding", Margin4{0, 4, 0, 0});
        BuiltRow row;
        row.entry = eng::MakeWeak(entry);
        row.hit = eng::MakeWeak(hit);
        row.pinHit = eng::MakeWeak(pinHit);
        row.pinBar = eng::MakeWeak(pinBar);
        gBuilt.push_back(row);
    }
    for (size_t i = 0; i < gBuilt.size(); ++i) {
        SetEntry(gBuilt[i], static_cast<int>(i), gRows[i]);
        Obj hit = eng::Get(gBuilt[i].hit);
        if (!hit) continue;
        const bool pressed = eng::Call(hit, "IsPressed").ReturnBool();
        {   // DEBUG: where the mouse is, and presses
            static std::string lastSeen;
            Obj pin = eng::Get(gBuilt[i].pinHit);
            const bool rowHover = eng::Call(hit, "IsHovered").ReturnBool(), pinHover = pin && eng::Call(pin, "IsHovered").ReturnBool();
            const bool pinPress = pin && eng::Call(pin, "IsPressed").ReturnBool();
            if (rowHover || pinHover || pressed || pinPress) {
                const std::string seen = "row " + std::to_string(i) + (rowHover ? " row-hover" : "") + (pinHover ? " rank-hover" : "") +
                                         (pressed ? " row-PRESSED" : "") + (pinPress ? " rank-PRESSED" : "");
                if (seen != lastSeen) hostlog::Info("DEBUG rows: " + seen);
                lastSeen = seen;
            }
        }
        if (gBuilt[i].wasPressed && !pressed && eng::Call(hit, "IsHovered").ReturnBool()) gClicked = static_cast<int>(i);
        gBuilt[i].wasPressed = pressed;
        if (Obj pin = eng::Get(gBuilt[i].pinHit)) {
            const bool pinPressed = eng::Call(pin, "IsPressed").ReturnBool();
            if (gBuilt[i].pinWasPressed && !pinPressed && eng::Call(pin, "IsHovered").ReturnBool()) gPinClicked = static_cast<int>(i);
            gBuilt[i].pinWasPressed = pinPressed;
        }
    }
    // Only whole rows inside the box show (measured: a row past its edge still drew its name, whatever its clipping),
    // and the box is a whole number of rows tall, so at rest none is cut.
    Obj scroller = eng::Get(gScroller), box = eng::Get(gListBox);
    Obj first = gBuilt.empty() ? nullptr : eng::Get(gBuilt.front().hit);
    Obj firstRow = first ? eng::Call(first, "GetParent").ReturnObj() : nullptr;
    const double rowHeight = firstRow ? eng::Call(firstRow, "GetDesiredSize").ReturnAs<Vec2d>().y + 4 : 0;   // + the slot's top padding
    if (!scroller || !box || rowHeight < 1 || gViewHeight < 1) return;
    const float fits = static_cast<float>(std::max(1.0, std::floor(gViewHeight / rowHeight)) * rowHeight);
    if (fits != gViewSet) {
        eng::Call(box, "SetMaxDesiredHeight", fits);
        gViewSet = fits;
    }
    // where it shows from (the offset asked for can be past the end: measured, it reads back unclamped)
    const double end = eng::Call(scroller, "GetScrollOffsetOfEnd").ReturnAs<float>(0);
    double offset = std::clamp(static_cast<double>(eng::Call(scroller, "GetScrollOffset").ReturnAs<float>(0)), 0.0, end);
    // scrolled: on to the next whole row the way it went (a wheel step smaller than a row still moves one)
    if (std::abs(offset - gSnapped) > 0.5) {
        const double rows = offset / rowHeight;
        offset = std::min(end, (offset > gSnapped ? std::ceil(rows - 0.01) : std::floor(rows + 0.01)) * rowHeight);
        eng::Call(scroller, "SetScrollOffset", static_cast<float>(offset));
        gSnapped = offset;
    }
    for (size_t i = 0; i < gBuilt.size(); ++i) {
        Obj hit = eng::Get(gBuilt[i].hit);
        Obj row = hit ? eng::Call(hit, "GetParent").ReturnObj() : nullptr;
        if (!row) continue;
        const double top = static_cast<double>(i) * rowHeight;
        const bool inside = top >= offset - 1 && top + rowHeight <= offset + fits + 1;
        eng::Call(row, "SetRenderOpacity", inside ? 1.0f : 0.0f);
    }
}
}  // namespace

void SetExtraRows(int owner, const std::string& title, const std::vector<std::string>& names, const std::vector<double>& times,
                  const std::vector<bool>& ghosts, const std::vector<bool>& pinned) {
    gRowsOwner = owner;
    gRowsTitle = title;
    gRows.clear();
    for (size_t i = 0; i < names.size(); ++i)
        gRows.push_back({names[i], i < times.size() ? times[i] : 0, i < ghosts.size() && ghosts[i], i < pinned.size() && pinned[i]});
}

int ExtraRowPinClicked() {
    const int c = gPinClicked;
    gPinClicked = -1;
    return c;
}

void TestPinRow(int row) { gPinClicked = row; }

void ClearRowsOf(int owner) {
    if (owner == gRowsOwner) gRows.clear();
}

void TestClickRow(int row) { gClicked = row; }

void SetExtraRowsIcon(int owner, const std::wstring& file) {
    if (owner != gRowsOwner && gRowsOwner >= 0) return;
    gIconFile = file;
    if (Obj tab = eng::Get(gTab))
        if (Obj icon = TabIcon()) eng::Call(tab, "UpdateIcon", icon);
}

void TestToggleTab() { gTestToggle = true; }

void TestScroll(float offset) {
    if (Obj scroller = eng::Get(gScroller)) eng::Call(scroller, "SetScrollOffset", offset);
}

bool ExtraRowsShown() { return gTabOpen; }

int ExtraRowClicked() {
    const int c = gClicked;
    gClicked = -1;
    return c;
}

void Frame() {
    RowsFrame();
    static double lastLook = -100;
    if (game::Seconds() - lastLook < 0.25) return;
    lastLook = game::Seconds();
    Obj board = gNote.empty() && gTitles.empty() ? nullptr : InMapBoard();
    Obj title = board ? eng::ReadObj(board, "TXT_HEader") : nullptr;
    // A title seen for the first time is remembered as the game wrote it, to put the note after (and back without).
    if (title && std::none_of(gTitles.begin(), gTitles.end(), [&](const Titled& t) { return eng::Get(t.title) == title; }))
        gTitles.push_back({eng::MakeWeak(title), w::ReadText(title), ""});
    for (auto it = gTitles.begin(); it != gTitles.end();) {
        Obj text = eng::Get(it->title);
        if (!text) {
            it = gTitles.erase(it);
            continue;
        }
        if (gTabOpen && text == eng::Get(gTitleShown)) {
            it->shown.clear();              // written again once the tab closes
            ++it;
            continue;
        }
        const std::string wanted = gNote.empty() ? it->original : it->original + "  " + gNote;
        if (wanted != it->shown) {
            w::SetText(text, wanted);
            it->shown = wanted;
        }
        ++it;
    }
    // The overall label: the note goes before "overall".
    Obj header = gOverallNote.empty() && gOverallLabels.empty() ? nullptr : HeaderBoard();
    Obj label = header ? OverallLabel(header) : nullptr;
    if (label && std::none_of(gOverallLabels.begin(), gOverallLabels.end(), [&](const Titled& t) { return eng::Get(t.title) == label; }))
        gOverallLabels.push_back({eng::MakeWeak(label), w::ReadText(label), ""});
    for (auto it = gOverallLabels.begin(); it != gOverallLabels.end();) {
        Obj text = eng::Get(it->title);
        if (!text) {
            it = gOverallLabels.erase(it);
            continue;
        }
        const std::string wanted = gOverallNote.empty() ? it->original : gOverallNote + "  " + it->original;
        if (wanted != it->shown) {
            w::SetText(text, wanted);
            it->shown = wanted;
        }
        ++it;
    }
}

}  // namespace leaderboard
