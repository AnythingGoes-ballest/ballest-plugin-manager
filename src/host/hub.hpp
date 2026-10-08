// The game's track hub (the workshop side of the play page, WBP_HubHomePage), driven from plugins: searches shown in
// the hub's own list, its list's entries, the map its info panel shows, and a place in its layout for a plugin's row
// of widgets (Window.DockInHub).
//
// A search goes the way the game's own filtered search goes (read from WBP_HubHomePage.SubmitFilteredSearchQuery and
// ForwardSearchQuery): a BallestHubQuerySpec handed to the list view's RefreshListView(true, name, true, spec), then
// SwitchToListView(None). The spec is made by the game's own BallestHubSubsystem.BuildHubTrackBrowseQuerySpec (search
// text, sort, author) from the list view's current spec, so paging, entries, the info panel, medals and playing all
// stay the game's. Game thread only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "engine.hpp"

namespace hub {

void Frame();

bool Shown();                           // the hub is on screen

// Shows a search in the hub's list. sort: relevance, top, trending, new, oldest, title, updated (the game's own
// sorts), played, subscribed, liked (Steam's, for searches of every map). author: a Steam id, 0 for anyone. withTags:
// Steam tags a map needs (all of them, or any one with anyTag); gameTags: also the tags picked in the game's own
// filter panel. False with `error` set if the hub isn't there or the game refused.
// show: switch to the list (false: the list gets the results, the hub stays on the page it's on).
bool Search(const std::string& text, const std::string& sort, int days, uint64_t author, const std::vector<std::string>& withTags, bool show,
            bool anyTag, bool gameTags, std::string* error);

// The list on screen (the main list, or the Subscribed page's), false on the hub's Home. Its maps, in order (entries
// the game has hidden are left out; none on Home), and hiding some of them.
bool ListShown();
std::string View();                     // the hub's page on screen: "home", "list", "subscribed"; "" without the hub
std::vector<uint64_t> Entries();
bool HideEntry(uint64_t id, bool hidden);
// Every map whose thumbnail is on screen: the shown list's, or the tiles of the hub's Home.
std::vector<uint64_t> Thumbnails();
// A small badge on every on-screen thumbnail of a map: a person icon and `text` (e.g. how many people have it). "" hides it.
// Re-set it whenever the list changes: the game reuses entries for other maps.
bool SetEntryBadge(uint64_t id, const std::string& text);

// A button after the author's name in the info panel ("" removes it), and whether it was clicked since last asked.
void SetAuthorButton(const std::string& label);
bool AuthorButtonClicked();

uint64_t Focused();                     // the map the info panel shows, 0 for none
uint64_t FocusedAuthor();

// Where a docked window goes: the hub's column above its pages (WBP_HubHomePage: VerticalBox_1, which holds the
// header and the page row); null when there is no hub.
eng::Obj DockPanel();
// A widget on screen: visible itself and up its parents, the active child of any switcher on the way, in the viewport.
bool WidgetOnScreen(eng::Obj widget);
// The docked window's height, so the hub's pages are made that much shorter and the hub keeps its size on screen.
void SetDockedHeight(double height);

}  // namespace hub
