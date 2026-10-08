// Things plugins draw in the world, and a camera they can look through. Used by the ghost viewer: each player's path
// as a tube, their ball as a sphere moving along it, the view flown around the track.
//
// Shapes are Geometry Script meshes (models::SpawnTube / SpawnBall) with no collision and no shadow, never attached to
// anything; they belong to the plugin that made them and go when it stops or the map changes. The camera is a
// CameraActor the player controller is set to view through (SetViewTargetWithBlend); releasing it goes back to
// the pawn. Game thread only.
#pragma once
#include <array>
#include <string>
#include <vector>

#include "engine.hpp"

namespace draw {

void Frame();                           // forgets what went with the map
void RemoveOwner(int owner);            // a plugin stopped: its shapes go, and its camera is released

// A shape; 0 if it could not be made. Colours are linear, 0..1.
// opacity below 1: see-through (the game's tinted glass), glow then ignored.
int Tube(int owner, const std::vector<std::array<double, 3>>& path, double radius, float r, float g, float b, bool glow, float opacity = 1);
// Separate straight tubes in one mesh (one object to draw): points in pairs, start then end of each.
int Segments(int owner, const std::vector<std::array<double, 3>>& pairs, double radius, float r, float g, float b, bool glow, float opacity = 1);
// A tube (Tube) given a new path in place: the same actor and material, its mesh emptied (UDynamicMesh.Reset) and swept
// again, for a line that changes every frame (a trail's growing end) without spawning anything. A path too short to
// sweep leaves it empty. False (nothing changed or the mesh left empty) if it isn't a tube of the owner's or the mesh
// didn't empty.
bool Retube(int owner, int id, const std::vector<std::array<double, 3>>& path, double radius);
int Ball(int owner, double radius, float r, float g, float b, bool glow);
bool Move(int owner, int id, double x, double y, double z);
// A glowing shape's colour and brightness (0 dark), changed in place.
bool Glow(int owner, int id, float r, float g, float b, float bright);
// A see-through shape's opacity (0..1), changed in place.
bool Fade(int owner, int id, float opacity);
// A model (models.hpp's format, with any 3D model files it names) built at the world's origin; moved, turned, scaled,
// shown and removed like a shape. Its spinning groups and animations play by themselves. 0 and `error` if it can't be.
int Model(int owner, const std::string& text, std::string* error);
bool Turn(int owner, int id, double pitch, double yaw, double roll);
bool Scale(int owner, int id, double scale);
// One of the game's Niagara particle effects played once where it is put (it removes itself), `scale` times its size,
// turned so its up points along (nx, ny, nz).
bool Effect(const std::string& system, double x, double y, double z, double scale, double nx = 0, double ny = 0, double nz = 1);
// One of the game's sounds, played once (not placed: as the game's own hit sounds, PlaySound2D).
std::string LastSoundState();        // for measuring: whether the last custom .wav sound is playing
bool Sound(const std::string& sound, double volume, double pitch);
// The game's camera shake (Shake_BallestCam), `scale` times as strong.
bool Shake(double scale);
int Adopt(int owner, eng::Obj actor);   // an actor made elsewhere, kept like a shape (moved, shown, removed with it)
eng::Obj ActorOf(int owner, int id);
bool Show(int owner, int id, bool shown);
void Remove(int owner, int id);
void Clear(int owner);

// Where a point in the world is on screen, in widget units (those of window offsets and the mouse position).
// False when it is behind the camera.
bool Project(double x, double y, double z, double* sx, double* sy);

// The camera: taking it looks through a camera of the plugin's, placed with SetCamera; releasing goes back to the
// game's own view. One plugin at a time.
bool TakeCamera(int owner);
bool SetCamera(int owner, double x, double y, double z, double pitch, double yaw, double fov);
void ReleaseCamera(int owner);
bool HasCamera(int owner);

}  // namespace draw
