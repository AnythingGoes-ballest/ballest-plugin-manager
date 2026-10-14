// The game's own replay ghosts (BallestGhostWorldSubsystem) fed with replays the host was handed, so they behave
// exactly like the leaderboard's own ghosts: spawned, moved, dressed and name-tagged by the game. Read from the game
// (AssetDumper pseudocode of WBP_LeaderboardEntry, BP_MyPlayerController, BP_BallgameGameInstance):
//   * a race opens a ghost session for its leaderboard identity (game instance EnsureNativeGhostSession ->
//     OpenGhostSession(GameplayIdentity, own Steam id)); a ghost is named by a BallestGhostKey = MakeGhostKey(the
//     session's identity, the player's Steam id as text);
//   * a row's ghost toggle (ClickGhostButton): on = SetGhostSelection(key, ForceShow) then, if the subsystem has no
//     replay for the key yet, BeginReplayRequest(key) -> ticket, the replay downloaded from the row's Steam UGC file,
//     AcceptReplayJson(ticket, raw JSON); off = SetGhostSelection(key, ForceHide). The row's look follows the
//     record (ApplyGhostRecordState -> SetGhostIsActive) through OnGhostRecordChanged.
// Here the replay text comes from a plugin instead of Steam; everything after AcceptReplayJson is the game's.
// Game thread only.
#pragma once
#include <string>

namespace nativeghost {

bool HasSession();
// Replays handed over from now on are drawn in the player's real look (their skin material for the ghost variant,
// opaque) instead of the game's see-through ghost look.
void SetRealSkins(bool on);
std::string Appearance();                     // test: the session's ghost appearance layers
// A local map has no leaderboard, so the game opens no ghost session for it (measured: none on a Tracks::OpenLocal
// race). This opens one as the game does (OpenGhostSession with an identity made by the game's own
// MakeUGCLeaderboardIdentity for the active level, file id -1, `boardName`), ghosts enabled and shown.
bool OpenSession(const std::string& boardName, std::string* error);
std::string SessionDescription();       // for tests: the session's identity and its ghost records
// The replay of a player (Steam id as text) handed to the game for the session's track. Kept by the game for the
// session; false with `error` if the game refused it.
bool Load(const std::string& steamId, const std::string& replayJson, std::string* error);
bool HasReplay(const std::string& steamId);
// The game has parsed it (record LoadState Ready with a replay): AcceptReplayJson only queues the parse (measured: the
// follow camera refuses a ghost that isn't ready, so replay mode has to wait for this).
bool Ready(const std::string& steamId);
// 0 inherit (the game's own settings), 1 show, 2 hide: as the leaderboard's own toggle sets it.
bool Select(const std::string& steamId, int visibility, std::string* error);
bool Visible(const std::string& steamId);     // the game shows that player's ghost now (selected and loaded)
// The game's replay mode for a player's loaded replay, as its board's row click does (WBP_Leaderboard
// OnEntryClicked_Handler -> controller PlayerWantsToPOVGhost(key): SetGhostActive, SetGhostSelection(ForceShow), the
// ghost follow camera on that ghost (AC_GhostFollowCam.SetNativeGhostSource), bPOVSpectating, the POV timer; every
// other selected ghost keeps playing). The game's own exit (PlayerWantsToStopPOV -> FinishNativePOV: the camera back on
// the ball, ForceRestartBall) returns to the race.
bool Watch(const std::string& steamId, std::string* error);
bool StopWatching();
bool Watching();                              // the controller is in the game's replay mode (bPOVSpectating)
// The replay handed to the game as one of its rivals (its rival roster; the game's own request for the replay is
// answered with this JSON), so its record has the game's "current rival" flag like a leaderboard rival's. Needs a
// session opened by the game's own set-up (OpenSession does that first).
bool LoadAsRival(const std::string& steamId, const std::string& replayJson, std::string* error);
std::string DebugWatch(const std::string& steamId);   // test: the follow camera's answer for that ghost
std::string DebugReopenAs(const std::string& localId);
// The game's replay camera follows only the session's own ghost (measured), so replay mode for another player's
// replay needs a session whose local player is that player: OpenAsLocal closes the session and opens one so (core
// identity of the level, ghosts shown). CloseSession closes it (OpenSession then opens the normal one again).
bool OpenAsLocal(const std::string& localId, std::string* error);
void CloseSession();
// The player's own saved best into the open session, by the game's own function (as its session set-up does).
bool LoadOwnBest();
// Whether the race's session wants the player's own best shown (its toggle, else the player's ghost settings).
bool OwnBestWanted(const std::string& steamId);
int Override(const std::string& steamId);     // a record's visibility override (0 inherit, 1 show, 2 hide)
// Replay mode: the watched ghost's name tag shows `name` (the game names the session's own ghost after the local player).
void NameWatchedTag(const std::string& steamId, const std::string& name);
std::string NameTags();                                                   // test: every ghost name tag (WBP_GhostBallNameTag_C) and its text
std::string RenameTags(const std::string& from, const std::string& to);   // test

}  // namespace nativeghost
