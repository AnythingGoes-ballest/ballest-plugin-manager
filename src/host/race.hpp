// Races: whether one is running and how often it was restarted from the beginning. Measured on the game:
//   * the player controller (BP_MyPlayerController_C) has bRaceActive: on while racing, including through
//     checkpoint respawns; off in menus, after the finish, and for the moment of a restart
//   * the ball (the controller's pawn, BP_RollingBall_C) has RaceId: the run's id while racing, 0 before the first
//     Play, -1 after a finish. A restart from the beginning (the Restart race key, or R before the first checkpoint)
//     keeps the ball and gives it a new RaceId. Since the game's 2026-10-06 update its RestartCounterThisSession no
//     longer rises on a restart, so restarts are counted by new raced run ids. Each map has a new ball, so counting
//     follows the ball: a different ball's first run is not a restart.
// Also the track being raced (name, author, author time, an identity to keep records by), saving and restoring the
// ball, pausing, and practice runs that can't finish. Game thread only.
#pragma once
#include <string>

#include "engine.hpp"

namespace race {

void Frame();                   // after game::Frame

bool OnTrack();                 // a race controller exists (a track is loaded, not the main menu)
bool Active();                  // a race is running
// The upload guard's question: whether this run must not reach a leaderboard, and why (the host touched it: a test
// command that changes the game, the ball moved by the host, practice, or game time not at normal speed).
bool RunTainted(std::string* why);
void TaintRun(const std::string& why);  // from now until the next run
int Restarts();                 // restarts from the beginning since the host started
int Respawns();                 // respawns at a checkpoint from R (not falls) since the host started
int Falls();                    // falls into a kill zone since the host started (each respawns at a checkpoint or the start)
// The map's checkpoints (strips and discs, BP_ActualCheckpointBase_C), ordered by position; 0 off track.
int CheckpointCount();
bool CheckpointPosition(int index, double* x, double* y, double* z);
int CurrentCheckpoint();        // the one a respawn goes to (its bCurrent), or -1 (the start)
bool MoveBall(double x, double y, double z);    // tests only: teleports the ball being played
bool CheckpointTrigger(int index, double* x, double* y, double* z);    // tests only: where a strip's trigger is
bool BallPosition(double* x, double* y, double* z);     // the ball being played (also in the editor's test runs)
eng::Obj PlayedBallActor();                             // that ball's actor, or null
bool EditorTesting();           // in the track editor, a test run is on (the camera is on the ball)
int RunId();                    // the ball's RaceId: a new value for every new run, the same through respawns; -1 off track
bool Complete();                // the run has been finished (the controller's bRaceComplete)
// What the player is steering with right now, as the ball reads it (BP_RollingBall_C InputAxes: x right, y forward,
// -1..1; InputJump), whatever keys or controller they use. False off track.
bool Input(double* x, double* y, bool* jump);

// Bounces: the ball being played hitting the ground or a wall hard enough. Read from BP_RollingBall: its Sphere's
// OnComponentHit is bound to a Blueprint event of the ball (BndEvt__..._ComponentHitSignature, with the hit's
// NormalImpulse), which the engine calls through the delegate; that function is wrapped (as the cosmetics wrap the
// Customize page's handler) to see every hit. (The game's own PlayCollisionSFX is called from Blueprint code, which
// doesn't go through a function's native entry: wrapping it saw nothing, measured.) Which hits count, and how strong
// they are, is measured: see kBounce* in race.cpp.
//   strength: how hard, 0..1 (about: soft below 0.4, medium to 0.75, hard above)
//   x, y, z: where the ball touched (its middle, less its radius along the push); nx, ny, nz: the push's direction
//   (the surface's normal, pointing out of it); ground: the push is mostly upward (a floor, not a wall)
struct Bounce {
    int serial = 0;                 // 1, 2, 3...: each bounce's own number
    double strength = 0, impulse = 0;
    double x = 0, y = 0, z = 0, nx = 0, ny = 0, nz = 1;
    bool ground = false;
};
// The first of the last 64 bounces after serial `after` (0: the oldest kept), false if none. Each reader keeps its own
// place, so plugins don't take each other's bounces. LatestBounce is the newest's serial (0 before any).
bool BounceAfter(int after, Bounce* out);
int LatestBounce();

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
    std::string image;              // custom tracks: the .jpg beside the map file, if there is one
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
