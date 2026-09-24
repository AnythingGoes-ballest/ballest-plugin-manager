// Models: small 3D objects made of simple shapes, described in text by a plugin and built at runtime as the engine's
// dynamic meshes (Geometry Script, which the game ships; custom asset files cannot be loaded, measured), then attached
// to a ball or a hat slot. Parts of a group can spin. Used by cosmetics for balls with depth or moving parts, and hats.
//
// The text, one statement a line ("#" at the start of a line, or "# ", starts a comment; lengths in cm, the ball's
// radius is 50; angles in degrees):
//   material <name> plastic|metal|glow #rrggbb [rough=0.5] [bright=5]
//   group <name> [spin=x|y|z] [speed=<degrees a second>] [travel]
//       parts after it belong to it; "travel" keeps the group upright and turned to where the ball is going instead
//       of rolling with the ball
//   <shape> <material> <size...> [at=x,y,z] [rot=pitch,yaw,roll] [scale=x,y,z]
//     sphere r=               box size=x,y,z            cylinder r= h=        cone r= top= h=
//     capsule r= len=         disc r= [hole=]           ring r= thick= [degrees=360]
//     saw r= teeth= depth= thick=       cup r= top= h= wall=
//   Spheres, boxes, discs, rings and saws are centred on "at"; cylinders, cones, capsules and cups stand on it (along
//   +z). Rings and saws lie flat (around z).
#pragma once
#include <string>
#include <vector>

#include "engine.hpp"

namespace models {

enum class Finish { Plastic, Metal, Glow };
struct Material {
    std::string name;
    Finish finish = Finish::Plastic;
    float r = 1, g = 1, b = 1;          // linear
    float rough = 0.5f, bright = 5;
};

enum class Shape { Sphere, Box, Cylinder, Cone, Capsule, Disc, Ring, Saw, Cup };
struct Part {
    Shape shape;
    int material = 0;
    double r = 0, h = 0, top = 0, thick = 0, depth = 0, hole = 0, degrees = 360, size[3] = {0, 0, 0};
    int teeth = 0;
    double at[3] = {0, 0, 0}, rot[3] = {0, 0, 0}, scale[3] = {1, 1, 1};
};

struct Group {
    std::string name;
    int spinAxis = -1;                  // 0 x, 1 y, 2 z, or -1
    double speed = 0;
    bool travel = false;
    std::vector<Part> parts;
};

struct Model {
    std::vector<Material> materials;
    std::vector<Group> groups;
};

// False with the line and reason in `error` if the text is not a model.
bool Parse(const std::string& text, Model* model, std::string* error);

// A model built on a component: one actor per group.
struct Built {
    std::vector<eng::Weak> actors;      // by group
    eng::Weak parent;
    double travelYaw = 90;             // until the ball moves: side-on to the Customize camera (measured)
    bool Alive() const;
};
Built Build(const Model& model, eng::Obj parent);
void Animate(const Model& model, Built& built, eng::Obj ballActor, double seconds);
void Destroy(Built& built);

}  // namespace models
