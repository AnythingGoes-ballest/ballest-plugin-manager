#include "hub.hpp"

#include <algorithm>

#include <cmath>
#include <cstring>
#include <map>

#include "game.hpp"
#include "log.hpp"
#include "widgets.hpp"

namespace w = ui::widgets;

namespace hub {
namespace {

using eng::Obj;

// BallestHubQuerySpec (type dump): 216 bytes; the members changed here.
constexpr int kSpecSize = 216, kSpecQueryKind = 0x11, kSpecQueryType = 0x28, kSpecTrendDays = 0x2c, kSpecPageIndex = 0x34,
              kSpecQueryGroupTags = 0x58, kSpecRequiredSteamTags = 0x68, kSpecMatchAnyTag = 0x88, kSpecUserSortOrder = 0xb5;
constexpr uint8_t kQueryAll = 0, kQueryUser = 2;    // EBallestHubQueryKind
constexpr uint8_t kUserSortVoteScore = 5;           // ESIK_UserUGCListSortOrder VoteScoreDesc
// EBallestHubBrowseSort (the game's own sorts) and ESIK_UGCQuery (Steam's, set over the game's for the others).
struct Sort {
    const char* name;
    uint8_t browse;
    int steamRank;                              // -1: the game's sort as it is
};
constexpr Sort kSorts[] = {{"top", 0, -1},     {"trending", 1, -1}, {"new", 2, -1},        {"relevance", 3, -1},
                           {"oldest", 4, -1},  {"title", 5, -1},    {"updated", 6, -1},    {"played", 0, 18},
                           {"subscribed", 0, 12}, {"liked", 0, 10}};

struct TArrayView {
    const void* data;
    int32_t num, max;
};

// The live play page (under /Engine/Transient, not its class's template), looked for at most once a second.
Obj PlayPage() {
    static eng::Weak cached;
    static double lastLook = -100;
    if (Obj page = eng::Get(cached)) return page;
    if (game::Seconds() - lastLook < 1) return nullptr;
    lastLook = game::Seconds();
    Obj cls = eng::FindClass("WBP_0_Play_C");
    if (!cls) return nullptr;
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::PathOf(o).rfind("/Engine/Transient", 0) == 0) found = o;
        return found == nullptr;
    });
    cached = eng::MakeWeak(found);
    return found;
}

Obj HubPage() {
    Obj page = PlayPage();
    return page ? eng::ReadObj(page, "WBP_HubHomePage_0") : nullptr;
}

// The hub's main list (where searches show).
Obj ListView() {
    Obj hub = HubPage();
    return hub ? eng::ReadObj(hub, "WBP_HubListView") : nullptr;
}

// The Subscribed page's own list (WBP_HubHomePage.WBP_SubscribedItems.WBP_ListView, the same list class).
Obj SubscribedListView() {
    Obj hub = HubPage();
    Obj page = hub ? eng::ReadObj(hub, "WBP_SubscribedItems") : nullptr;
    return page ? eng::ReadObj(page, "WBP_ListView") : nullptr;
}

// Shown on screen: visible itself and up its parents, the active child of any switcher on the way, and its top user
// widget in the viewport. Parents across widget trees are found as hud.cpp does (a tree's root: its user widget).
bool OnScreen(Obj widget) {
    static Obj userClass = eng::FindClass("UserWidget");
    static Obj switcherClass = eng::FindClass("WidgetSwitcher");
    for (int i = 0; widget && i < 60; ++i) {
        if (!eng::Call(widget, "IsVisible").ReturnBool()) return false;
        Obj parent = eng::Call(widget, "GetParent").ReturnObj();
        if (parent && switcherClass && eng::IsA(parent, switcherClass) && eng::Call(parent, "GetActiveWidget").ReturnObj() != widget)
            return false;
        if (!parent) {
            Obj owner = eng::OuterOf(eng::OuterOf(widget));
            if (!owner || !userClass || !eng::IsA(owner, userClass)) {
                if (userClass && eng::IsA(widget, userClass) && eng::FindFunction(eng::ClassOf(widget), "IsInViewport"))
                    return eng::Call(widget, "IsInViewport").ReturnBool();
                return true;
            }
            parent = owner;
        }
        widget = parent;
    }
    return true;
}

Obj Subsystem(const char* className) {
    Obj controller = game::PlayerController();
    Obj cls = eng::FindClass(className);
    return controller && cls ? eng::Call(eng::FindCdo("SubsystemBlueprintLibrary"), "GetGameInstanceSubsystem", controller, cls).ReturnObj()
                             : nullptr;
}

// The game's main menu UI manager, which keeps the tags picked in the hub's filter panel.
Obj UiManager() {
    static eng::Weak cached;
    static double lastLook = -100;
    if (Obj manager = eng::Get(cached)) return manager;
    if (game::Seconds() - lastLook < 1) return nullptr;
    lastLook = game::Seconds();
    Obj cls = eng::FindClass("WBP_MainMenu_UIManager_C");
    if (!cls) return nullptr;
    Obj found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::PathOf(o).rfind("/Engine/Transient", 0) == 0) found = o;
        return found == nullptr;
    });
    cached = eng::MakeWeak(found);
    return found;
}

// The maps the list's entries hold (WBP_HubListEntry.MyPubFileID), with the entry widgets.
struct Entry {
    uint64_t id;
    Obj widget;
    bool inUse = true;          // list entries: showing one of the list's current results (see ListEntries)
};
// The list on screen: the main list or the Subscribed page's; null on the hub's Home (its tiles aren't a list).
Obj ShownList() {
    if (Obj main = ListView(); main && OnScreen(main)) return main;
    if (Obj sub = SubscribedListView(); sub && OnScreen(sub)) return sub;
    return nullptr;
}

bool HiddenByPlugin(Obj widget);

// The list's entries in order. Entry i is in use when the list's current results (UGCDisplayDetails, an array of
// SIK_SteamUGCDetails of 168 bytes with the map's id first; types dump) have an i-th map and it is the entry's map:
// entries left over from earlier results keep their old map and must never be counted or shown again (measured: a
// leftover entry holding a map that was also in the new results came back as a second copy).
std::vector<Entry> ListEntries() {
    std::vector<Entry> out;
    Obj list = ShownList();
    const eng::Prop details = list ? eng::FindProp(eng::ClassOf(list), "UGCDisplayDetails") : eng::Prop{};
    const uint8_t* data = nullptr;
    int32_t count = -1;
    if (details) {
        std::memcpy(&data, list + details.offset, sizeof data);
        std::memcpy(&count, list + details.offset + 8, sizeof count);
    }
    int index = 0;
    for (Obj entry : list ? eng::ReadObjArray(list, "Entries") : std::vector<Obj>{}) {
        const int i = index++;
        int64_t id = 0;
        if (!entry || !eng::ReadBytes(entry, "MyPubFileID", &id, sizeof id) || id <= 0) continue;
        bool inUse = true;
        if (data && count >= 0) {
            int64_t result = 0;
            if (i < count) std::memcpy(&result, data + static_cast<size_t>(i) * 168, sizeof result);
            inUse = i < count && result == id;
            static int logged = 0;
            if (!inUse && logged < 3 && eng::Call(entry, "IsVisible").ReturnBool() && !HiddenByPlugin(entry)) {
                ++logged;
                hostlog::Info("hub: entry " + std::to_string(i) + " is on screen but isn't result " + std::to_string(i) + " of " +
                              std::to_string(count) + " (its map " + std::to_string(id) + ", the result's " + std::to_string(result) + ")");
            }
        }
        out.push_back({static_cast<uint64_t>(id), entry, inUse});
    }
    return out;
}

// Entries a plugin hid, per list (main and Subscribed), forgotten when that list shows new results (its
// LastResponse's RequestId changes): the game sets its entries' visibility itself then.
struct Hidden {
    eng::Weak list, widget;
};
std::vector<Hidden> gHidden;
struct Seen {
    eng::Weak list;
    uint8_t request[16] = {};
};
Seen gSeen[2];

void ForgetHiddenOnNewResults() {
    Obj lists[2] = {ListView(), SubscribedListView()};
    for (int i = 0; i < 2; ++i) {
        Obj list = lists[i];
        const eng::Prop last = list ? eng::FindProp(eng::ClassOf(list), "LastResponse") : eng::Prop{};
        if (!last) continue;
        uint8_t request[16] = {};
        std::memcpy(request, list + last.offset, sizeof request);      // BallestHubQueryResponse.RequestId at 0
        if (eng::Get(gSeen[i].list) != list || std::memcmp(request, gSeen[i].request, sizeof request) != 0) {
            gSeen[i].list = eng::MakeWeak(list);
            std::memcpy(gSeen[i].request, request, sizeof request);
            gHidden.erase(std::remove_if(gHidden.begin(), gHidden.end(), [&](const Hidden& h) { return eng::Get(h.list) == list; }),
                          gHidden.end());
        }
    }
}

bool HiddenByPlugin(Obj widget) {
    for (const auto& h : gHidden)
        if (eng::Get(h.widget) == widget) return true;
    return false;
}

// The hub's pages box (HubHome_SB, a SizeBox of a fixed height) and its height before any docked row.
eng::Weak gSizedFor;
float gOriginalHeight = 0;
double gAppliedHeight = -1;

}  // namespace

// A plugin's button after the author's name in the hub's info panel (WBP_HubTrackInformationPanel: byText and
// Username_Text, types dump). Added to the name's own row when that is a HorizontalBox ending with the name; the row
// is logged once, so the placement can be checked.
std::string gAuthorLabel;
eng::Weak gAuthorButton, gAuthorText;
bool gAuthorWasPressed = false, gAuthorClicked = false;

void AuthorButtonFrame() {
    Obj button = eng::Get(gAuthorButton);
    if (gAuthorLabel.empty()) {
        if (button) w::SetVisibility(button, w::kCollapsed);
        return;
    }
    if (!button) {
        Obj page = PlayPage();
        Obj panel = page ? eng::ReadObj(page, "WBP_HubInformationPanel") : nullptr;
        Obj name = panel ? eng::ReadObj(panel, "Username_Text") : nullptr;
        Obj row = name ? eng::Call(name, "GetParent").ReturnObj() : nullptr;
        Obj tree = panel ? eng::ReadObj(panel, "WidgetTree") : nullptr;
        static Obj rowClass = eng::FindClass("HorizontalBox");
        if (!row || !tree) return;
        const int count = eng::Call(row, "GetChildrenCount").ReturnAs<int32_t>(0);
        const int at = eng::Call(row, "GetChildIndex", name).ReturnAs<int32_t>(-1);
        const bool fits = rowClass && eng::IsA(row, rowClass) && at >= 0;
        static bool logged = false;
        if (!logged) {
            const std::string path = eng::PathOf(eng::ClassOf(row));
            hostlog::Info("hub author button: the name's row is a " + path.substr(path.rfind('.') + 1) + " of " + std::to_string(count) +
                          " widgets, the name at " + std::to_string(at) + (fits ? ": the button goes after it" : ": no button"));
            logged = true;
        }
        if (!fits) return;
        Obj b = w::Spawn("Button", tree), text = w::Spawn("TextBlock", tree);
        if (!b || !text) return;
        w::Unfocusable(b);
        eng::Call(b, "SetBackgroundColor", ui::Color{0.22f, 0.22f, 0.24f, 1});
        w::SetFontSize(text, 13);
        w::SetTextColor(text, ui::Color{1, 1, 1, 1});
        w::SetText(text, gAuthorLabel);
        w::AddChild(b, text);
        // HorizontalBox can only append: the widgets after the name come off, the button goes on, and they go back on
        // with their slots' settings (HorizontalBoxSlot: Size at 0x40, Padding 0x48, alignments 0x58/0x59; types dump).
        struct Later {
            Obj widget;
            uint8_t slot[0x1a];
        };
        std::vector<Later> later;
        for (int i = at + 1; i < count; ++i) {
            Later l{eng::Call(row, "GetChildAt", i).ReturnObj(), {}};
            Obj slot = l.widget ? eng::ReadObj(l.widget, "Slot") : nullptr;
            if (slot) std::memcpy(l.slot, slot + 0x40, sizeof l.slot);
            later.push_back(l);
        }
        for (const auto& l : later) eng::Call(row, "RemoveChild", l.widget);
        w::AddToRow(row, b, 12);
        for (const auto& l : later) {
            Obj slot = eng::Call(row, "AddChildToHorizontalBox", l.widget).ReturnObj();
            if (!slot) continue;
            uint8_t size[8], pad[16];
            std::memcpy(size, l.slot, 8);
            std::memcpy(pad, l.slot + 8, 16);
            eng::Params ps(eng::FunctionOn(slot, "SetSize"));
            ps.SetArg(0, size, sizeof size);
            eng::Invoke(slot, ps);
            eng::Params pp(eng::FunctionOn(slot, "SetPadding"));
            pp.SetArg(0, pad, sizeof pad);
            eng::Invoke(slot, pp);
            eng::Call(slot, "SetHorizontalAlignment", l.slot[0x18]);
            eng::Call(slot, "SetVerticalAlignment", l.slot[0x19]);
        }
        gAuthorButton = eng::MakeWeak(b);
        gAuthorText = eng::MakeWeak(text);
        button = b;
    }
    w::SetText(eng::Get(gAuthorText), gAuthorLabel);
    w::SetVisibility(button, 0);
    const bool pressed = eng::Call(button, "IsPressed").ReturnBool();
    if (gAuthorWasPressed && !pressed && eng::Call(button, "IsHovered").ReturnBool()) gAuthorClicked = true;
    gAuthorWasPressed = pressed;
}

void Frame() {
    ForgetHiddenOnNewResults();
    AuthorButtonFrame();
}

bool Shown() {
    Obj page = PlayPage();
    if (!page) return false;
    Obj body = eng::ReadObj(page, "Body_WS");
    Obj hub = eng::ReadObj(page, "WBP_HubHomePage_0");
    // The play page's body switches between the game's own tracks and the hub (WBP_0_Play: Body_WS).
    return body && hub && eng::Call(body, "GetActiveWidget").ReturnObj() == hub && OnScreen(page);
}

bool Search(const std::string& text, const std::string& sortName, int days, uint64_t author, const std::vector<std::string>& withTags, bool show,
            bool anyTag, bool gameTags, std::string* error) {
    const Sort* sort = nullptr;
    for (const auto& s : kSorts)
        if (sortName == s.name) sort = &s;
    if (!sort) {
        *error = "unknown sort \"" + sortName + "\"";
        return false;
    }
    // One author's maps (a Steam user list): the game's builder takes new, oldest, title and updated only (measured:
    // "Selected track sort is unavailable for an author query"); top is Steam's vote-score order of the list.
    const bool authorTop = author && sortName == "top";
    if (author && !authorTop && sortName != "new" && sortName != "oldest" && sortName != "title" && sortName != "updated") {
        *error = "\"" + sortName + "\" doesn't sort one author's maps (new, oldest, title, updated or top do)";
        return false;
    }
    if (authorTop) sort = &kSorts[2];
    Obj hub = HubPage();
    Obj list = ListView();
    Obj subsystem = Subsystem("BallestHubSubsystem");
    if (!hub || !list || !subsystem) {
        *error = "the track hub isn't there";
        return false;
    }
    const eng::Prop current = eng::FindProp(eng::ClassOf(list), "NativeQuerySpec");
    Obj build = eng::FindFunction(eng::ClassOf(subsystem), "BuildHubTrackBrowseQuerySpec");
    Obj refresh = eng::FindFunction(eng::ClassOf(list), "RefreshListView");
    if (!current || current.size != kSpecSize || !build || !refresh) {
        *error = "the hub isn't as this host knows it";
        return false;
    }

    // The game's own builder, from the list's current spec (read and copied by the call, not kept).
    eng::Params p(build);
    const std::wstring wideText = eng::Widen(text);
    const eng::FString searchText{wideText.c_str(), static_cast<int32_t>(wideText.size() + 1), static_cast<int32_t>(wideText.size() + 1)};
    p.SetArg(0, list + current.offset, kSpecSize);
    p.SetArg(1, searchText);
    p.SetArg(2, sort->browse);
    p.SetArg(3, author);
    if (!eng::Invoke(subsystem, p) || !p.ReturnBool()) {
        const uint8_t* message = p.Get("ErrorMessageOut");
        *error = "the game didn't make the search" + (message ? ": " + eng::ReadFString(message) : std::string());
        return false;
    }
    const uint8_t* built = p.Get("QuerySpecOut");
    if (!built) {
        *error = "the game didn't make the search";
        return false;
    }
    std::vector<uint8_t> spec(built, built + kSpecSize);

    // Over the game's spec: Steam's other rankings, the trend window, the first page and the tags. The arrays here
    // point at this function's memory; RefreshListView copies the spec into the list view during the call.
    if (authorTop && spec[kSpecQueryKind] == kQueryUser) spec[kSpecUserSortOrder] = kUserSortVoteScore;
    if (spec[kSpecQueryKind] == kQueryAll) {
        if (sort->steamRank >= 0) spec[kSpecQueryType] = static_cast<uint8_t>(sort->steamRank);
        if (sort->browse == 1 && sort->steamRank < 0) {
            const int32_t trend = days < 1 ? 1 : days;
            std::memcpy(spec.data() + kSpecTrendDays, &trend, sizeof trend);
        }
    }
    const int32_t firstPage = 0;
    std::memcpy(spec.data() + kSpecPageIndex, &firstPage, sizeof firstPage);
    std::vector<std::wstring> tagText;
    std::vector<eng::FString> tags;
    tagText.reserve(withTags.size());
    for (const auto& tag : withTags) {
        tagText.push_back(eng::Widen(tag));
        tags.push_back({tagText.back().c_str(), static_cast<int32_t>(tagText.back().size() + 1), static_cast<int32_t>(tagText.back().size() + 1)});
    }
    if (!tags.empty()) {
        const TArrayView array{tags.data(), static_cast<int32_t>(tags.size()), static_cast<int32_t>(tags.size())};
        std::memcpy(spec.data() + kSpecRequiredSteamTags, &array, sizeof array);
        spec[kSpecMatchAnyTag] = anyTag ? 1 : 0;
    }
    // The game's filter panel's tags, as its own filtered search sends them (QueryGroupTags without UGCTypes.Map).
    std::vector<uint64_t> groupTags;                // FGameplayTag: one FName
    eng::Params filters(nullptr);
    if (gameTags)
        if (Obj manager = UiManager()) {
            filters = eng::Call(manager, "GetCurrentTagFilters");
            if (const uint8_t* container = filters.Get("OutFilters")) {
                TArrayView array{};
                std::memcpy(&array, container, sizeof array);       // FGameplayTagContainer.GameplayTags at 0
                const auto* names = static_cast<const uint32_t*>(array.data);
                for (int32_t i = 0; names && i < array.num && i < 256; ++i)
                    if (eng::Name(names[i * 2], static_cast<int32_t>(names[i * 2 + 1])) != "UGCTypes.Map") {
                        uint64_t tag = 0;
                        std::memcpy(&tag, names + i * 2, sizeof tag);
                        groupTags.push_back(tag);
                    }
            }
        }
    const TArrayView groups{groupTags.empty() ? nullptr : groupTags.data(), static_cast<int32_t>(groupTags.size()),
                            static_cast<int32_t>(groupTags.size())};
    std::memcpy(spec.data() + kSpecQueryGroupTags, &groups, sizeof groups);
    if (!groupTags.empty() && tags.empty()) spec[kSpecMatchAnyTag] = 1;   // as the game's own filtered search

    // As WBP_HubHomePage.ForwardSearchQuery: RefreshListView(bNewHandle true, a name, bBroadcastOnComplete true, spec).
    const eng::Params name = eng::MakeText("");
    size_t nameSize = 0;
    const uint8_t* nameText = name.Return(&nameSize);
    eng::Params r(refresh);
    const uint8_t yes = 1;
    r.SetArg(0, yes);
    if (nameText) r.SetArg(1, nameText, nameSize);
    r.SetArg(2, yes);
    r.SetArg(3, spec.data(), spec.size());
    if (!r.Ok() || !eng::Invoke(list, r)) {
        *error = "the hub's list didn't take the search";
        return false;
    }
    if (nameText) eng::ReleaseText(nameText);
    if (show) eng::Call(hub, "SwitchToListView", static_cast<Obj>(nullptr));
    hostlog::Info("hub: search \"" + text + "\" by " + sortName + (author ? " of " + std::to_string(author) : "") +
                  " (query kind " + std::to_string(spec[kSpecQueryKind]) + ")" +
                  (tags.empty() ? "" : ", " + std::to_string(tags.size()) + " tag(s)") +
                  (groupTags.empty() ? "" : ", " + std::to_string(groupTags.size()) + " filter tag(s)"));
    return true;
}

namespace {

// Badges on list entries' thumbnails (SetEntryBadge): one per entry widget, kept and re-labelled as the game reuses
// the entry for other maps.
struct Badge {
    eng::Weak entry, root, text;
};
std::vector<Badge> gBadges;

// A rounded, filled rectangle of a fixed size.
Obj RoundBlock(Obj outer, float width, float height, ui::Color c, double radius) {
    Obj box = w::Spawn("SizeBox", outer), fill = w::Spawn("Border", outer);
    if (!box || !fill) return nullptr;
    eng::Call(box, "SetWidthOverride", width);
    eng::Call(box, "SetHeightOverride", height);
    w::RoundCorners(fill, radius);
    eng::Call(fill, "SetBrushColor", c);
    w::AddChild(box, fill);
    return box;
}

// The panel a badge goes in: the nearest Overlay holding the entry's thumbnail (WBP_HubListEntry.TrackImage). The
// chain up from the thumbnail is logged once, so where it lands can be checked.
Obj BadgePanel(Obj entry) {
    static Obj overlayClass = eng::FindClass("Overlay");
    // the list's entries: the row's own overlay (around its Background image), so the badge can sit at its right end
    static Obj tileClass = eng::FindClass("WBP_TrackPreview_C");
    if (!(tileClass && eng::IsA(entry, tileClass))) {
        Obj background = eng::ReadObj(entry, "Background");
        Obj parent = background ? eng::Call(background, "GetParent").ReturnObj() : nullptr;
        static bool loggedRow = false;
        if (!loggedRow) {
            const std::string path = parent ? eng::PathOf(eng::ClassOf(parent)) : "none";
            hostlog::Info("hub badges: a list entry's Background is in a " + path.substr(path.rfind('.') + 1));
            loggedRow = true;
        }
        if (parent && overlayClass && eng::IsA(parent, overlayClass)) return parent;
    }
    Obj image = eng::ReadObj(entry, "TrackImage");
    std::string chain;
    Obj found = nullptr;
    Obj at = image;
    for (int i = 0; at && i < 4 && !found; ++i) {
        Obj parent = eng::Call(at, "GetParent").ReturnObj();
        if (parent) chain += std::string(chain.empty() ? "" : " < ") + eng::PathOf(eng::ClassOf(parent)).substr(eng::PathOf(eng::ClassOf(parent)).rfind('.') + 1);
        if (parent && overlayClass && eng::IsA(parent, overlayClass)) found = parent;
        at = parent;
    }
    static bool logged = false;
    if (!logged) hostlog::Info("hub badges: the thumbnail's parents: " + (chain.empty() ? std::string("none") : chain) +
                               (found ? "; badges go in the first Overlay" : "; no Overlay, so no badges"));
    logged = true;
    return found;
}

Obj MakeBadge(Obj entry, Obj* textOut) {
    Obj panel = BadgePanel(entry);
    Obj outer = eng::ReadObj(entry, "WidgetTree");
    if (!panel || !outer) return nullptr;
    Obj back = w::Spawn("Border", outer), row = w::Spawn("HorizontalBox", outer), person = w::Spawn("VerticalBox", outer),
        text = w::Spawn("TextBlock", outer);
    if (!back || !row || !person || !text) return nullptr;
    w::RoundCorners(back, 7);
    eng::Call(back, "SetBrushColor", ui::Color{0, 0, 0, 0.62f});
    eng::Call(back, "SetPadding", w::Margin{6, 2, 7, 2});
    // a little person: a round head over rounded shoulders
    const ui::Color white{1, 1, 1, 0.95f};
    Obj headSlot = eng::Call(person, "AddChildToVerticalBox", RoundBlock(outer, 7, 7, white, 3.5)).ReturnObj();
    Obj bodySlot = eng::Call(person, "AddChildToVerticalBox", RoundBlock(outer, 12, 6, white, 3)).ReturnObj();
    if (headSlot) eng::Call(headSlot, "SetHorizontalAlignment", w::kAlignCenter);
    if (bodySlot) {
        eng::Call(bodySlot, "SetHorizontalAlignment", w::kAlignCenter);
        eng::Call(bodySlot, "SetPadding", w::Margin{0, 1, 0, 0});
    }
    w::AddToRow(row, person, 0);
    w::SetFontSize(text, 13);
    w::SetTextColor(text, white);
    w::AddToRow(row, text, 5);
    w::AddChild(back, row);
    // Home's tiles (WBP_TrackPreview) have the map's name along the bottom: their badge goes top-left (measured on
    // screen); the list's entries keep it bottom-left.
    static Obj tileClass = eng::FindClass("WBP_TrackPreview_C");
    const bool tile = tileClass && eng::IsA(entry, tileClass);
    if (tile) w::AddToOverlay(panel, back, w::kAlignLeft, w::kAlignLeft, w::Margin{8, 8, 0, 0});
    else w::AddToOverlay(panel, back, w::kAlignEnd, w::kAlignCenter, w::Margin{0, 0, 14, 0});     // the row's right end
    w::SetVisibility(back, w::kHitTestInvisible);           // clicks go to the entry as before
    *textOut = text;
    return back;
}
}  // namespace

// Every map thumbnail on screen: the shown list's entries, or on the hub's Home its category rows' tiles
// (WBP_HubHomePage.CategoryRows[].CategoryWidgets[], WBP_TrackPreview_C with "Published File Id"; types dump).
std::vector<Entry> ThumbnailEntries() {
    if (ShownList()) return ListEntries();
    std::vector<Entry> out;
    Obj hub = HubPage();
    if (!hub || !OnScreen(hub)) return out;
    for (Obj row : eng::ReadObjArray(hub, "CategoryRows")) {
        if (!row || !OnScreen(row)) continue;
        for (Obj tile : eng::ReadObjArray(row, "CategoryWidgets")) {
            int64_t id = 0;
            if (tile && eng::ReadBytes(tile, "Published File Id", &id, sizeof id) && id > 0 && eng::ReadObj(tile, "TrackImage"))
                out.push_back({static_cast<uint64_t>(id), tile});
        }
    }
    return out;
}

std::vector<uint64_t> Thumbnails() {
    std::vector<uint64_t> ids;
    for (const auto& e : ThumbnailEntries())
        if (std::find(ids.begin(), ids.end(), e.id) == ids.end()) ids.push_back(e.id);
    return ids;
}

bool SetEntryBadge(uint64_t id, const std::string& text) {
    bool any = false;
    for (const auto& e : ThumbnailEntries()) {
        if (e.id != id) continue;
        Badge* badge = nullptr;
        for (auto& b : gBadges)
            if (eng::Get(b.entry) == e.widget && eng::Get(b.root) && eng::Get(b.text)) badge = &b;
        if (!badge) {
            if (text.empty()) continue;
            Obj label = nullptr;
            Obj root = MakeBadge(e.widget, &label);
            if (!root) continue;
            gBadges.erase(std::remove_if(gBadges.begin(), gBadges.end(), [](const Badge& b) { return !eng::Get(b.entry); }),
                          gBadges.end());
            gBadges.push_back({eng::MakeWeak(e.widget), eng::MakeWeak(root), eng::MakeWeak(label)});
            badge = &gBadges.back();
        }
        w::SetText(eng::Get(badge->text), text);
        w::SetVisibility(eng::Get(badge->root), text.empty() ? w::kCollapsed : w::kHitTestInvisible);
        any = true;             // the same map can be in several of Home's rows: every tile gets it
    }
    return any;
}

bool ListShown() { return ShownList() != nullptr; }

std::string View() {
    if (!Shown()) return "";
    Obj list = ShownList();
    if (list && list == ListView()) return "list";
    if (list) return "subscribed";
    return "home";
}

void SetAuthorButton(const std::string& label) { gAuthorLabel = label; }
bool AuthorButtonClicked() {
    const bool c = gAuthorClicked;
    gAuthorClicked = false;
    return c;
}

std::vector<uint64_t> Entries() {
    std::vector<uint64_t> ids;
    for (const auto& e : ListEntries())
        if (e.inUse && (HiddenByPlugin(e.widget) || eng::Call(e.widget, "IsVisible").ReturnBool())) ids.push_back(e.id);
    return ids;
}

bool HideEntry(uint64_t id, bool hidden) {
    for (const auto& e : ListEntries()) {
        if (e.id != id) continue;
        if (!e.inUse) {             // left over from earlier results: stays as the game left it, and is forgotten
            gHidden.erase(std::remove_if(gHidden.begin(), gHidden.end(), [&](const Hidden& h) { return eng::Get(h.widget) == e.widget; }),
                          gHidden.end());
            continue;
        }
        const bool ours = HiddenByPlugin(e.widget);
        if (!ours && !eng::Call(e.widget, "IsVisible").ReturnBool()) continue;     // the game's: not in use
        if (hidden && !ours) {
            eng::Call(e.widget, "SetVisibility", uint8_t{1});               // ESlateVisibility::Collapsed
            gHidden.push_back({eng::MakeWeak(ShownList()), eng::MakeWeak(e.widget)});
        } else if (!hidden && ours) {
            eng::Call(e.widget, "SetVisibility", uint8_t{0});               // Visible, as the game shows entries
            for (auto it = gHidden.begin(); it != gHidden.end(); ++it)
                if (eng::Get(it->widget) == e.widget) {
                    gHidden.erase(it);
                    break;
                }
        }
        return true;
    }
    return false;
}

namespace {
bool FocusedDetails(uint64_t* id, uint64_t* owner) {
    Obj page = PlayPage();
    Obj panel = page ? eng::ReadObj(page, "WBP_HubInformationPanel") : nullptr;
    const eng::Prop details = panel ? eng::FindProp(eng::ClassOf(panel), "ActiveUGCDetails") : eng::Prop{};
    if (!details) return false;
    // SIK_SteamUGCDetails: PublishedFileId at 0, Owner at 0x10 (type dump).
    std::memcpy(id, panel + details.offset, sizeof *id);
    std::memcpy(owner, panel + details.offset + 0x10, sizeof *owner);
    return true;
}
}  // namespace

uint64_t Focused() {
    uint64_t id = 0, owner = 0;
    return FocusedDetails(&id, &owner) ? id : 0;
}

uint64_t FocusedAuthor() {
    uint64_t id = 0, owner = 0;
    return FocusedDetails(&id, &owner) && id ? owner : 0;
}

bool WidgetOnScreen(Obj widget) { return widget && OnScreen(widget); }

Obj DockPanel() {
    Obj hub = HubPage();
    Obj header = hub ? eng::ReadObj(hub, "WBP_HubHeader") : nullptr;
    return header ? eng::Call(header, "GetParent").ReturnObj() : nullptr;
}

void SetDockedHeight(double height) {
    Obj hub = HubPage();
    Obj box = hub ? eng::ReadObj(hub, "HubHome_SB") : nullptr;
    if (!box) return;
    if (eng::Get(gSizedFor) != box) {
        float original = 0;
        if (!eng::ReadBytes(box, "HeightOverride", &original, sizeof original) || original <= 0) return;
        gSizedFor = eng::MakeWeak(box);
        gOriginalHeight = original;
        gAppliedHeight = -1;
    }
    if (std::fabs(height - gAppliedHeight) < 0.5) return;
    gAppliedHeight = height;
    eng::Call(box, "SetHeightOverride", static_cast<float>(gOriginalHeight - height > 100 ? gOriginalHeight - height : 100));
}

}  // namespace hub
