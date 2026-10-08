// The leaderboard shown inside a map (WBP_RaceUIManager's WBP_Leaderboard): how many players it has, and a note after
// its title. Measured: the widget keeps the board it shows in NativeActiveLeaderboardRecord (with its Steam handle,
// LeaderboardHandle), its title is the text block TXT_HEader, and Steam's entry count for a handle comes from the
// game's Steam Integration Kit (SIK_UserStatsLibrary.GetLeaderboardEntryCount, the handle passed as a plain number).
// Game thread only.
#pragma once
#include <string>
#include <vector>

namespace leaderboard {

void Frame();                                   // keeps the note on the title (the game rebuilds its UI)
int Players();                                  // players on the board on screen inside a map, or -1
void SetTitleNote(int owner, const std::string& note);   // "" removes it; `owner` is the plugin (last one wins)
// The main menu's overall leaderboard (the bar at the top, WBP_HeaderSeasonScore): how many players it has, and a note
// before its "overall" label. Read from the game's Blueprint: the bar finds the season's Steam leaderboard by name and
// keeps its id in its graph's Temp_int_Variable; the label is its text block named "TextBlock".
int OverallPlayers();                           // players on the overall leaderboard, or -1 (not on the main menu)
void SetOverallNote(int owner, const std::string& note);
// Rows as a third view of the in-map leaderboard: a button of the game's own kind beside its world and friends ones
// switches the board to them (and those two back). They're the game's own leaderboard rows (WBP_LeaderboardEntry_C:
// rank, name, a time set the way the game sets its own, and the ghost toggle's look), with the title as the board's.
// Each row is covered by a see-through button, so clicks come here instead of starting the game's own ghost download.
// No names: no button. The last plugin to set rows owns them.
// A row's rank (its left end) is a second click target, for pinning: pinned rows show a lime bar on their left edge
// and a lime rank.
void SetExtraRows(int owner, const std::string& title, const std::vector<std::string>& names, const std::vector<double>& times,
                  const std::vector<bool>& ghosts, const std::vector<bool>& pinned);
void SetExtraRowsIcon(int owner, const std::wstring& file);   // the button's image (a PNG/JPEG); "" for the game's list icon
bool ExtraRowsShown();                          // the board shows the rows (their button was clicked)
void TestScroll(float offset);                 // test hook: the rows scrolled to this offset
void TestToggleTab();                           // test hook: as if the rows' button (or, when open, world) were clicked
int ExtraRowClicked();                          // the row clicked since last asked (not on its rank), or -1
int ExtraRowPinClicked();                       // the row whose rank was clicked since last asked, or -1
void TestClickRow(int row);                     // test hook: as if that row were clicked
void TestPinRow(int row);                       // test hook: as if that row's rank were clicked
void RemoveOwner(int owner);

}  // namespace leaderboard
