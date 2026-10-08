// Leaderboard ghosts of a track, loaded for viewing: the top entries of its Steam leaderboard and the player's own,
// each with its replay (the path the ball took), plus the track's checkpoints, so a plugin can draw and replay them.
//
// The leaderboard is the one the game uses for the track on screen: GetActiveLevelLeaderboardRecord on the game
// instance gives its Steam handle (BallestLeaderboardRecord.LeaderboardHandle, +0x0; ResolvedSteamLeaderboardName,
// +0x40), which works for the game's tracks and workshop tracks alike. A name can be given instead (the game's own
// tracks use their level name, e.g. Map_LethTrial_01). Entries and replays come from Steam (steam.hpp) and replays
// are read by ghostdata. Game thread only.
#pragma once
#include <string>
#include <vector>

#include "ghostdata.hpp"

namespace ghosts {

void Frame();                           // a new map: everything loaded for the last one is unloaded

struct Ghost {
    ghostdata::Replay replay;
    int rank = 0;
    bool own = false;                   // the player's own entry
    uint64_t steamId = 0;
    // Its checkpoint order (Order), worked out by the parse workers against the checkpoints known when it arrived;
    // orderOf: how many checkpoints that was (-1: not worked out yet).
    std::vector<int> order;
    int orderOf = -1;
};

// Starts loading the top `count` entries (0: every entry), plus the player's own. Loading the same leaderboard again
// keeps the replays already here that are still wanted and downloads only the rest; another leaderboard drops them.
// `leaderboard` "" for the track on screen. False if it can't start (no Steam, or no track and no name).
bool Load(const std::string& leaderboard, int count);

// Runs from a local file instead of a leaderboard (text, made by a plugin's own tools), replacing the runs here:
//   RUN1 <tab> ox <tab> oy <tab> oz <tab> unit
//   name <tab> time <tab> skin|- <tab> t0 <tab> dt <tab> base64 of int16 (x, y, z) per sample
// position = origin + q * unit (game axes), sample k at t0 + k * dt seconds. Velocity and view come from the path.
// Returns how many runs were read (-1: the file can't be read or isn't this format).
int LoadLocal(const std::wstring& path);
// The same file read and parsed on a worker thread ahead of a LoadLocal of it; whether that is done.
void PrefetchLocal(const std::wstring& path);
bool PrefetchReady(const std::wstring& path);
// Crowd trails: points per trail at most and the build time a frame (defaults 160 and 3 ms); whether any are queued.
void CrowdTrailDetail(int points, double budgetMs);
bool CrowdTrailsPending(int owner);    // the calling plugin's crowds
// Trails glowing (opaque only) at a brightness, and chunked see-through trails fading with age (seconds) to fadeMin.
bool CrowdTrailStyle(int owner, int id, bool glow, float brightness, double fadeSeconds, float fadeMin);
// Skinned crowd balls' size (1: the game's ball).
bool CrowdBallScale(int owner, int id, double scale);
// A colour group's balls as tinted glass of its colour at an opacity (below 1; they then keep their colour when the crowd
// wears skins), or glowing again (1): for dimming some balls.
bool CrowdGroupGlass(int owner, int id, int group, float opacity);

// Where a ghost's player's camera was at time t, as the game's own camera would have placed it: the ball's camera
// rig (BallAsyncCameraTargetComponent's spring arm and camera, read from the game: arm length, target and socket
// offsets, field of view) turned by the replay's control rotation. Without the arm's collision and lag. out: x, y,
// z, pitch, yaw, field of view. False without a rig or a replay with control rotations.
bool View(size_t ghost, double t, double out[6]);

// The player's own ball for a ghost, as the game shows other players' balls (BP_NonPlayerRollingBall_C, dressed with
// CreateNonPlayerRollingBall from the replay's skin, special skin, accessory and basic-ball settings; its name tag
// shows the player's name). A Draw id (moved, shown and removed like a shape), or 0.
// realSkin: the player's own skin (skinMaterial, the accessory's own material) at full opacity, instead of the game's
// rival-ghost look (ghostSkinMaterial, faded by distance to your ball)
int PlayerBall(int owner, size_t ghost, bool realSkin = false);
// Puts it where the ghost was at time t, turning as the ball turned (UpdateTargetTransform, as multiplayer does).
bool PlacePlayerBall(int owner, int id, size_t ghost, double t);
// The same for any replay (the player's own recent runs, runs.hpp).
int PlayerBallFor(int owner, const ghostdata::Replay& replay, bool realSkin = false);
bool PlaceBallFor(int owner, int id, const ghostdata::Replay& replay, double t);
// Its name tag shown or not (the tag's widget made see-through; the ball's own blueprint shows and hides the
// component by the game's ghost settings every frame, so that is left alone).
bool ShowPlayerName(int owner, int id, bool shown);

// A crowd: many ghosts' balls drawn as a few instanced meshes (one per colour group), all placed natively in one call
// a frame. For thousands of balls, where a shape or a player ball each would be too many objects. A Draw id.
int CrowdCreate(int owner, double radius, const std::vector<float>& palette);   // palette: r, g, b per group
// Which ghosts are in it and their colour group (parallel arrays); rebuilds its instances.
bool CrowdMembers(int owner, int id, const std::vector<int>& ghosts, const std::vector<int>& groups);
// Its members in their own skins: a ball the game's size in each skin (one instanced mesh per skin); they don't roll. Members whose skin doesn't load stay in their colour. Call after CrowdMembers.
bool CrowdSkins(int owner, int id);
// A faint trail for each member shown (CrowdTimes), in its colour (opacity as Draw::Tube's), built a few a frame so
// there's no hitch. Calling it again builds them again (for another set shown); trails share meshes, so they can't be
// hidden one by one. chunkSeconds > 0: built in pieces of that many seconds, for CrowdTrailsUpTo.
bool CrowdTrails(int owner, int id, double radius, float opacity, double chunkSeconds = 0);
// With chunkSeconds, only the pieces of the trails before `t` (each run's own clock) are shown: the trails so far.
bool CrowdTrailsUpTo(int owner, int id, double t);
// Shows or hides the crowd's trails (those still to be built follow).
bool CrowdShowTrails(int owner, int id, bool shown);
// Per ghost (indexed by ghost): its time offset, and whether it's shown (hidden ones are drawn at size 0).
bool CrowdTimes(int owner, int id, const std::vector<double>& offsets, const std::vector<bool>& shown);
// Every member where its ghost is at time t plus its offset.
bool CrowdPlace(int owner, int id, double t);
std::string State();                    // "idle", "loading ...", "ready", or "error: ..."
std::string Leaderboard();              // the Steam leaderboard's name, once known
int Entries();                          // how many entries the leaderboard has in all (0 until known)
int WithoutReplay();                    // entries asked for that have no replay file (so none to load)
const std::vector<Ghost>& All();        // by rank; grows while replays arrive

struct Checkpoint {
    int number = 0;                     // the game's GoalNumber
    ghostdata::Point at;
};
// The checkpoints of the track on screen, by number.
std::vector<Checkpoint> Checkpoints();
// The checkpoint numbers a ghost took, in order (see ghostdata::CheckpointOrder); 0 for a split no checkpoint matched.
std::vector<int> Order(size_t ghost);

}  // namespace ghosts
