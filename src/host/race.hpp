// Races: whether one is running and how often it was restarted from the beginning. Measured on the game:
//   * the player controller (BP_MyPlayerController_C) has bRaceActive: on while racing, including through
//     checkpoint respawns; off in menus, after the finish, and for the moment of a restart
//   * the ball (the controller's pawn, BP_RollingBall_C) has RestartCounterThisSession, which the game raises by
//     one on every restart from the beginning of the track (Backspace, or R before the first checkpoint), and not
//     for checkpoint respawns, falls or the track's first start. Each map has a new ball whose counter starts
//     again, so counting follows the ball: a different ball is a new baseline, not a restart.
// Game thread only.
#pragma once

namespace race {

void Frame();                   // after game::Frame

bool OnTrack();                 // a race controller exists (a track is loaded, not the main menu)
bool Active();                  // a race is running
int Restarts();                 // restarts from the beginning since the host started

}  // namespace race
