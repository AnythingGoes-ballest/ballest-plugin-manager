// The game's own records (best time, medal, splits, PB ghost data) of the player's own never-published maps, for a
// plugin that keeps its records elsewhere (a server). Read from the game (AssetDumper pseudocode of
// BP_BallgameGameInstance and BP_MyPlayerController):
//   * a finish that becomes the record is saved by the controller calling the game instance's
//     AsyncSaveProfileChanges(SaveParams: SBallerProfile, bSyncronous, FinishPolicy); the save queue then writes
//     SaveParams.HighScoreToSave into the profile's HighScores[levelName] and LevelNameMedalMap[levelName] (the medal
//     from LevelManager.GetMedalForLevelFromHighscore) - only if HighScoreToSave.levelName isn't "" - and saves the
//     profile with SaveGameToSlot itself;
//   * the other fields are applied only when set (a name that isn't "", valid skin/accessory/goal objects,
//     bHasSeenWelcome true, skin preferences that differ from the profile's).
// So a record is cleared through that same function: a save request whose record for the level is empty (no time,
// no splits, no replay samples), every other field left as the profile has it, so the game writes and saves it
// itself. New records are kept out by the game's own rule: a finish whose posting snapshot is practice-only is no
// personal best (BallestScorePostingSubsystem.CaptureFinishPostingSnapshot wrapped: on a suppressed level its
// snapshot comes back practice-only), for the suppressed levels only. Only for maps whose saved file in the player's
// UserSavedMaps folder was never published. Game thread only.
#pragma once
#include <string>

namespace localrecords {

// The game's record for the level (the map's display name, as Race::TrackName shows it) replaced by an empty one,
// saved by the game: at once while that map is raced and is the player's own never-published map
// (race::OwnUnpublishedMapHere), else the next time it is (Frame). False if the level isn't one of the player's own
// unpublished maps, or the game can't be asked.
bool Clear(const std::string& levelName, std::string* error);
void Frame();                   // clears asked for earlier, once their map is raced
// From now on (this session) the game saves no record for the level when a finish on it is taken while that map is the
// player's own never-published one (a track of another kind with the same name keeps its records). Several levels may
// be suppressed.
bool Suppress(const std::string& levelName, std::string* error);
void Unsuppress(const std::string& levelName);
// The game's record for the level as the profile holds it now: best time (seconds, 0 none), whether found.
bool Read(const std::string& levelName, double* best, int* splits, int* samples);
int Medal(const std::string& levelName);        // the profile's medal for the level (Enum_MedalGranted), -1 none
// The game's finish snapshots watched (each finish tells race::NoteFinish); suppressing a level needs it too. False if
// the game's CaptureFinishPostingSnapshot isn't as measured.
bool WatchFinishes(std::string* error);

}  // namespace localrecords
