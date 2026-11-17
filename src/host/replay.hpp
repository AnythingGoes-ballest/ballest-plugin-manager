// Replays: whether one is being watched, its playback clock and length, and the replay camera modes. Built on
// the game's own replay system (measured): BallestGhostWorldSubsystem for playback, the replay camera
// BP_FreeCam_C and its AC_GhostFollowCam component for following. Game thread only.
#pragma once
#include <string>
#include <vector>

namespace replay {

void Frame(float dt);                       // after game::Frame and input::Frame

bool Active();
double Time();
double Length();                            // the replay's recorded duration; <= 0 while unknown
void Seek(double seconds);
void Restart();

enum Camera { CameraDefault = 0, CameraFollow3D = 1, CameraFree = 2 };
int CameraMode();
void SetCameraMode(int mode);
// How far the camera stays from the ball, in whole units: the replay camera's arm length (default mode) and the
// chase distance (follow 3D). 0 is the game's own.
double CameraDistance();
void SetCameraDistance(double units);
// Pieces between the camera and the ball turn to glass, and the camera is no longer pulled in by them.
bool SeeThrough();
void SetSeeThrough(bool on);

// A plugin-owned replay made from two to four recordings. This only owns the native ghost session and playback;
// camera, HUD and game-mode rules remain the plugin's. All functions are scoped to the plugin that prepared the
// ghosts.
bool PrepareGhosts(int owner, const std::vector<std::string>& names, const std::vector<std::string>& recordings);
bool GhostsReady(int owner);
bool StartGhosts(int owner);
void StopGhosts(int owner);
double GhostTime(int owner);
int GhostCount(int owner);
std::string GhostName(int owner, int index);
double GhostDuration(int owner, int index);         // recording duration, or -1 for an invalid index
std::string GhostError(int owner);
void RemoveOwner(int owner);

// Test hook: a simulated replay (its own clock at 1x, restarting at the end) behind the same functions, for
// testing replay plugins when no real replay can be started.
void Simulate(bool on, double length);
std::string TestLoadGlass();              // test hook: loads the glass material, reports before and after

}  // namespace replay
