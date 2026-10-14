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
int ExtraRowGhostClicked();                     // the row whose ghost toggle (its right end) was clicked, or -1
void TestGhostRow(int row);                     // test hook: as if that row's ghost toggle were clicked
void TestHoverRow(int row);                     // test hook: as if the mouse were over that row's tile (-1: none)
void TestClickRow(int row);                     // test hook: as if that row were clicked
void TestPinRow(int row);                       // test hook: as if that row's rank were clicked
// Hiscores from another source for one track: while the race's track key (race::CurrentTrack().key) is `trackKey`, the
// in-map leaderboard shows these rows IN PLACE of the game's own list (world/friends), with no button to switch: the
// same rows of the game's own kind as SetExtraRows builds (rank, name, time set as the game sets its own), the title
// replaced. `ranks` (0 or missing: the row's position) lets a row stand apart from the list (the player's own, 57th);
// `ghosts` (optional) sets each row's ghost toggle look (on/off); clicks on rows come through ExtraRowClicked. `highlight` marks one row (the lime bar and rank of a pinned row), -1 none. Empty `names` with the key set shows the
// title alone (loading). The last plugin to set a source owns it; ClearSource gives the board back to the game.
// Measured 2026-10-12 in a slot: a local map raced with Tracks::OpenLocal shows the in-map board (title "leaderboard",
// world/friends buttons, five empty rows), so there is a board to fill.
void SetSource(int owner, const std::string& trackKey, const std::string& title, const std::vector<std::string>& names,
               const std::vector<double>& times, const std::vector<int>& ranks, int highlight,
               const std::vector<bool>& ghosts = {});
void ClearSource(int owner);
// A source row's ghost as the game's own replay ghost (nativeghost.hpp): the replay is handed to the game's ghosts for
// that player (a ghost session opened for the local map if the game has none), selected as the leaderboard's own
// toggle selects one (ForceShow; the one selected before ForceHide), the row lit (its ghost toggle on, its background
// the game's lime), and the race restarted the way the restart key restarts it (controller ManuallyRestartBall), so
// the ghost runs from the start with the race clock. ClearRowGhost hides it again and restarts. Only while the
// source applies (its track key is the race's); false otherwise or if the game refused the replay (`error`).
bool SetRowGhost(int owner, int row, const std::string& steamId, const std::string& replayJson, std::string* error);
bool ClearRowGhost(int owner);
// A source row's replay in the game's replay mode, as the board's own row click (nativeghost::Watch): the replay handed
// to the game's ghosts like SetRowGhost's, then the game's POV of that ghost, with every selected ghost still shown;
// the row marked as watched (the game's POV row look, SetReplayIsActive) while it lasts. No restart. The game's own
// exit from replay mode returns to the race (and restarts it, as the game does).
bool WatchRowReplay(int owner, int row, const std::string& steamId, const std::string& replayJson, std::string* error);
std::string RowGhost();                         // the Steam id whose ghost a source row shows now, or ""
// The player's own best ghost on the source's track from a replay (e.g. their online best), as the game's own PB ghost:
// the race's ghost session is opened (the game's own set-up) and the replay handed over under the player's own Steam
// id, so the game shows it as their personal best (by their ghost settings), also in replay mode. "" removes it.
bool SetOwnReplay(int owner, const std::string& replayJson, std::string* error);
bool SourceShown();                             // the board on screen shows a plugin's source now
void RemoveOwner(int owner);

}  // namespace leaderboard
