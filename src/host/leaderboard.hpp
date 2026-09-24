// The leaderboard shown inside a map (WBP_RaceUIManager's WBP_Leaderboard): how many players it has, and a note after
// its title. Measured: the widget keeps the board it shows in NativeActiveLeaderboardRecord (with its Steam handle,
// LeaderboardHandle), its title is the text block TXT_HEader, and Steam's entry count for a handle comes from the
// game's Steam Integration Kit (SIK_UserStatsLibrary.GetLeaderboardEntryCount, the handle passed as a plain number).
// Game thread only.
#pragma once
#include <string>

namespace leaderboard {

void Frame();                                   // keeps the note on the title (the game rebuilds its UI)
int Players();                                  // players on the board on screen inside a map, or -1
void SetTitleNote(int owner, const std::string& note);   // "" removes it; `owner` is the plugin (last one wins)
void RemoveOwner(int owner);

}  // namespace leaderboard
