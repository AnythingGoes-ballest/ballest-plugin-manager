#include "models.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "cosmetics.hpp"
#include "game.hpp"
#include "log.hpp"

namespace models {
namespace {

using eng::Obj;
using eng::Params;

constexpr double kPi = 3.14159265358979323846;
// Measured materials that render on dynamic meshes: the game's plain ball colour (BaseColor, Metallic, Roughness) and
// its emissive light (Light_Color, Light_Emissive_Intensity).
const wchar_t* kColourMaterial = L"/Game/Art/Materials/Instances/Ball/MI_BallRed.MI_BallRed";
const wchar_t* kGlowMaterial = L"/Game/Art/Materials/Environment/Materials/Instances/MI_Env_Emissive_Yellow.MI_Env_Emissive_Yellow";
// The snow globe skin's glass (BP_SnowGlobeSkin's Sphere wears it; read from the package: translucent, with scalar
// parameters RimStrength, RimExp, HighlightStrength, HighlightExp, GhostOpacity and Vel).
const wchar_t* kGlassMaterial = L"/Game/Art/Materials/Masters/M_SnowGlobeTop.M_SnowGlobeTop";

struct Vec3 {
    double x, y, z;
};
struct Rot {
    double pitch, yaw, roll;
};
struct Transform {
    uint8_t bytes[96];
};
struct Options {                        // FGeometryScriptPrimitiveOptions (measured)
    uint8_t polygroupMode = 0, flip = 0, uvMode = 0, pad = 0;
    int32_t materialId = 0;
};
struct RevolveOptions {                 // FGeometryScriptRevolveOptions (measured)
    float degrees = 360, offset = 0;
    uint8_t reverse = 0, hardNormals = 0, pad[2] = {};
    float hardAngle = 30;
    uint8_t profileAtMidpoint = 0, fillEndcaps = 1, pad2[2] = {};
};
struct ArrayHeader {
    void* data;
    int32_t num, max;
};

// ------------------------------------------------------------------------------------------------ parsing
float Linear(int c) { return static_cast<float>(std::pow(c / 255.0, 2.2)); }

bool Numbers(const std::string& s, double* out, int n) {
    std::stringstream ss(s);
    std::string item;
    int i = 0;
    while (std::getline(ss, item, ',') && i < n) out[i++] = std::atof(item.c_str());
    return i == n;
}

}  // namespace

bool Parse(const std::string& text, Model* model, std::string* error) {
    *model = Model{};
    Group main;
    main.name = "main";
    model->groups.push_back(main);
    std::stringstream lines(text);
    std::string line;
    int number = 0;
    auto fail = [&](const std::string& why) {
        *error = "line " + std::to_string(number) + ": " + why;
        return false;
    };
    while (std::getline(lines, line)) {
        ++number;
        // A comment: "#" starting a line, or "# " anywhere (colours are "#rrggbb", with no space).
        const size_t first = line.find_first_not_of(" 	");
        if (first != std::string::npos && line[first] == '#') continue;
        if (const size_t comment = line.find("# "); comment != std::string::npos) line = line.substr(0, comment);
        std::stringstream words(line);
        std::vector<std::string> w;
        for (std::string word; words >> word;) w.push_back(word);
        if (w.empty()) continue;
        auto value = [&](const char* key, std::string* out) {
            const std::string prefix = std::string(key) + "=";
            for (const auto& word : w)
                if (word.rfind(prefix, 0) == 0) {
                    *out = word.substr(prefix.size());
                    return true;
                }
            return false;
        };
        auto num = [&](const char* key, double fallback) {
            std::string v;
            return value(key, &v) ? std::atof(v.c_str()) : fallback;
        };
        if (w[0] == "material") {
            if (w.size() >= 3 && w[2] == "glass") {
                Material m;
                m.name = w[1];
                m.finish = Finish::Glass;
                m.rim = static_cast<float>(num("rim", 1));
                m.highlight = static_cast<float>(num("highlight", 1));
                model->materials.push_back(m);
                continue;
            }
            if (w.size() < 4 || w[3].size() != 7 || w[3][0] != '#') return fail("material <name> plastic|metal|glow #rrggbb, or material <name> glass");
            Material m;
            m.name = w[1];
            if (w[2] == "metal") m.finish = Finish::Metal;
            else if (w[2] == "glow") m.finish = Finish::Glow;
            else if (w[2] != "plastic") return fail("finish must be plastic, metal, glow or glass");
            const long rgb = std::strtol(w[3].c_str() + 1, nullptr, 16);
            m.r = Linear((rgb >> 16) & 255);
            m.g = Linear((rgb >> 8) & 255);
            m.b = Linear(rgb & 255);
            m.rough = static_cast<float>(num("rough", m.finish == Finish::Metal ? 0.25 : 0.5));
            m.bright = static_cast<float>(num("bright", 5));
            model->materials.push_back(m);
            continue;
        }
        if (w[0] == "tempo") {
            Tempo& t = model->tempo;
            t.rate = num("rate", 1);
            t.run = num("run", 0);
            t.max = num("max", 1e9);
            t.calm = num("calm", 1);
            t.full = num("full", 1);
            if (t.rate < 0 || t.run < 0 || t.max <= 0 || t.calm < 0 || t.calm > 1 || t.full <= 0)
                return fail("tempo: rate and run from 0, max and full above 0, calm from 0 to 1");
            continue;
        }
        if (w[0] == "group") {
            if (w.size() < 2) return fail("group <name>");
            Group g;
            g.name = w[1];
            auto axisOf = [](const std::string& a) { return a == "x" ? 0 : a == "y" ? 1 : a == "z" ? 2 : -1; };
            std::string v;
            if (value("spin", &v)) g.spinAxis = axisOf(v);
            g.speed = num("speed", 0);
            for (const auto& word : w) g.travel |= word == "travel";
            if (model->groups.size() == 1 && model->groups[0].parts.empty()) model->groups.clear();
            if (value("on", &v)) {
                for (size_t i = 0; i < model->groups.size(); ++i)
                    if (model->groups[i].name == v) g.parent = static_cast<int>(i);
                if (g.parent < 0) return fail("on=" + v + ": no group of that name before this one");
                if (g.travel) return fail("travel is for groups on the ball; one on another group turns with it");
            }
            if (value("pivot", &v) && !Numbers(v, g.pivot, 3)) return fail("pivot=x,y,z");
            if (value("swing", &v)) {
                g.swingAxis = axisOf(v);
                if (g.swingAxis < 0) return fail("swing=x|y|z");
                g.swingAngle = num("angle", 0);
            }
            g.bob = num("bob", 0);
            g.phase = num("phase", 0);
            if (g.travel && g.Moves()) return fail("a travel group can't pivot, swing or bob: put those on a group on it");
            model->groups.push_back(g);
            continue;
        }
        static const std::pair<const char*, Shape> shapes[] = {
            {"sphere", Shape::Sphere}, {"box", Shape::Box},   {"cylinder", Shape::Cylinder}, {"cone", Shape::Cone},
            {"capsule", Shape::Capsule}, {"disc", Shape::Disc}, {"ring", Shape::Ring},       {"saw", Shape::Saw},
            {"cup", Shape::Cup}};
        Part p{};
        bool known = false;
        for (const auto& [name, shape] : shapes)
            if (w[0] == name) {
                p.shape = shape;
                known = true;
            }
        if (!known) return fail("unknown statement '" + w[0] + "'");
        if (w.size() < 2) return fail(w[0] + " needs a material");
        p.material = -1;
        for (size_t i = 0; i < model->materials.size(); ++i)
            if (model->materials[i].name == w[1]) p.material = static_cast<int>(i);
        if (p.material < 0) return fail("no material '" + w[1] + "' (define it first)");
        p.r = num("r", 0);
        p.h = num("h", 0);
        p.top = num("top", 0);
        p.thick = num("thick", 0);
        p.depth = num("depth", 0);
        p.hole = num("hole", 0);
        p.degrees = num("degrees", 360);
        p.teeth = static_cast<int>(num("teeth", 0));
        if (p.shape == Shape::Capsule) p.h = num("len", 0);
        if (p.shape == Shape::Cup) p.thick = num("wall", 0);
        std::string v;
        if (value("size", &v) && !Numbers(v, p.size, 3)) return fail("size=x,y,z");
        if (value("at", &v) && !Numbers(v, p.at, 3)) return fail("at=x,y,z");
        if (value("rot", &v) && !Numbers(v, p.rot, 3)) return fail("rot=pitch,yaw,roll");
        if (value("scale", &v) && !Numbers(v, p.scale, 3)) return fail("scale=x,y,z");
        const bool ok = p.shape == Shape::Box        ? p.size[0] > 0 && p.size[1] > 0 && p.size[2] > 0
                        : p.shape == Shape::Ring     ? p.r > 0 && p.thick > 0
                        : p.shape == Shape::Saw      ? p.r > 0 && p.teeth >= 3 && p.thick > 0 && p.depth > 0 && p.depth < p.r
                        : p.shape == Shape::Cup      ? p.r > 0 && p.h > 0 && p.thick > 0 && p.thick < p.r
                        : p.shape == Shape::Cylinder || p.shape == Shape::Capsule ? p.r > 0 && p.h > 0
                        : p.shape == Shape::Cone     ? (p.r > 0 || p.top > 0) && p.h > 0
                                                     : p.r > 0;
        if (!ok) return fail(w[0] + ": sizes missing or out of range");
        model->groups.back().parts.push_back(p);
    }
    for (const auto& g : model->groups)
        if (!g.parts.empty()) return true;
    *error = "no parts";
    return false;
}

namespace {

// ------------------------------------------------------------------------------------------------ building
Obj Lib(const char* name) { return eng::FindCdo(name); }

Transform MakeTransform(const double at[3], const double rot[3], const double scale[3]) {
    Transform t{};
    const Vec3 location{at[0], at[1], at[2]}, size{scale[0], scale[1], scale[2]};
    const Rot rotation{rot[0], rot[1], rot[2]};
    const Params p = eng::Call(Lib("KismetMathLibrary"), "MakeTransform", location, rotation, size);
    if (const uint8_t* r = p.Return()) std::memcpy(t.bytes, r, sizeof t.bytes);
    return t;
}

// An engine-allocated array of 2D points (FVector2D, doubles), for the polygon functions.
ArrayHeader Points(const std::vector<std::pair<double, double>>& points) {
    const int n = static_cast<int>(points.size());
    const std::wstring empty;
    // LeftPad to 8n - 1 characters: 16n bytes with the terminator, room for n points.
    const eng::FString s{empty.c_str(), 1, 1};
    const Params p = eng::Call(Lib("KismetStringLibrary"), "LeftPad", s, static_cast<int32_t>(8 * n - 1));
    ArrayHeader a{nullptr, 0, 0};
    size_t size = 0;
    const uint8_t* r = p.Return(&size);
    if (!r || size != 16) return a;
    std::memcpy(&a.data, r, sizeof a.data);
    if (!a.data) return a;
    auto* d = static_cast<double*>(a.data);
    for (int i = 0; i < n; ++i) {
        d[2 * i] = points[static_cast<size_t>(i)].first;
        d[2 * i + 1] = points[static_cast<size_t>(i)].second;
    }
    a.num = a.max = n;
    return a;
}

bool Append(Obj mesh, const Part& part) {
    Obj lib = Lib("GeometryScriptLibrary_MeshPrimitiveFunctions");
    Options options;
    options.materialId = part.material;
    const Transform transform = MakeTransform(part.at, part.rot, part.scale);
    const char* fn = nullptr;
    switch (part.shape) {
        case Shape::Sphere: fn = "AppendSphereLatLong"; break;
        case Shape::Box: fn = "AppendBox"; break;
        case Shape::Cylinder: fn = "AppendCylinder"; break;
        case Shape::Cone: fn = "AppendCone"; break;
        case Shape::Capsule: fn = "AppendCapsule"; break;
        case Shape::Disc: fn = "AppendDisc"; break;
        case Shape::Ring:
        case Shape::Cup: fn = "AppendRevolvePolygon"; break;
        case Shape::Saw: fn = "AppendSimpleExtrudePolygon"; break;
    }
    Params p(eng::FindFunction(eng::ClassOf(lib), fn));
    p.Set("TargetMesh", mesh);
    p.Set("PrimitiveOptions", options);
    p.Set("Transform", transform);
    const float r = static_cast<float>(part.r), h = static_cast<float>(part.h);
    const uint8_t center = 0, base = 1;
    switch (part.shape) {
        case Shape::Sphere:
            p.Set("Radius", r);
            p.Set("StepsPhi", int32_t{24});
            p.Set("StepsTheta", int32_t{32});
            p.Set("Origin", center);
            break;
        case Shape::Box:
            p.Set("DimensionX", static_cast<float>(part.size[0]));
            p.Set("DimensionY", static_cast<float>(part.size[1]));
            p.Set("DimensionZ", static_cast<float>(part.size[2]));
            p.Set("Origin", center);
            break;
        case Shape::Cylinder:
            p.Set("Radius", r);
            p.Set("Height", h);
            p.Set("RadialSteps", int32_t{32});
            p.Set("HeightSteps", int32_t{1});
            p.Set("bCapped", uint8_t{1});
            p.Set("Origin", base);
            break;
        case Shape::Cone:
            p.Set("BaseRadius", r);
            p.Set("TopRadius", static_cast<float>(part.top));
            p.Set("Height", h);
            p.Set("RadialSteps", int32_t{32});
            p.Set("HeightSteps", int32_t{1});
            p.Set("bCapped", uint8_t{1});
            p.Set("Origin", base);
            break;
        case Shape::Capsule:
            p.Set("Radius", r);
            p.Set("LineLength", h);
            p.Set("HemisphereSteps", int32_t{8});
            p.Set("CircleSteps", int32_t{24});
            p.Set("SegmentSteps", int32_t{1});
            p.Set("Origin", base);
            break;
        case Shape::Disc:
            p.Set("Radius", r);
            p.Set("AngleSteps", int32_t{48});
            p.Set("SpokeSteps", int32_t{1});
            p.Set("StartAngle", 0.0f);
            p.Set("EndAngle", 360.0f);
            p.Set("HoleRadius", static_cast<float>(part.hole));
            break;
        case Shape::Ring: {
            // A circle of the ring's thickness, swept around z at radius r.
            std::vector<std::pair<double, double>> circle;
            for (int i = 0; i < 12; ++i) {
                const double a = 2 * kPi * i / 12;
                circle.push_back({part.thick / 2 * std::cos(a), part.thick / 2 * std::sin(a)});
            }
            const ArrayHeader points = Points(circle);
            RevolveOptions revolve;
            revolve.degrees = static_cast<float>(part.degrees);
            p.Set("PolygonVertices", points);
            p.Set("RevolveOptions", revolve);
            p.Set("Radius", r);
            p.Set("Steps", int32_t{36});
            break;
        }
        case Shape::Cup: {
            // The wall's cross-section (distance from the axis, height), swept all the way round: a bowl with a
            // floor, open at the top.
            const double wall = part.thick, top = part.top > 0 ? part.top : part.r;
            const ArrayHeader points = Points({{0, 0}, {part.r, 0}, {top, part.h}, {top - wall, part.h},
                                               {part.r - wall, wall}, {0, wall}});
            RevolveOptions revolve;
            p.Set("PolygonVertices", points);
            p.Set("RevolveOptions", revolve);
            p.Set("Radius", 0.0f);
            p.Set("Steps", int32_t{40});
            break;
        }
        case Shape::Saw: {
            std::vector<std::pair<double, double>> star;
            for (int i = 0; i < part.teeth; ++i) {
                // Each tooth: a slanted edge up to the tip, then straight back down (a ripsaw's hook).
                const double a0 = 2 * kPi * i / part.teeth, a1 = 2 * kPi * (i + 0.8) / part.teeth;
                star.push_back({(part.r - part.depth) * std::cos(a0), (part.r - part.depth) * std::sin(a0)});
                star.push_back({part.r * std::cos(a1), part.r * std::sin(a1)});
            }
            const ArrayHeader points = Points(star);
            p.Set("PolygonVertices", points);
            p.Set("Height", static_cast<float>(part.thick));
            p.Set("HeightSteps", int32_t{1});
            p.Set("bCapped", uint8_t{1});
            p.Set("Origin", center);
            break;
        }
    }
    return eng::Invoke(lib, p);
}

Obj MakeMaterial(const Material& m, Obj worldContext) {
    const wchar_t* path = m.finish == Finish::Glow ? kGlowMaterial : m.finish == Finish::Glass ? kGlassMaterial : kColourMaterial;
    Obj parent = cosmetics::LoadAsset(path);
    if (!parent) return nullptr;
    const Params made = eng::Call(Lib("KismetMaterialLibrary"), "CreateDynamicMaterialInstance", worldContext, parent,
                                  std::array<uint8_t, 8>{}, uint8_t{0});
    Obj mid = made.ReturnObj();
    if (!mid) return nullptr;
    if (m.finish == Finish::Glass) {
        cosmetics::ScaleGlass(mid, m.rim, m.highlight);
        return mid;
    }
    auto vector = [&](const char* name, float r, float g, float b) {
        Params p(eng::FunctionOn(mid, "SetVectorParameterValue"));
        const std::wstring wide = eng::Widen(name);
        const Params nameCall = eng::Call(Lib("KismetStringLibrary"), "Conv_StringToName",
                                          eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
        const float value[4] = {r, g, b, 1};
        if (const uint8_t* fname = nameCall.Return()) p.Set("ParameterName", fname, 8);
        p.Set("Value", value);
        eng::Invoke(mid, p);
    };
    auto scalar = [&](const char* name, float v) {
        Params p(eng::FunctionOn(mid, "SetScalarParameterValue"));
        const std::wstring wide = eng::Widen(name);
        const Params nameCall = eng::Call(Lib("KismetStringLibrary"), "Conv_StringToName",
                                          eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
        if (const uint8_t* fname = nameCall.Return()) p.Set("ParameterName", fname, 8);
        p.Set("Value", v);
        eng::Invoke(mid, p);
    };
    if (m.finish == Finish::Glow) {
        vector("Light_Color", m.r, m.g, m.b);
        scalar("Light_Emissive_Intensity", m.bright);
    } else {
        vector("BaseColor", m.r, m.g, m.b);
        scalar("Metallic", m.finish == Finish::Metal ? 1.0f : 0.0f);
        scalar("Roughness", m.rough);
    }
    return mid;
}

Obj SpawnMeshActor(Obj worldContext) {
    Obj cls = eng::FindClass("DynamicMeshActor");
    const double zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
    Transform t = MakeTransform(zero, zero, one);
    Obj statics = Lib("GameplayStatics");
    Params begin(eng::FunctionOn(statics, "BeginDeferredActorSpawnFromClass"));
    begin.Set("WorldContextObject", worldContext);
    begin.Set("ActorClass", cls);
    begin.Set("SpawnTransform", t);
    begin.Set("CollisionHandlingOverride", uint8_t{1});     // always spawn
    begin.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, begin);
    Obj actor = begin.ReturnObj();
    if (!actor) return nullptr;
    Params finish(eng::FunctionOn(statics, "FinishSpawningActor"));
    finish.Set("Actor", actor);
    finish.Set("SpawnTransform", t);
    finish.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, finish);
    return actor;
}

}  // namespace

bool Built::Alive() const {
    if (!eng::Get(parent)) return false;
    for (const auto& a : actors)
        if (!eng::Get(a)) return false;
    return !actors.empty();
}

Built Build(const Model& model, Obj parent) {
    Built built;
    built.parent = eng::MakeWeak(parent);
    Obj controller = game::PlayerController();
    if (!parent || !controller) return built;
    std::vector<Obj> materials;
    for (const auto& m : model.materials) materials.push_back(MakeMaterial(m, controller));
    for (const auto& group : model.groups) {
        Obj actor = SpawnMeshActor(controller);
        Obj component = actor ? eng::ReadObj(actor, "DynamicMeshComponent") : nullptr;
        Obj mesh = component ? eng::Call(component, "GetDynamicMesh").ReturnObj() : nullptr;
        if (!mesh) {
            hostlog::Warn("models: could not make a dynamic mesh actor");
            if (actor) eng::Call(actor, "K2_DestroyActor");
            Destroy(built);
            return built;
        }
        for (Part part : group.parts) {                               // built about the group's pivot
            for (int k = 0; k < 3; ++k) part.at[k] -= group.pivot[k];
            Append(mesh, part);
        }
        for (size_t i = 0; i < materials.size(); ++i)
            if (materials[i]) eng::Call(component, "SetMaterial", static_cast<int32_t>(i), materials[i]);
        eng::Call(component, "SetCollisionEnabled", uint8_t{0});     // never touches the ball's physics
        const uint8_t snap = 2;                                       // EAttachmentRule::SnapToTarget
        Obj on = parent;                                              // the ball, or the group it is built on
        if (group.parent >= 0) {
            Obj holder = eng::Get(built.actors[static_cast<size_t>(group.parent)]);
            on = holder ? eng::ReadObj(holder, "DynamicMeshComponent") : nullptr;
        }
        if (on) eng::Call(actor, "K2_AttachToComponent", on, std::array<uint8_t, 8>{}, snap, snap, snap, uint8_t{0});
        if (group.travel) eng::Call(component, "SetAbsolute", uint8_t{0}, uint8_t{1}, uint8_t{0});
        built.actors.push_back(eng::MakeWeak(actor));
    }
    return built;
}

namespace {
double Wrap(double degrees) { return std::remainder(degrees, 360.0); }

// The yaw of the camera the player sees through.
bool CameraYaw(double* yaw) {
    Obj controller = game::PlayerController();
    Obj manager = controller ? eng::ReadObj(controller, "PlayerCameraManager") : nullptr;
    if (!manager) return false;
    *yaw = eng::Call(manager, "GetCameraRotation").ReturnAs<Rot>().yaw;
    return true;
}
}  // namespace

void Animate(const Model& model, Built& built, Obj ballActor, double seconds) {
    const double dt = built.last < 0 ? 0 : std::clamp(seconds - built.last, 0.0, 0.1);
    built.last = seconds;
    double speed = 0;                                   // cm/s
    if (ballActor) {
        const Vec3 v = eng::Call(ballActor, "GetVelocity").ReturnAs<Vec3>();
        speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        // A group that travels faces where the ball is going. Before the ball has gone anywhere it faces the way the
        // camera looks (a racing ball waiting at the start), or looks at the camera (the Customize page's ball,
        // which never travels); turns are eased so a wobble in the ball's path doesn't shake it.
        const bool menu = eng::ClassOf(ballActor) == eng::FindClass("BP_MenuBall_C");
        double target = built.travelYaw, camera = 0;
        bool known = false;
        if (v.x * v.x + v.y * v.y > 50.0 * 50.0) {
            target = std::atan2(v.y, v.x) * 180 / kPi;
            known = true;
        } else if ((menu || !built.facing) && CameraYaw(&camera)) {
            target = menu ? camera + 180 : camera;
            known = true;
        }
        if (known && !built.facing) built.travelYaw = target;
        else if (known) built.travelYaw += Wrap(target - built.travelYaw) * std::min(1.0, dt * 8);
        built.facing |= known;
    }
    // The tempo: swings a second from the ball's speed, and how far limbs swing (calm at rest, full when fast).
    const Tempo& t = model.tempo;
    built.pace += (speed / 100 - built.pace) * std::min(1.0, dt * 6);
    built.beat += dt * std::min(t.max, t.rate + t.run * built.pace);
    const double reach = t.calm + (1 - t.calm) * std::min(1.0, built.pace / t.full);
    const double turn = 2 * kPi * built.beat;
    for (size_t i = 0; i < model.groups.size() && i < built.actors.size(); ++i) {
        const Group& g = model.groups[i];
        const bool moves = g.Moves();
        if (g.spinAxis < 0 && !g.travel && !moves) continue;
        Obj actor = eng::Get(built.actors[i]);
        Obj component = actor ? eng::ReadObj(actor, "DynamicMeshComponent") : nullptr;
        if (!component) continue;
        double angles[3] = {0, 0, 0};                   // about x (roll), y (pitch), z (yaw)
        if (g.spinAxis >= 0) angles[g.spinAxis] += std::fmod(seconds * g.speed, 360.0);
        const double phase = g.phase * kPi / 180;
        if (g.swingAxis >= 0) angles[g.swingAxis] += g.swingAngle * reach * std::sin(turn + phase);
        Rot r{angles[1], angles[2], angles[0]};
        if (g.travel) {
            r.yaw += built.travelYaw;
            Params p(eng::FunctionOn(component, "K2_SetWorldRotation"));
            p.Set("NewRotation", r);
            p.Set("bTeleport", uint8_t{1});
            eng::Invoke(component, p);
            continue;
        }
        if (!moves) {
            Params p(eng::FunctionOn(component, "K2_SetRelativeRotation"));
            p.Set("NewRotation", r);
            p.Set("bTeleport", uint8_t{1});
            eng::Invoke(component, p);
            continue;
        }
        // Where the pivot sits on what the group is built on (that group's pivot, or the ball's centre), lifted by
        // the bob.
        const double* base = g.parent >= 0 ? model.groups[static_cast<size_t>(g.parent)].pivot : nullptr;
        Vec3 at{g.pivot[0] - (base ? base[0] : 0), g.pivot[1] - (base ? base[1] : 0), g.pivot[2] - (base ? base[2] : 0)};
        at.z += g.bob * reach * (0.5 - 0.5 * std::cos(2 * turn + phase));
        Params p(eng::FunctionOn(component, "K2_SetRelativeLocationAndRotation"));
        p.Set("NewLocation", at);
        p.Set("NewRotation", r);
        p.Set("bTeleport", uint8_t{1});
        eng::Invoke(component, p);
    }
}

void Destroy(Built& built) {
    for (const auto& a : built.actors)
        if (Obj actor = eng::Get(a)) eng::Call(actor, "K2_DestroyActor");
    built.actors.clear();
}

}  // namespace models
