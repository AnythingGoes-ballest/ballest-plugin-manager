// Races: whether one is running and how often it was restarted from the beginning. Measured on the game:
//   * the player controller (BP_MyPlayerController_C) has bRaceActive: on while racing, including through
//     checkpoint respawns; off in menus, after the finish, and for the moment of a restart
//   * the ball (the controller's pawn, BP_RollingBall_C) has RestartCounterThisSession, which the game raises by
//     one on every restart from the beginning of the track (Backspace, or R before the first checkpoint), and not
//     for checkpoint respawns, falls or the track's first start. Each map has a new ball whose counter starts
//     again, so counting follows the ball: a different ball is a new baseline, not a restart.
// Also the track being raced (name, author, author time, an identity to keep records by), saving and restoring the
// ball, pausing, and practice runs that can't finish. Game thread only.
#pragma once
#include <string>

namespace race {

void Frame();                   // after game::Frame

bool OnTrack();                 // a race controller exists (a track is loaded, not the main menu)
bool Active();                  // a race is running
int Restarts();                 // restarts from the beginning since the host started
int RunId();                    // the ball's RaceId: a new value for every new run, the same through respawns; -1 off track
bool Complete();                // the run has been finished (the controller's bRaceComplete)
// What the player is steering with right now, as the ball reads it (BP_RollingBall_C InputAxes: x right, y forward,
// -1..1; InputJump), whatever keys or controller they use. False off track.
bool Input(double* x, double* y, bool* jump);

// The track on screen, read from the race UI once it has filled in (re-read every second until then):
//   * name and author: the race header (WBP_Header in WBP_RaceUIManager: Title, authorText; the game's own tracks
//     show the placeholder "playername" as author, which reads as "")
//   * author time: the medal row's author entry (WBP_IngameGoalsGroup.WBP_AuthorGoal.UserGoal_Numeric); the
//     controller's AuthorTime is only right on the game's own tracks
//   * key: custom tracks all play on Map_TrackUGCPlayback, with the file in the game instance's TargetLevelName
//     (".../workshop/content/<app>/<id>/Map_LevelEditorMain&<track>_<author>.ballmap"): "custom:<id or local>:<file>";
//     the game's own tracks: "map:<map name>"
struct Track {
    std::string key, name, author;
    double authorTime = 0;          // seconds, 0 if unknown
    bool custom = false;
};
const Track& CurrentTrack();

bool SetPaused(bool paused);    // GameplayStatics.SetGamePaused
bool Paused();

// The ball's state, as text: position, rotation, velocities, the camera's control rotation, spring arms and camera.
// "" when there is no ball.
std::string SaveBall();
// Puts the ball back in a saved state (without its velocity unless `momentum`). Always turns practice on first: a
// run with a restored position can't finish.
bool LoadBall(const std::string& state, bool momentum);

// Practice: the timer stops and shows 999:999.999, checkpoints and the finish stop responding, the leaderboard,
// personal standing and ghosts are hidden. There is no turning it off: it ends by itself on a new run (RaceId
// changes), a new ball or a new map, and what it hid comes back then. So a run that was practised can't finish.
void StartPractice();
bool Practice();

}  // namespace race
