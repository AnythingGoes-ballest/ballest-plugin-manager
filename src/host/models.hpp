// Models: small 3D objects made of simple shapes, described in text by a plugin and built at runtime as the engine's
// dynamic meshes (Geometry Script, which the game ships; custom asset files cannot be loaded, measured), then attached
// to a ball or a hat slot. Parts of a group can spin. Used by cosmetics for balls with depth or moving parts, and hats.
//
// The text, one statement a line ("#" at the start of a line, or "# ", starts a comment; lengths in cm, the ball's
// radius is 50; angles in degrees):
//   material <name> plastic|metal|glow #rrggbb [rough=0.5] [bright=5]
//   material <name> glass [#rrggbb] [opacity=0.2]   (see-through: clear M_GlassV2, or tinted M_Glass)
//   material <name> game <material asset path>       (one of the game's own materials, as it is)
//   set <material> "<parameter>" <value>             a parameter of a material's dynamic instance: a number, a colour
//       (#rrggbb, or r,g,b[,a] in linear values; "*k" after either brightens it, for glowing colours), a picture file of
//       the plugin's (.png, .jpg) or a texture asset of the game's (a path starting with "/")
//   animate <material> "<parameter>" <from> <to> [period=<seconds>] [wave=sine|triangle|saw|square] [phase=<degrees>]
//       [run=<more a second per m/s of the ball>]: a number or colour moving between two values every frame
//   race <material> "<parameter>" <factor>           away from the Customize page (a race, the editor, a replay) that
//       parameter, as set or animated, is that many times as much (a colour's brightness): daylight needs more glow
//   group <name> [spin=x|y|z] [speed=<degrees a second>] [travel|facing] [on=<group>] [pivot=x,y,z]
//         [swing=x|y|z angle=<degrees> [phase=<degrees>]] [bob=<cm> [phase=<degrees>]] [lean=<degrees>]
//       parts after it belong to it; "travel" keeps the group upright and turned to where the ball is going instead
//       of rolling with the ball; "facing" turns the group's +x to the camera every frame (a disc of glow round the
//       ball, which the ball hides in front of it: a halo); "on" builds it on an earlier group, moving with it (a
//       limb on a body); "pivot" is the point it turns about (a shoulder); "swing" rocks it to and fro about an axis
//       through the pivot, and "bob" lifts it and lets it down twice a swing (a step), both keeping time with the
//       model's tempo; "lean" tips it forward about its pivot as the ball speeds up, that many degrees from the
//       tempo's "full" speed
//   tempo [rate=<swings a second at rest>] [run=<more a second per m/s of the ball>] [max=<swings a second>]
//         [calm=<share of the swing at rest>] [full=<m/s at which swings are full>]
//   <shape> <material> <size...> [at=x,y,z] [rot=pitch,yaw,roll] [scale=x,y,z]
//     sphere r=               box size=x,y,z            cylinder r= h=        cone r= top= h=
//     capsule r= len=         disc r= [hole=]           ring r= thick= [degrees=360]
//     saw r= teeth= depth= thick=       cup r= top= h= wall=      bowl r= wall=
//     spiral r= inner= turns= thick=
//   A bowl is the lower half of a sphere's shell, centred on "at" (open at the top); a spiral is a tube coiled flat
//   around z from radius r in to radius inner.
//   mesh <file> [size=<cm>] [at=x,y,z] [rot=pitch,yaw,roll] [scale=x,y,z] [material=<name>] [node=<name>]
//        [anim=<name>] [rate=<times its speed at rest>] [run=<more a second per m/s of the ball>] [idle=<name>]
//        [frames=<a clip>]
//     a model file from Blender or another 3D tool: glTF 2.0 (.glb, or .gltf) or OBJ (.obj with its .mtl), next to
//     the model's text file. It stands on "at" (the bottom of its bounds, centred), its longest side "size" cm (as
//     the file has it without). Its colours, glow, see-through parts and textures come from the file, or all of it
//     is "material". "anim" plays one of its animations (a name, or part of one), faster as the ball speeds up;
//     "idle" plays instead while the ball is still. Animations are baked into "frames" poses (24 by default).
//     "node" takes only that node of the file (and the nodes under it): a character's parts as one file. A file is
//     read once for the whole model, its materials added once.
//   move <name> [when=idle|run|air|jump|land] [min=<m/s>] [max=<m/s>] [weight=1] [length=<s>] [blend=0.25]
//        [rate=1] [run=0]
//   key <seconds> <group> [rot=pitch,yaw,roll] [at=x,y,z] [swing=<0..1>]
//   moves [gap=<s>[,<s>]]
//   group <name> ... drop=jump|land [push=x,y,z] [life=<s>] [max=<n>] [hit=<cm/s>] [carry=0.5]
//     a group that isn't shown on the ball: at each jump or landing a copy of it is let go into the world where it
//     is (thrown by "push", cm/s in its own axes), falls to the ground below, rolls to a stop, lies there "life"
//     seconds and shrinks away; "max" (1 to 8) at once, the oldest reused; a landing drops one only when the ball
//     came down at least "hit" cm/s (400: about an 80 cm fall); it leaves with "carry" of the ball's speed along the
//     ground (0: it stays where it was let go). Only to look at: nothing touches it.
//     moves: short keyframed animations of the groups (a hop, a wave, a spin), played one after another, picked at
//     random from those for what the ball is doing ("when": still, rolling, in the air; or at once on a jump or a
//     landing), and eased in and out over "blend" seconds. Each "key" (of the last "move") turns a group by "rot"
//     about its pivot and moves it by "at", on top of its swing and bob, which "swing" scales (0: the move holds the
//     limb on its own). Keys of a group are joined smoothly. "moves gap" is the pause between two moves (random in
//     the range). "rate" and "run" are the move's speed: "rate" times as fast, "run" more for every m/s of the ball.
//   Spheres, boxes, discs, rings and saws are centred on "at"; cylinders, cones, capsules and cups stand on it (along
//   +z). Rings and saws lie flat (around z).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine.hpp"

namespace models {

enum class Finish { Plastic, Metal, Glow, Glass, Image, Game };
// A parameter set on a material ("set"): a number, a colour, or a texture (a picture file, or a texture asset).
struct Setting {
    enum class Kind { Scalar, Colour, Texture };
    std::string parameter;
    Kind kind = Kind::Scalar;
    float value[4] = {0, 0, 0, 1};
    std::wstring file, asset;           // Texture: a picture file, or else an asset path
};
// A parameter that moves ("animate"): from `from` to `to` and back (sine, triangle, square) or from `from` to `to` and
// over again (saw), a cycle every `period` seconds, `phase` degrees into it; `run` makes the cycle that much faster
// for every m/s of the ball's speed.
struct Motion {
    enum class Wave { Sine, Triangle, Saw, Square };
    std::string parameter;
    bool colour = false;
    Wave wave = Wave::Sine;
    float from[4] = {0, 0, 0, 1}, to[4] = {1, 1, 1, 1};
    double period = 4, phase = 0, run = 0;
};
struct Material {
    std::string name;
    Finish finish = Finish::Plastic;
    std::wstring image;                 // Image: the texture's file
    float r = 1, g = 1, b = 1;          // linear
    float rough = 0.5f, bright = 5;
    float opacity = 0.2f;               // glass
    bool tinted = false;                // glass: given a colour
    // Refracting glass (a glTF material with transmission, or "ior=" in a model file): bends what is behind it by its
    // index of refraction and reflects its surroundings, as Blender's glass does.
    bool refracts = false;
    float ior = 1.5f;                   // index of refraction: 1 is air (no bending), glass about 1.45-1.5, diamond 2.4
    float reflect = 1;                  // how strongly it reflects, 0 to 1
    std::wstring asset;                 // Game: the material asset
    std::vector<Setting> settings;      // "set", after the finish's own parameters
    std::vector<Motion> motions;        // "animate"
    std::vector<std::pair<std::string, float>> race;    // "race": parameter, factor away from the Customize page
    float RaceFactor(const std::string& parameter) const {
        for (const auto& [param, k] : race)
            if (param == parameter) return k;
        return 1;
    }
};

enum class Shape { Sphere, Box, Cylinder, Cone, Capsule, Disc, Ring, Saw, Cup, Bowl, Spiral, Mesh };

// A model file's triangles, placed and baked: its rest pose, and each frame of its animations. Positions are where
// the part puts them (in the group's space, before its pivot), in cm.
struct Frame {
    std::vector<std::vector<float>> positions, normals;     // per item, 3 per vertex
};
struct MeshData {
    std::wstring file;
    std::vector<int> materials;                             // per item: the model's material
    std::vector<std::vector<float>> uv;                     // per item, 2 per vertex
    std::vector<std::vector<uint32_t>> indices;             // per item, 3 per triangle
    Frame rest;
    std::vector<Frame> clip, idle;                          // "anim" and "idle", evenly over each
    double clipLength = 0, idleLength = 0, rate = 1, run = 0;
    size_t triangles = 0;
};

struct Part {
    Shape shape;
    int material = 0;
    std::shared_ptr<const MeshData> mesh;       // Mesh
    double r = 0, h = 0, top = 0, thick = 0, depth = 0, hole = 0, degrees = 360, inner = 0, turns = 0, size[3] = {0, 0, 0};
    int teeth = 0;
    double at[3] = {0, 0, 0}, rot[3] = {0, 0, 0}, scale[3] = {1, 1, 1};
};

struct Group {
    std::string name;
    int spinAxis = -1;                  // 0 x, 1 y, 2 z, or -1
    double speed = 0;
    bool travel = false;
    bool facing = false;                // "facing": its +x turned to the camera every frame (a glow behind the ball)
    int parent = -1;                    // "on": the group it is built on, or -1 for the ball
    double pivot[3] = {0, 0, 0};
    int swingAxis = -1;                 // 0 x, 1 y, 2 z, or -1
    double swingAngle = 0, phase = 0, bob = 0;
    double lean = 0;                    // degrees forward at full speed (the tempo's "full")
    bool keyed = false;                 // a move has keys for it
    // "drop": not shown on the ball; at that moment (a When: jump or land) a copy of it is let go into the world where
    // it is, thrown by "push" (cm/s, its own axes), and lies there "life" seconds before shrinking away; at most "max".
    int drop = -1;
    double push[3] = {-200, 0, 150}, life = 4, hit = 400;   // hit: a landing at least this fast (cm/s) drops one
    double carry = 0.5;                 // the share of the ball's speed along the ground it leaves with
    int max = 4;
    bool Moves() const { return parent >= 0 || swingAxis >= 0 || bob != 0 || lean != 0 || keyed || drop >= 0 || pivot[0] != 0 || pivot[1] != 0 || pivot[2] != 0; }
    std::vector<Part> parts;
};

struct Tempo {
    double rate = 1, run = 0, max = 1e9, calm = 1, full = 1;
};

// A move: keyframes for some of the groups, played now and then (see "move" above).
enum class When { Idle, Run, Air, Jump, Land };
struct Key {
    double time = 0;
    int group = 0;
    double rot[3] = {0, 0, 0};          // pitch, yaw, roll
    double at[3] = {0, 0, 0};
    double swing = 1;                   // how much of the group's own swing and bob is kept
};
struct Move {
    std::string name;
    When when = When::Run;
    double min = 0, max = 1e9;          // the ball's speed, m/s
    double weight = 1, length = 0, blend = 0.25, rate = 1, run = 0;
    std::vector<Key> keys;              // by group, then time
};

struct Model {
    std::vector<Material> materials;
    std::vector<Group> groups;
    std::vector<Move> moves;
    double gap[2] = {0.3, 1.2};         // seconds between moves
    Tempo tempo;
    bool hideBall = false;              // "ball hidden": a clear ball's own glass isn't drawn (the model is the ball)
};

// False with the line and reason in `error` if the text is not a model. Mesh files named in it are found in `folder`
// (a path from the text is taken as it is when absolute).
bool Parse(const std::string& text, Model* model, std::string* error, const std::wstring& folder = L"");

// A model built on a component: one actor per group.
struct Built {
    std::vector<eng::Weak> actors;      // by group
    // An animated mesh part: one actor per frame (the clip's, then the idle's), attached to its group's; the frame
    // shown is the only one visible.
    struct Flip {
        size_t group = 0;
        std::shared_ptr<const MeshData> mesh;
        std::vector<eng::Weak> frames;
        int shown = -1;
        double time = 0;                // in the clip being played
        bool idling = false;
    };
    std::vector<Flip> flips;
    eng::Weak parent;
    double travelYaw = 90;             // until the ball moves: side-on to the Customize camera (measured)
    bool facing = false;                // travelYaw has been set from the camera or the ball's travel
    double last = -1, beat = 0, pace = 0;   // the tempo's clock: last time, swings so far, smoothed ball speed
    std::vector<std::vector<double>> clocks;    // per material, per animation: cycles so far ("run" makes them uneven)
    bool race = true;                   // not on the Customize page's ball: "race" factors apply
    // Moves: the one playing and the one fading out under it (-1: none), how far into each, and the wait for the next.
    struct Playing {
        int move = -1;
        double time = 0, fade = 1;      // fade: 1, or going down to 0 once it has been cut short
    };
    Playing now, before;
    double wait = 0;
    int lastMove = -1;
    int state = 0;                      // When the ball is in (Idle, Run or Air)
    double vz = 0, fall = 0;            // the ball's last vertical speed (cm/s), and how long it has been falling freely
    bool vzKnown = false;
    double landing = 0;                 // how fast the ball came down at the last landing (cm/s)
    // A landing is the ball's fall stopped (a kick up in its vertical speed) with the game's own ground bounce (race.hpp)
    // within kPair seconds of it: a restart stops the fall too, but with no bounce.
    int bounceSeen = -1;                // the last bounce read
    double kickAt = -1, kickFall = 0, groundAt = -1;
    double ballAt[3] = {0, 0, 0};       // where the ball was last frame (a jump in it is a restart, not a landing)
    bool ballKnown = false;
    // Dropped copies of a "drop" group, each an actor of its own in the world (made when the model is built, reused).
    struct Dropped {
        eng::Weak actor;
        bool live = false;
        double age = 0, pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0}, rot[3] = {0, 0, 0}, spin = 0, ground = 0, retrace = 0;
    };
    struct Drops {
        size_t group = 0;
        double radius = 10;             // from its pivot down to its lowest point: it lies and rolls on that
        std::vector<Dropped> copies;
        size_t next = 0;
    };
    std::vector<Drops> drops;
    uint32_t seed = 0;
    bool Alive() const;
};
Built Build(const Model& model, eng::Obj parent);
void Animate(const Model& model, Built& built, eng::Obj ballActor, double seconds);
void Destroy(Built& built);
void TraceMoves(bool on);               // tests only: log each move started, and the ball's vertical speed (10 a second)

// Shapes in the world, for drawing (ghost trails and balls): a mesh actor on its own (not attached, no collision) in
// one colour, glowing or plastic. Null when it could not be made.
struct Colour {
    float r = 1, g = 1, b = 1;          // linear
    bool glow = false;
    float bright = 5;
    float opacity = 1;                  // below 1: see-through, as tinted glass (M_Glass) of the colour
};
eng::Obj SpawnTube(const std::vector<std::array<double, 3>>& path, double radius, const Colour& colour);   // along the points
eng::Obj SpawnBall(double radius, const Colour& colour);
eng::Obj SpawnHolder();                 // an empty mesh actor in the world, for a model built on its root component
std::vector<eng::Obj> Actors(const Built& built);       // every actor of a built model (groups and frames)
// An empty mesh actor in this colour (tinted glass when see-through), and a tube added to one: many tubes in one mesh
// are one object to draw. sides: of the tube's cross-section.
eng::Obj SpawnMesh(const Colour& colour);
bool AppendTube(eng::Obj actor, const std::vector<std::array<double, 3>>& path, double radius, int sides);
// A shape's dynamic material (SpawnTube/SpawnBall), and that material given another colour and brightness (a glowing
// shape) or opacity (a see-through one), in place.
eng::Obj GlowMaterial(eng::Obj actor);
eng::Obj NewGlowMaterial(float r, float g, float b, float bright);     // a new dynamic glow material of that colour
eng::Obj NewGlassMaterial(float r, float g, float b, float opacity);   // a new tinted see-through one
bool SetGlow(eng::Obj material, float r, float g, float b, float bright);
bool SetOpacity(eng::Obj material, float opacity);
// Tests only: sets a parameter (one value: scalar; 3-4: a colour) on every refracting glass material made so far, to
// tune how glass draws. The number of materials changed.
int TuneRefractingGlass(const std::string& parameter, const std::vector<float>& values);
int TuneRefractingGlassTexture(const std::string& parameter, const std::string& asset);   // tests only: a texture
void SetRefractingGlassParent(const std::string& path);
void SetGlassProbe(bool on);
void TuneGlassProbe(double interval, int size);
void TuneGlassProbeLook(bool lean, bool lead);     // tests only: lean capture, capture ahead of the ball   // tests only: seconds between captures (0 every frame), face pixels    // tests only: allow live reflections (on by default) or not, to measure their cost
void ProbeFrame();              // every frame   // tests only: refracting glass made from now on uses it ("" back)

}  // namespace models
