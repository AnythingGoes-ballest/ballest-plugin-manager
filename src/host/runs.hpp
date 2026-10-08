// The player's own recent runs, finished or not: each attempt at a track from the race's start until a restart from
// the beginning, the finish, or leaving the track (checkpoint respawns and falls stay in the same run); not practice
// runs. Recorded about ten times a second in the game's own replay format (the JSON of Saved\Ghosts: locations, elapsedTime,
// rotation, velocities, controlRotations, and the skin fields, copied from the player's newest saved ghost), so the
// ghost code reads them like any replay. Kept in PluginManager\runs, the newest `Keep` across every track.
//
// A run's ball is the game's player ball in the player's skin (ghosts::PlayerBallFor), placed where the run was at
// any time: the race's own clock while racing (Elapsed), the replay's while watching one. Game thread only.
#pragma once
#include <string>

namespace runs {

void Frame();                           // after race::Frame

void SetKeep(int count);                // how many runs are kept (oldest go first); 10 until set
int Count();                            // runs kept, newest first
std::string Id(int i);                  // a run's own name, the same while it's kept (its place moves as runs come and go)
std::string TrackKey(int i);            // race::Track.key of the track it was on
std::string TrackName(int i);
double Time(int i);                     // how long it lasted (the finish time for a finished run), seconds
bool Finished(int i);
double Age(int i);                      // seconds since it ended
// Pinned runs are kept whatever comes after them and don't count toward Keep (unpinning one lets the oldest go again).
bool Pinned(int i);
void SetPinned(int i, bool pinned);
double Elapsed();                       // the run going on now: seconds since its start; -1 without one

int Ball(int owner, int i);             // a Draw id, or 0
bool Place(int owner, int id, int i, double t);

}  // namespace runs
