#include "models.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "cosmetics.hpp"
#include "game.hpp"
#include "log.hpp"
#include "meshfile.hpp"
#include "race.hpp"

namespace models {
namespace {

using eng::Obj;
using eng::Params;

constexpr double kPi = 3.14159265358979323846;
// Measured materials that render on dynamic meshes: the game's plain ball colour (BaseColor, Metallic, Roughness) and
// its emissive light (Light_Color, Light_Emissive_Intensity).
// Since the 2026-10-06 update the racing ball fades out near the camera with materials of the game's own that fade by
// themselves (cooked copies under /Game/Art/Materials/CameraFade, named after the original and a hash); the ball's fade
// puts them on in place of their originals (see cosmetics' FadeOf). It knows the ball colour's copy, but not the
// light's: so lights are built straight on a fade copy, MI_LightEmissiveDimmest's. That is MI_Env_Emissive_Yellow's
// grandparent, and Yellow and its parent White set only the two parameters set here (measured: same look, and it fades;
// the menu ball, which has no fade, shows it as before).
const wchar_t* kColourMaterial = L"/Game/Art/Materials/Instances/Ball/MI_BallRed.MI_BallRed";
const wchar_t* kGlowMaterial = L"/Game/Art/Materials/Environment/Materials/Instances/MI_Env_Emissive_Yellow.MI_Env_Emissive_Yellow";
const wchar_t* kGlowFadeMaterial =
    L"/Game/Art/Materials/CameraFade/Opaque/MI_LightEmissiveDimmest_CF_f9b0ffd01e.MI_LightEmissiveDimmest_CF_f9b0ffd01e";
// Glass. The stadium water (M_Water_Base) draws in the default translucency pass; M_Glass in the one before depth of
// field, so water behind it is drawn over it (reported: a clear ball vanished in front of water). Clear glass is
// M_GlassV2, which is in the default pass with the water, so the two sort by distance. V2 can't be tinted (its
// ColorGlass changes nothing, measured), and neither can any other glass of the game in that pass (measured:
// M_GlassStylized, M_DiscCheckpointGlass, the snow globe's, the translucent light, the Vefects flat and the ball wake
// materials), so tinted glass is M_Glass (read from the package: ColorGlass, Opacity, Refraction), moved into the
// water's pass (GlassInWatersPass).
const wchar_t* kClearGlassMaterial = L"/Game/Art/Materials/Masters/M_GlassV2.M_GlassV2";
const wchar_t* kTintedGlassMaterial = L"/Game/Art/Materials/Masters/M_Glass.M_Glass";
// Refracting glass: the EP Master Materials glass the game ships (MM_EP_GlassPBR, read from the package), through its
// example instance, which brings the reflection picture ("Fake Cubemap") and a flat normal. Its parameters (names read
// in game with matparams): [Glass] Refraction, Translucent Color, Reflective color, Curvature, Chromatic Aberration,
// [Opacity] Value, [Roughness] Value, [Specular] Value.
const wchar_t* kRefractingGlassMaterial =
    L"/Game/Packs/EPMasterMaterials/Materials/Instances/Examples/Glass/MI_EP_GlassExample01d.MI_EP_GlassExample01d";

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

// A rotator's turn applied to a vector, as FRotationMatrix does (the engine's convention, so "rot" means what it means
// on the other shapes).
void Turn(const double rot[3], const double in[3], double out[3]) {
    const double k = kPi / 180, sp = std::sin(rot[0] * k), cp = std::cos(rot[0] * k), sy = std::sin(rot[1] * k),
                 cy = std::cos(rot[1] * k), sr = std::sin(rot[2] * k), cr = std::cos(rot[2] * k);
    const double x[3] = {cp * cy, cp * sy, sp};
    const double y[3] = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    const double z[3] = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    for (int c = 0; c < 3; ++c) out[c] = in[0] * x[c] + in[1] * y[c] + in[2] * z[c];
}

// The words after "mesh": the file (in quotes when it has spaces), then key=value options.
bool MeshWords(const std::string& line, std::string* file, std::vector<std::string>* options) {
    size_t at = line.find("mesh") + 4;
    while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at]))) ++at;
    if (at >= line.size()) return false;
    size_t end;
    if (line[at] == '"') {
        end = line.find('"', at + 1);
        if (end == std::string::npos) return false;
        *file = line.substr(at + 1, end - at - 1);
        ++end;
    } else {
        end = at;
        while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) ++end;
        *file = line.substr(at, end - at);
    }
    std::stringstream rest(line.substr(end));
    for (std::string word; rest >> word;) options->push_back(word);
    return !file->empty();
}

// A model file read, placed and baked for a mesh part: its materials join the model's.
bool LoadMesh(const std::string& fileName, const std::vector<std::string>& options, const std::wstring& folder, Model* model, Part* part,
              std::string* why) {
    auto option = [&](const char* key, std::string* out) {
        const std::string prefix = std::string(key) + "=";
        for (const auto& word : options)
            if (word.rfind(prefix, 0) == 0) {
                *out = word.substr(prefix.size());
                return true;
            }
        return false;
    };
    auto number = [&](const char* key, double fallback) {
        std::string v;
        return option(key, &v) ? std::atof(v.c_str()) : fallback;
    };
    std::wstring file = eng::Widen(fileName);
    for (auto& c : file)
        if (c == L'/') c = L'\\';
    if (!(file.size() > 1 && file[1] == L':') && !folder.empty()) file = folder + (folder.back() == L'\\' ? L"" : L"\\") + file;
    meshfile::Model m;
    std::string error;
    if (!m.Load(file, hostlog::DataDir() + L"\\cache\\models", &error)) {
        *why = fileName + ": " + error;
        return false;
    }
    auto data = std::make_shared<MeshData>();
    data->file = file;
    std::string v;
    double at[3] = {0, 0, 0}, rot[3] = {0, 0, 0}, scale[3] = {1, 1, 1};
    if (option("at", &v) && !Numbers(v, at, 3)) return *why = "at=x,y,z", false;
    if (option("rot", &v) && !Numbers(v, rot, 3)) return *why = "rot=pitch,yaw,roll", false;
    if (option("scale", &v) && !Numbers(v, scale, 3)) return *why = "scale=x,y,z", false;
    const double size = number("size", 0);
    const int frames = std::clamp(static_cast<int>(number("frames", 24)), 2, 60);
    data->rate = number("rate", 1);
    data->run = number("run", 0);
    // Materials: the model's own for "material=", else the file's, added to the model's.
    int forced = -1;
    if (option("material", &v)) {
        for (size_t i = 0; i < model->materials.size(); ++i)
            if (model->materials[i].name == v) forced = static_cast<int>(i);
        if (forced < 0) return *why = "no material '" + v + "' (define it first)", false;
    }
    const int base = static_cast<int>(model->materials.size());
    if (forced < 0)
        for (const auto& fm : m.Materials()) {
            Material mat;
            mat.name = fileName + "#" + fm.name;
            mat.r = fm.r, mat.g = fm.g, mat.b = fm.b;
            mat.rough = fm.rough;
            const float glow = std::max({fm.er, fm.eg, fm.eb});
            if (!fm.textureFile.empty() && GetFileAttributesW(fm.textureFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
                mat.finish = Finish::Image;
                mat.image = fm.textureFile;
            } else if (glow > 0.01f) {
                mat.finish = Finish::Glow;
                mat.r = fm.er / glow, mat.g = fm.eg / glow, mat.b = fm.eb / glow;
                mat.bright = 5 * glow;
            } else if (fm.transmission > 0.01f) {
                // Blender's glass (Transmission, IOR): refracting glass, tinted by its base colour, reflecting by its
                // specular. The more it transmits, the less of the glass itself is drawn.
                mat.finish = Finish::Glass;
                mat.refracts = mat.tinted = true;
                mat.ior = std::clamp(fm.ior, 1.0f, 3.0f);
                mat.reflect = std::clamp(fm.specular, 0.0f, 1.0f);
                mat.opacity = std::clamp(fm.a * (1 - fm.transmission), 0.0f, 1.0f);
            } else if (fm.a < 0.99f) {
                mat.finish = Finish::Glass;
                mat.tinted = true;
                mat.opacity = fm.a;
            } else {
                mat.finish = fm.metallic >= 0.5f ? Finish::Metal : Finish::Plastic;
            }
            model->materials.push_back(mat);
        }
    for (const auto& item : m.Items()) {
        data->materials.push_back(forced >= 0 ? forced : base + item.material);
        data->uv.push_back(item.uv);
        data->indices.push_back(item.indices);
        data->triangles += item.indices.size() / 3;
    }
    // Placed from the rest pose: its longest side "size" cm (as the file has it), then turned by "rot", then moved so
    // the bottom of its bounds, centred, is on "at".
    const meshfile::Pose rest = m.At(-1, 0);
    auto bounds = [](const std::vector<std::vector<float>>& positions, double lo[3], double hi[3]) {
        for (int c = 0; c < 3; ++c) lo[c] = 1e30, hi[c] = -1e30;
        for (const auto& p : positions)
            for (size_t i = 0; i + 2 < p.size(); i += 3)
                for (int c = 0; c < 3; ++c) {
                    lo[c] = std::min(lo[c], static_cast<double>(p[i + static_cast<size_t>(c)]));
                    hi[c] = std::max(hi[c], static_cast<double>(p[i + static_cast<size_t>(c)]));
                }
    };
    double lo[3], hi[3];
    bounds(rest.positions, lo, hi);
    const double longest = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-6});
    const double k = size > 0 ? size / longest : 1;
    double offset[3] = {0, 0, 0};
    auto place = [&](const meshfile::Pose& pose) {
        Frame f;
        f.positions.resize(pose.positions.size());
        f.normals.resize(pose.normals.size());
        for (size_t it = 0; it < pose.positions.size(); ++it) {
            const auto& p = pose.positions[it];
            const auto& n = pose.normals[it];
            f.positions[it].resize(p.size());
            f.normals[it].resize(n.size());
            for (size_t i = 0; i + 2 < p.size(); i += 3) {
                double local[3], turned[3], nl[3], nt[3];
                for (int c = 0; c < 3; ++c) {
                    local[c] = p[i + static_cast<size_t>(c)] * k * scale[c];
                    nl[c] = n[i + static_cast<size_t>(c)] / (scale[c] != 0 ? scale[c] : 1);
                }
                Turn(rot, local, turned);
                Turn(rot, nl, nt);
                const double len = std::sqrt(nt[0] * nt[0] + nt[1] * nt[1] + nt[2] * nt[2]);
                for (int c = 0; c < 3; ++c) {
                    f.positions[it][i + static_cast<size_t>(c)] = static_cast<float>(turned[c] + offset[c]);
                    f.normals[it][i + static_cast<size_t>(c)] = static_cast<float>(len > 0 ? nt[c] / len : 0);
                }
            }
        }
        return f;
    };
    data->rest = place(rest);                           // turned about the file's origin, to find where it ends up
    bounds(data->rest.positions, lo, hi);
    const double stand[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]};
    for (int c = 0; c < 3; ++c) offset[c] = at[c] - stand[c];
    data->rest = place(rest);
    auto bake = [&](const char* key, std::vector<Frame>* out, double* length) {
        std::string name;
        if (!option(key, &name)) return true;
        const int anim = m.FindAnimation(name);
        if (anim < 0) {
            std::string list;
            for (const auto& a : m.Animations()) list += (list.empty() ? "" : ", ") + a;
            *why = fileName + " has no animation '" + name + "'" + (list.empty() ? " (it has none)" : " (it has: " + list.substr(0, 400) + ")");
            return false;
        }
        *length = m.Length(anim);
        for (int f = 0; f < frames; ++f) out->push_back(place(m.At(anim, *length * f / frames)));
        return true;
    };
    if (!bake("anim", &data->clip, &data->clipLength) || !bake("idle", &data->idle, &data->idleLength)) return false;
    if (!data->idle.empty() && data->clip.empty()) {                // an idle only: it plays all the time
        data->clip = std::move(data->idle);
        data->clipLength = data->idleLength;
        data->idle.clear();
    }
    part->shape = Shape::Mesh;
    part->mesh = data;
    part->material = data->materials.empty() ? 0 : data->materials[0];
    hostlog::Info("models: " + fileName + ": " + std::to_string(data->triangles) + " triangles, " + std::to_string(m.Materials().size()) +
                  " material(s)" + (data->clip.empty() ? "" : ", " + std::to_string(data->clip.size() + data->idle.size()) + " frames baked"));
    return true;
}

}  // namespace

bool Parse(const std::string& text, Model* model, std::string* error, const std::wstring& folder) {
    *model = Model{};
    Group main;
    main.name = "main";
    model->groups.push_back(main);
    bool implicitMain = true;           // groups[0] is still that one, made here, not one the text named
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
                if (w.size() >= 4 && w[3].size() == 7 && w[3][0] == '#') {
                    m.tinted = true;
                    const long rgb = std::strtol(w[3].c_str() + 1, nullptr, 16);
                    m.r = Linear((rgb >> 16) & 255);
                    m.g = Linear((rgb >> 8) & 255);
                    m.b = Linear(rgb & 255);
                }
                m.opacity = static_cast<float>(num("opacity", 0.2));
                m.rough = static_cast<float>(num("rough", 0.05));
                if (m.opacity < 0 || m.opacity > 1) return fail("glass: opacity from 0 to 1");
                std::string given;
                if (value("ior", &given) || value("reflect", &given)) {        // refracting glass
                    m.refracts = m.tinted = true;
                    m.ior = static_cast<float>(num("ior", 1.5));
                    m.reflect = static_cast<float>(num("reflect", 1));
                    if (m.ior < 1 || m.ior > 3) return fail("glass: ior from 1 to 3");
                    if (m.reflect < 0 || m.reflect > 1) return fail("glass: reflect from 0 to 1");
                }
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
        if (w[0] == "ball") {                           // ball hidden
            if (w.size() != 2 || w[1] != "hidden") return fail("ball hidden");
            model->hideBall = true;
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
            // The implicit first group goes if nothing was put in it; a group the text named stays even when empty (an
            // empty travelling group that others are built on: it was dropped, and "on=" then failed, reported).
            if (implicitMain && model->groups.size() == 1 && model->groups[0].parts.empty()) model->groups.clear();
            implicitMain = false;
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
        if (w[0] == "mesh") {
            std::string fileName, why;
            std::vector<std::string> options;
            if (!MeshWords(line, &fileName, &options)) return fail("mesh <file> [options]");
            Part p{};
            if (!LoadMesh(fileName, options, folder, model, &p, &why)) return fail(why);
            model->groups.back().parts.push_back(p);
            continue;
        }
        static const std::pair<const char*, Shape> shapes[] = {
            {"sphere", Shape::Sphere}, {"box", Shape::Box},   {"cylinder", Shape::Cylinder}, {"cone", Shape::Cone},
            {"capsule", Shape::Capsule}, {"disc", Shape::Disc}, {"ring", Shape::Ring},       {"saw", Shape::Saw},
            {"cup", Shape::Cup},       {"bowl", Shape::Bowl},   {"spiral", Shape::Spiral}};
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
        if (p.shape == Shape::Cup || p.shape == Shape::Bowl) p.thick = num("wall", 0);
        p.inner = num("inner", 0);
        p.turns = num("turns", 0);
        std::string v;
        if (value("size", &v) && !Numbers(v, p.size, 3)) return fail("size=x,y,z");
        if (value("at", &v) && !Numbers(v, p.at, 3)) return fail("at=x,y,z");
        if (value("rot", &v) && !Numbers(v, p.rot, 3)) return fail("rot=pitch,yaw,roll");
        if (value("scale", &v) && !Numbers(v, p.scale, 3)) return fail("scale=x,y,z");
        const bool ok = p.shape == Shape::Box        ? p.size[0] > 0 && p.size[1] > 0 && p.size[2] > 0
                        : p.shape == Shape::Ring     ? p.r > 0 && p.thick > 0
                        : p.shape == Shape::Saw      ? p.r > 0 && p.teeth >= 3 && p.thick > 0 && p.depth > 0 && p.depth < p.r
                        : p.shape == Shape::Cup      ? p.r > 0 && p.h > 0 && p.thick > 0 && p.thick < p.r
                        : p.shape == Shape::Bowl     ? p.r > 0 && p.thick > 0 && p.thick < p.r
                        : p.shape == Shape::Spiral   ? p.r > 0 && p.inner >= 0 && p.inner < p.r && p.turns > 0 && p.thick > 0
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

bool Append(Obj mesh, const Part& part);

// A spiral: short capsules end to end along a flat coil around z (in the part's own space), from radius r in to
// radius inner over `turns` turns, each placed with the part's transform.
bool AppendSpiral(Obj mesh, const Part& part) {
    const int steps = std::max(8, static_cast<int>(part.turns * 28));
    auto point = [&](int i) {
        const double t = static_cast<double>(i) / steps, a = 2 * kPi * part.turns * t, rad = part.r + (part.inner - part.r) * t;
        return Vec3{rad * std::cos(a), rad * std::sin(a), 0};
    };
    Obj math = Lib("KismetMathLibrary");
    const Transform whole = MakeTransform(part.at, part.rot, part.scale);
    bool ok = true;
    for (int i = 0; i < steps; ++i) {
        const Vec3 a = point(i), b = point(i + 1);
        const Vec3 d{b.x - a.x, b.y - a.y, b.z - a.z};
        const double length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const Rot along = eng::Call(math, "MakeRotFromZ", d).ReturnAs<Rot>();
        // The capsule stands on its base: start it half a thickness back so the pieces overlap into one tube.
        const double r = part.thick / 2;
        const Vec3 start{a.x - d.x / length * r, a.y - d.y / length * r, a.z};
        const double at[3] = {start.x, start.y, start.z}, rot[3] = {along.pitch, along.yaw, along.roll}, one[3] = {1, 1, 1};
        const Transform local = MakeTransform(at, rot, one);
        const Params composed = eng::Call(math, "ComposeTransforms", local, whole);
        Transform t{};
        if (const uint8_t* bytes = composed.Return()) std::memcpy(t.bytes, bytes, sizeof t.bytes);
        Obj lib = Lib("GeometryScriptLibrary_MeshPrimitiveFunctions");
        Params p(eng::FindFunction(eng::ClassOf(lib), "AppendCapsule"));
        Options options;
        options.materialId = part.material;
        p.Set("TargetMesh", mesh);
        p.Set("PrimitiveOptions", options);
        p.Set("Transform", t);
        p.Set("Radius", static_cast<float>(r));
        p.Set("LineLength", static_cast<float>(length));
        p.Set("HemisphereSteps", int32_t{3});
        p.Set("CircleSteps", int32_t{10});
        p.Set("SegmentSteps", int32_t{1});
        p.Set("Origin", uint8_t{1});
        ok &= eng::Invoke(lib, p);
    }
    return ok;
}

bool Append(Obj mesh, const Part& part) {
    if (part.shape == Shape::Spiral) return AppendSpiral(mesh, part);
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
        case Shape::Cup:
        case Shape::Bowl: fn = "AppendRevolvePolygon"; break;
        case Shape::Spiral:
        case Shape::Mesh: return false;                 // built from their own buffers (AppendFrame)
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
        case Shape::Bowl: {
            // The shell's cross-section (distance from the axis, height): a quarter circle out from the bottom to the
            // rim at the centre's height, and back in along the inside, swept all the way round.
            std::vector<std::pair<double, double>> section;
            const int n = 12;
            for (int i = 0; i <= n; ++i) {
                const double a = kPi / 2 * i / n;
                section.push_back({part.r * std::sin(a), -part.r * std::cos(a)});
            }
            const double in = part.r - part.thick;
            for (int i = n; i >= 0; --i) {
                const double a = kPi / 2 * i / n;
                section.push_back({in * std::sin(a), -in * std::cos(a)});
            }
            const ArrayHeader points = Points(section);
            RevolveOptions revolve;
            p.Set("PolygonVertices", points);
            p.Set("RevolveOptions", revolve);
            p.Set("Radius", 0.0f);
            p.Set("Steps", int32_t{48});
            break;
        }
        case Shape::Spiral:
        case Shape::Mesh: break;
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

// A material's pass is its TranslucencyPass (0 before depth of field, 1 after: M_Glass 0, M_Water_Base 1, read in
// game), and the renderer reads it live (measured). So M_Glass is moved into the water's pass, where the two sort by
// distance, and drawn after the water (TranslucentSortPriority, kGlassSortPriority). Measured over water from the top:
// before, tinted glass vanished wherever water was behind it, even at full opacity; after, it shows at 25%. What was
// already drawing with M_Glass keeps its old pass and isn't drawn at all (measured) until its render state is rebuilt,
// so those are rebuilt (a visibility toggle: MarkRenderStateDirty isn't callable). The game reloads M_Glass with its
// own setting when it's unloaded (measured: back to 0 after a map change), so this is checked each time glass is made.
constexpr int32_t kGlassSortPriority = 1;

void GlassInWatersPass(Obj glass) {
    uint8_t pass = 1;
    if (!glass || !eng::ReadBytes(glass, "TranslucencyPass", &pass, 1) || pass != 0) return;
    const uint8_t after = 1;
    eng::WriteBytes(glass, "TranslucencyPass", &after, 1);
    const auto started = std::chrono::steady_clock::now();
    Obj primitive = eng::FindClass("PrimitiveComponent");
    std::vector<Obj> found;
    eng::ForEachObject([&](Obj o) {
        if (!eng::IsDefaultObject(o) && eng::IsA(o, primitive)) found.push_back(o);
        return true;
    });
    int rebuilt = 0;
    for (Obj c : found) {
        bool visible = false;
        if (!eng::ReadBool(c, "bVisible", &visible) || !visible) continue;
        const Params count = eng::Call(c, "GetNumMaterials");
        int32_t n = 0;
        if (const uint8_t* r = count.Return()) std::memcpy(&n, r, 4);
        bool uses = false;
        for (int32_t i = 0; i < n && !uses; ++i) {
            Obj m = eng::Call(c, "GetMaterial", i).ReturnObj();
            uses = m && eng::Call(m, "GetBaseMaterial").ReturnObj() == glass;
        }
        if (!uses) continue;
        eng::Call(c, "SetVisibility", uint8_t{0}, uint8_t{0});
        eng::Call(c, "SetVisibility", uint8_t{1}, uint8_t{0});
        ++rebuilt;
    }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    hostlog::Info("models: tinted glass moved into the water's pass; " + std::to_string(rebuilt) + " of " + std::to_string(found.size()) +
                  " components redrawn in " + std::to_string(took) + " ms");
}

// A texture per image file, loaded once for the session (every model built with it shares it).
Obj ImageTexture(const std::wstring& file) {
    static std::vector<std::pair<std::wstring, eng::Weak>> loaded;
    for (const auto& [f, t] : loaded)
        if (f == file)
            if (Obj texture = eng::Get(t)) return texture;
    Obj texture = cosmetics::LoadTexture(file);
    if (texture) loaded.push_back({file, eng::MakeWeak(texture)});
    return texture;
}

// [Glass] Refraction for an index of refraction: the example instance's -0.14 (MI_EP_GlassExample01d) stands for
// ordinary glass (IOR 1.5). Measured with live reflections: -0.13 to -0.7 look nearly the same, the capture does the
// work, so the example's scale is kept.
constexpr float kRefractionPerIor = -0.28f;
// [Glass] Curvature: how far in from the rim the reflection reaches. Measured on Leth Trial: the example's 0.9 laid a
// white haze of reflection over the whole ball (0 and 0.2 turn it milky white), 1.2 keeps it near the rim so the ball
// reads as clear glass, 1.5 and up darken it.
constexpr float kGlassCurvature = 1.2f;
// What refracting glass reflects until a live capture feeds it (the Customize page's ball, a replay): the HDRI
// Backdrop plugin's sky the game ships, instead of the example's street photo (measured: it read as a street inside
// the ball).
const wchar_t* kGlassSkyCube = L"/HDRIBackdrop/Textures/approaching_storm_4k.approaching_storm_4k";

// Refracting glass materials made so far (for the glassset test command, which tunes them live).
std::vector<eng::Weak> gRefracting;
std::wstring gRefractingParent;         // tests only (glassparent): another base material for refracting glass

// A texture parameter of a material instance set to a game or engine texture asset; false if it doesn't load.
bool texture(Obj mid, const char* parameter, const wchar_t* asset) {
    Obj tex = cosmetics::LoadAsset(asset);
    if (!mid || !tex) return false;
    const std::wstring wide = eng::Widen(parameter);
    const Params named = eng::Call(Lib("KismetStringLibrary"), "Conv_StringToName",
                                   eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
    const uint8_t* name = named.Return();
    if (!name) return false;
    Params p(eng::FunctionOn(mid, "SetTextureParameterValue"));
    p.Set("ParameterName", name, 8);
    p.Set("Value", tex);
    return eng::Invoke(mid, p);
}

// The light's parent: its fade copy, or the light without fade if the game no longer has that copy.
Obj GlowParent() {
    static bool noCopy = false;
    if (!noCopy) {
        if (Obj copy = cosmetics::LoadAsset(kGlowFadeMaterial)) return copy;
        noCopy = true;
        hostlog::Warn("models: the game's camera fade copy of the light material did not load; lights on the ball won't fade");
    }
    return cosmetics::LoadAsset(kGlowMaterial);
}

Obj MakeMaterial(const Material& m, Obj worldContext) {
    if (m.finish == Finish::Image) {
        Obj material = cosmetics::ImageMaterial(ImageTexture(m.image), "ModelImage");
        if (!material) hostlog::Warn("models: the image of material " + m.name + " did not load");
        return material;
    }
    const wchar_t* path = m.finish == Finish::Glow    ? kGlowMaterial
                          : m.finish == Finish::Glass ? (m.refracts ? (gRefractingParent.empty() ? kRefractingGlassMaterial : gRefractingParent.c_str())
                                                         : m.tinted ? kTintedGlassMaterial : kClearGlassMaterial)
                                                      : kColourMaterial;
    Obj parent = path == kGlowMaterial ? GlowParent() : cosmetics::LoadAsset(path);
    if (!parent) return nullptr;
    if (path == kTintedGlassMaterial) GlassInWatersPass(parent);
    const Params made = eng::Call(Lib("KismetMaterialLibrary"), "CreateDynamicMaterialInstance", worldContext, parent,
                                  std::array<uint8_t, 8>{}, uint8_t{0});
    Obj mid = made.ReturnObj();
    if (!mid) return nullptr;
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
    if (m.finish == Finish::Glass && m.refracts) {
        scalar("[Glass] Refraction", kRefractionPerIor * (m.ior - 1));
        vector("[Glass] Translucent Color", m.r, m.g, m.b);
        vector("[Glass] Reflective color", m.reflect, m.reflect, m.reflect);
        scalar("[Specular] Value", m.reflect);
        scalar("[Roughness] Value", m.rough);
        scalar("[Opacity] Value", m.opacity);
        scalar("[Glass] Chromatic Aberration", 0);
        scalar("[Glass] Curvature", kGlassCurvature);
        // The example instance's plaster normal map frosts the glass and its base colour texture greys it (measured):
        // a flat normal and plain white instead.
        // The engine's Starter Content glass and the game's M_Glass name theirs plainly (read from the packages);
        // a material ignores parameters it doesn't have.
        vector("ColorGlass", m.r, m.g, m.b);
        scalar("Opacity", m.opacity);
        scalar("Refraction", m.ior);
        scalar("Roughness", m.rough);
        scalar("Specular", m.reflect);
        texture(mid, "Normal Map", L"/Engine/EngineMaterials/FlatNormal.FlatNormal");
        texture(mid, "Base Color", L"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
        texture(mid, "Fake Cubemap", kGlassSkyCube);
        gRefracting.push_back(eng::MakeWeak(mid));
    } else if (m.finish == Finish::Glass) {
        vector("ColorGlass", m.r, m.g, m.b);
        scalar("Opacity", m.opacity);
        if (m.tinted) {
            scalar("Roughness", m.rough);
            scalar("Refraction", 1);        // no bending: M_Glass's own amount drew a glass bowl dark grey (measured)
        }
    } else if (m.finish == Finish::Glow) {
        vector("Light_Color", m.r, m.g, m.b);
        scalar("Light_Emissive_Intensity", m.bright);
    } else {
        vector("BaseColor", m.r, m.g, m.b);
        scalar("Metallic", m.finish == Finish::Metal ? 1.0f : 0.0f);
        scalar("Roughness", m.rough);
        // MI_BallRed's master (M_ArenaColorBase, read from the package) mixes a concrete texture into the colour
        // (ConcreteValue, 0.05 on MI_BallRed) and adds a pale rim colour (FresnelColorMult) that turned orange to
        // yellow and peach to grey on the menu ball (measured): both off, so a part is the colour it was given.
        scalar("ConcreteValue", 0);
        scalar("FresnelColorMult", 0);
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

Obj SpawnHolder() { return SpawnMeshActor(game::PlayerController()); }

// Memory from the engine's allocator for the mesh buffers (a padded string donates its buffer, as Points does). Kept
// and reused: AppendBuffersToMesh only reads them.
struct EngineBuffer {
    void* data = nullptr;
    size_t bytes = 0;
};

void* Room(EngineBuffer& b, size_t bytes) {
    if (bytes <= b.bytes) return b.data;
    const size_t want = std::max(bytes, b.bytes * 2);
    const std::wstring empty;
    const eng::FString s{empty.c_str(), 1, 1};
    const Params p = eng::Call(Lib("KismetStringLibrary"), "LeftPad", s, static_cast<int32_t>(want / 2 + 8));
    size_t size = 0;
    const uint8_t* r = p.Return(&size);
    if (!r || size != 16) return nullptr;
    void* data = nullptr;
    std::memcpy(&data, r, sizeof data);
    if (!data) return nullptr;
    b.data = data;
    b.bytes = want;
    return data;
}

// FGeometryScriptSimpleMeshBuffers (read from the game's types: 13 arrays, Vertices +0x00, Normals +0x10, UV0 +0x20,
// VertexColors +0xa0, Triangles +0xb0, TriGroupIDs +0xc0).
struct MeshBuffers {
    ArrayHeader arrays[13];
};
constexpr int kVertices = 0, kNormals = 1, kUV0 = 2, kTriangles = 11;

// A baked frame's triangles added to a dynamic mesh, each item with its material, moved by -pivot.
bool AppendFrame(Obj mesh, const MeshData& data, const Frame& frame, const double pivot[3]) {
    static EngineBuffer vertices, normals, uvs, triangles;
    Obj lib = Lib("GeometryScriptLibrary_MeshBasicEditFunctions");
    Obj fn = lib ? eng::FindFunction(eng::ClassOf(lib), "AppendBuffersToMesh") : nullptr;
    if (!fn) return false;
    bool ok = true;
    for (size_t it = 0; it < data.indices.size() && it < frame.positions.size(); ++it) {
        const auto& pos = frame.positions[it];
        const auto& nrm = frame.normals[it];
        const auto& uv = data.uv[it];
        const auto& idx = data.indices[it];
        const size_t count = pos.size() / 3, tris = idx.size() / 3;
        if (count == 0 || tris == 0) continue;
        auto* v = static_cast<double*>(Room(vertices, count * 24));
        auto* n = static_cast<double*>(Room(normals, count * 24));
        auto* t = static_cast<double*>(Room(uvs, count * 16));
        auto* f = static_cast<int32_t*>(Room(triangles, tris * 12));
        if (!v || !n || !t || !f) return false;
        for (size_t i = 0; i < count; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                v[i * 3 + c] = pos[i * 3 + c] - pivot[c];
                n[i * 3 + c] = nrm[i * 3 + c];
            }
            t[i * 2] = i * 2 < uv.size() ? uv[i * 2] : 0;
            t[i * 2 + 1] = i * 2 + 1 < uv.size() ? uv[i * 2 + 1] : 0;
        }
        // In the files' own order: with the axes turned into the game's (meshfile.hpp), that is the order that faces
        // out (measured: reversed, the T-rex drew black, its insides showing).
        for (size_t k = 0; k < tris; ++k) {
            f[k * 3] = static_cast<int32_t>(idx[k * 3]);
            f[k * 3 + 1] = static_cast<int32_t>(idx[k * 3 + 1]);
            f[k * 3 + 2] = static_cast<int32_t>(idx[k * 3 + 2]);
        }
        MeshBuffers b{};
        b.arrays[kVertices] = {v, static_cast<int32_t>(count), static_cast<int32_t>(count)};
        b.arrays[kNormals] = {n, static_cast<int32_t>(count), static_cast<int32_t>(count)};
        b.arrays[kUV0] = {t, static_cast<int32_t>(count), static_cast<int32_t>(count)};
        b.arrays[kTriangles] = {f, static_cast<int32_t>(tris), static_cast<int32_t>(tris)};
        Params p(fn);
        p.Set("TargetMesh", mesh);
        p.Set("Buffers", b);
        p.Set("MaterialID", static_cast<int32_t>(data.materials[it]));
        p.Set("bDeferChangeNotifications", uint8_t{0});
        // The list of the new triangles' ids is a shared array the call makes and nothing here frees. Given back to
        // every call, it is refilled rather than made again, so there is one for the session.
        static std::vector<uint8_t> indexList;
        const int32_t listSize = p.SizeOf("NewTriangleIndicesList");
        if (listSize > 0 && indexList.size() == static_cast<size_t>(listSize)) p.Set("NewTriangleIndicesList", indexList.data(), indexList.size());
        ok &= eng::Invoke(lib, p);
        size_t got = 0;
        if (const uint8_t* list = p.Get("NewTriangleIndicesList", &got); list && got >= 16) {
            static bool checked = false;
            if (!checked && !indexList.empty()) {
                checked = true;
                const bool same = std::memcmp(indexList.data() + 8, list + 8, 8) == 0;
                hostlog::Info(std::string("models: the mesh buffers' index list is ") + (same ? "reused" : "made again") + " by each call");
            }
            indexList.assign(list, list + got);
        }
    }
    return ok;
}

bool Built::Alive() const {
    if (!eng::Get(parent)) return false;
    for (const auto& a : actors)
        if (!eng::Get(a)) return false;
    for (const auto& f : flips)
        for (const auto& a : f.frames)
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
            if (part.shape == Shape::Mesh) {
                if (part.mesh && part.mesh->clip.empty()) AppendFrame(mesh, *part.mesh, part.mesh->rest, group.pivot);
                continue;
            }
            for (int k = 0; k < 3; ++k) part.at[k] -= group.pivot[k];
            Append(mesh, part);
        }
        for (size_t i = 0; i < materials.size(); ++i)
            if (materials[i]) eng::Call(component, "SetMaterial", static_cast<int32_t>(i), materials[i]);
        eng::Call(component, "SetTranslucentSortPriority", kGlassSortPriority);   // glass after the water
        eng::Call(component, "SetCollisionEnabled", uint8_t{0});     // never touches the ball's physics
        // As the game's own ball (its Sphere has bReceivesDecals false): the start pad's shadow decal would otherwise
        // paint the model's lower half black (measured on the start pad and the menu stand).
        eng::Call(component, "SetReceivesDecals", uint8_t{0});
        const uint8_t snap = 2;                                       // EAttachmentRule::SnapToTarget
        Obj on = parent;                                              // the ball, or the group it is built on
        if (group.parent >= 0) {
            Obj holder = eng::Get(built.actors[static_cast<size_t>(group.parent)]);
            on = holder ? eng::ReadObj(holder, "DynamicMeshComponent") : nullptr;
        }
        if (on) eng::Call(actor, "K2_AttachToComponent", on, std::array<uint8_t, 8>{}, snap, snap, snap, uint8_t{0});
        if (group.travel) eng::Call(component, "SetAbsolute", uint8_t{0}, uint8_t{1}, uint8_t{0});
        built.actors.push_back(eng::MakeWeak(actor));
        // Animated mesh parts: a mesh per frame, on the group, only the one shown visible.
        for (const Part& part : group.parts) {
            if (part.shape != Shape::Mesh || !part.mesh || part.mesh->clip.empty()) continue;
            Built::Flip flip;
            flip.group = built.actors.size() - 1;
            flip.mesh = part.mesh;
            const auto started = std::chrono::steady_clock::now();
            for (int set = 0; set < 2; ++set)
                for (const Frame& frame : set == 0 ? part.mesh->clip : part.mesh->idle) {
                    Obj frameActor = SpawnMeshActor(controller);
                    Obj frameComponent = frameActor ? eng::ReadObj(frameActor, "DynamicMeshComponent") : nullptr;
                    Obj frameMesh = frameComponent ? eng::Call(frameComponent, "GetDynamicMesh").ReturnObj() : nullptr;
                    if (!frameMesh) continue;
                    AppendFrame(frameMesh, *part.mesh, frame, group.pivot);
                    for (size_t i = 0; i < materials.size(); ++i)
                        if (materials[i]) eng::Call(frameComponent, "SetMaterial", static_cast<int32_t>(i), materials[i]);
                    eng::Call(frameComponent, "SetTranslucentSortPriority", kGlassSortPriority);
                    eng::Call(frameComponent, "SetCollisionEnabled", uint8_t{0});
                    eng::Call(frameComponent, "SetReceivesDecals", uint8_t{0});
                    eng::Call(frameActor, "K2_AttachToComponent", component, std::array<uint8_t, 8>{}, snap, snap, snap, uint8_t{0});
                    eng::Call(frameComponent, "SetVisibility", uint8_t{0}, uint8_t{0});
                    flip.frames.push_back(eng::MakeWeak(frameActor));
                }
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
            hostlog::Info("models: " + std::to_string(flip.frames.size()) + " frames of " + std::to_string(part.mesh->triangles) +
                          " triangles built in " + std::to_string(took) + " ms");
            built.flips.push_back(std::move(flip));
        }
    }
    return built;
}

std::vector<Obj> Actors(const Built& built) {
    std::vector<Obj> out;
    for (const auto& a : built.actors)
        if (Obj actor = eng::Get(a)) out.push_back(actor);
    for (const auto& f : built.flips)
        for (const auto& a : f.frames)
            if (Obj actor = eng::Get(a)) out.push_back(actor);
    return out;
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
    // Animated meshes: the clip plays faster as the ball goes faster ("rate" at rest, "run" more per m/s), and the
    // idle clip, if there is one, while the ball is still.
    constexpr double kStill = 0.5;                      // m/s
    for (auto& flip : built.flips) {
        const MeshData& m = *flip.mesh;
        const bool idling = !m.idle.empty() && built.pace < kStill;
        if (idling != flip.idling) {
            flip.idling = idling;
            flip.time = 0;
        }
        const double length = idling ? m.idleLength : m.clipLength;
        const size_t count = idling ? m.idle.size() : m.clip.size();
        if (count == 0 || length <= 0) continue;
        flip.time = std::fmod(flip.time + dt * (idling ? 1 : m.rate + m.run * built.pace), length);
        if (flip.time < 0) flip.time += length;
        const size_t frame = std::min(count - 1, static_cast<size_t>(flip.time / length * static_cast<double>(count)));
        const int index = static_cast<int>(idling ? m.clip.size() + frame : frame);
        if (index == flip.shown || index >= static_cast<int>(flip.frames.size())) continue;
        auto show = [&](int i, bool on) {
            Obj actor = i >= 0 && i < static_cast<int>(flip.frames.size()) ? eng::Get(flip.frames[static_cast<size_t>(i)]) : nullptr;
            Obj component = actor ? eng::ReadObj(actor, "DynamicMeshComponent") : nullptr;
            if (component) eng::Call(component, "SetVisibility", static_cast<uint8_t>(on), uint8_t{0});
        };
        show(index, true);
        show(flip.shown, false);
        flip.shown = index;
    }
}

void Destroy(Built& built) {
    for (Obj actor : Actors(built)) eng::Call(actor, "K2_DestroyActor");
    built.actors.clear();
    built.flips.clear();
}

namespace {
// An engine-allocated array of FVector (three doubles each), for the sweep's path: allocated the way Points does it.
ArrayHeader Vectors(const std::vector<std::array<double, 3>>& points) {
    const int n = static_cast<int>(points.size());
    const std::wstring empty;
    // LeftPad to 12n - 1 characters: 24n bytes with the terminator, room for n vectors.
    const eng::FString s{empty.c_str(), 1, 1};
    const Params p = eng::Call(Lib("KismetStringLibrary"), "LeftPad", s, static_cast<int32_t>(12 * n - 1));
    ArrayHeader a{nullptr, 0, 0};
    size_t size = 0;
    const uint8_t* r = p.Return(&size);
    if (!r || size != 16) return a;
    std::memcpy(&a.data, r, sizeof a.data);
    if (!a.data) return a;
    std::memcpy(a.data, points.data(), points.size() * sizeof points[0]);
    a.num = a.max = n;
    return a;
}

Obj Coloured(Obj actor, const Colour& c) {
    Obj component = actor ? eng::ReadObj(actor, "DynamicMeshComponent") : nullptr;
    if (!component) return nullptr;
    Material m;
    m.finish = c.opacity < 1 ? Finish::Glass : c.glow ? Finish::Glow : Finish::Plastic;
    m.tinted = c.opacity < 1;
    m.opacity = c.opacity;
    m.r = c.r;
    m.g = c.g;
    m.b = c.b;
    m.bright = c.bright;
    m.rough = 0.4f;
    if (Obj material = MakeMaterial(m, game::PlayerController())) eng::Call(component, "SetMaterial", int32_t{0}, material);
    eng::Call(component, "SetCollisionEnabled", uint8_t{0});
    eng::Call(component, "SetCastShadow", uint8_t{0});
    eng::Call(component, "SetTranslucentSortPriority", kGlassSortPriority);       // glass after the water
    return actor;
}
}  // namespace

// Its dynamic material (MakeMaterial's, on the mesh's first slot): the glow material's Light_Color and
// Light_Emissive_Intensity, as MakeMaterial sets them.
Obj NewGlowMaterial(float r, float g, float b, float bright) {
    Material m;
    m.finish = Finish::Glow;
    m.r = r;
    m.g = g;
    m.b = b;
    m.bright = bright;
    return MakeMaterial(m, game::PlayerController());
}

Obj GlowMaterial(Obj actor) {
    Obj component = actor ? eng::ReadObj(actor, "DynamicMeshComponent") : nullptr;
    return component ? eng::Call(component, "GetMaterial", int32_t{0}).ReturnObj() : nullptr;
}

bool SetGlow(Obj mid, float r, float g, float b, float bright) {
    if (!mid) return false;
    auto name = [](const char* text, uint8_t out[8]) {
        const std::wstring wide = eng::Widen(text);
        const Params made = eng::Call(Lib("KismetStringLibrary"), "Conv_StringToName",
                                      eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
        if (const uint8_t* n = made.Return()) std::memcpy(out, n, 8);
    };
    static uint8_t colourName[8] = {}, brightName[8] = {};
    static bool named = false;
    if (!named) {
        name("Light_Color", colourName);
        name("Light_Emissive_Intensity", brightName);
        named = true;
    }
    Params colour(eng::FunctionOn(mid, "SetVectorParameterValue"));
    const float value[4] = {r, g, b, 1};
    colour.Set("ParameterName", colourName, 8);
    colour.Set("Value", value);
    Params intensity(eng::FunctionOn(mid, "SetScalarParameterValue"));
    intensity.Set("ParameterName", brightName, 8);
    intensity.Set("Value", bright);
    return eng::Invoke(mid, colour) && eng::Invoke(mid, intensity);
}

// The path a tube is swept along, without what makes the sweep throw a spike across the whole track: points closer
// than the tube is wide, and points where it doubles back past 120 degrees (the swept corner has nowhere to go).
// Measured: one run's trail on S1 Track 01 (a ball jittering on the spot) had 72 steps under 2 cm and 18 full reversals,
// and drew two straight lines through the start and finish; its data was fine.
std::vector<std::array<double, 3>> SweepablePath(const std::vector<std::array<double, 3>>& in, double radius) {
    auto gap = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return std::sqrt(x * x + y * y + z * z);
    };
    const double spacing = std::max(2.0, radius);
    std::vector<std::array<double, 3>> path;
    for (size_t k = 0; k < in.size(); ++k) {
        if (!path.empty() && gap(in[k], path.back()) < spacing) {
            if (k + 1 == in.size() && path.size() > 1) path.back() = in[k];     // the end stays the end
            continue;
        }
        path.push_back(in[k]);
    }
    for (bool removed = true; removed && path.size() > 2;) {
        removed = false;
        std::vector<std::array<double, 3>> kept{path[0]};
        for (size_t k = 1; k + 1 < path.size(); ++k) {
            const auto& a = kept.back();
            const auto& b = path[k];
            const auto& c = path[k + 1];
            const double ax = b[0] - a[0], ay = b[1] - a[1], az = b[2] - a[2], bx = c[0] - b[0], by = c[1] - b[1], bz = c[2] - b[2];
            const double la = std::sqrt(ax * ax + ay * ay + az * az), lb = std::sqrt(bx * bx + by * by + bz * bz);
            if (la > 0 && lb > 0 && (ax * bx + ay * by + az * bz) / (la * lb) < -0.5) {
                removed = true;
                continue;
            }
            kept.push_back(b);
        }
        kept.push_back(path.back());
        path.swap(kept);
    }
    return path;
}

bool AppendTube(Obj actor, const std::vector<std::array<double, 3>>& raw, double radius, int sides) {
    Obj component = actor ? eng::ReadObj(actor, "DynamicMeshComponent") : nullptr;
    Obj mesh = component ? eng::Call(component, "GetDynamicMesh").ReturnObj() : nullptr;
    const std::vector<std::array<double, 3>> path = SweepablePath(raw, radius);
    if (!mesh || path.size() < 2) return false;
    // A circle of `sides` swept along the points (Geometry Script's AppendSimpleSweptPolygon works out each point's
    // frame itself).
    std::vector<std::pair<double, double>> circle;
    for (int i = 0; i < sides; ++i) {
        const double a = 2 * kPi * i / sides;
        circle.push_back({radius * std::cos(a), radius * std::sin(a)});
    }
    Obj lib = Lib("GeometryScriptLibrary_MeshPrimitiveFunctions");
    Params p(eng::FindFunction(eng::ClassOf(lib), "AppendSimpleSweptPolygon"));
    const double zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
    p.Set("TargetMesh", mesh);
    p.Set("PrimitiveOptions", Options{});
    p.Set("Transform", MakeTransform(zero, zero, one));
    p.Set("PolygonVertices", Points(circle));
    p.Set("SweepPath", Vectors(path));
    p.Set("bLoop", uint8_t{0});
    p.Set("bCapped", uint8_t{1});
    p.Set("StartScale", 1.0f);
    p.Set("EndScale", 1.0f);
    p.Set("RotationAngleDeg", 0.0f);
    p.Set("MiterLimit", 1.0f);
    return eng::Invoke(lib, p);
}

bool SetOpacity(Obj mid, float opacity) {
    if (!mid) return false;
    static uint8_t opacityName[8] = {};
    static bool named = false;
    if (!named) {
        const std::wstring wide = L"Opacity";
        const Params made = eng::Call(Lib("KismetStringLibrary"), "Conv_StringToName",
                                      eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
        if (const uint8_t* n = made.Return()) std::memcpy(opacityName, n, 8);
        named = true;
    }
    Params p(eng::FunctionOn(mid, "SetScalarParameterValue"));
    p.Set("ParameterName", opacityName, 8);
    p.Set("Value", opacity);
    return eng::Invoke(mid, p);
}

void SetRefractingGlassParent(const std::string& path) { gRefractingParent = eng::Widen(path); }

// --- live reflections ----------------------------------------------------------------------------------------------------
// While refracting glass is worn, a cube scene capture follows the played ball and renders its surroundings (the ball
// and the meshes on it hidden) into a cube render target, 10 times a second, which the glass takes as its
// "Fake Cubemap": what it shows through it and on it is then the real track. Measured on an RTX 4070 Ti at 256 px:
// 161 fps without, 147-151 with (about 0.6 ms a frame); one capture serves every glass part. No glass, no capture.
// The engine has no Blueprint function that makes a cube render target, so one is spawned (GameplayStatics.SpawnObject)
// and given its GPU resource by UTexture::UpdateResource, a virtual function at vtable +0x318: read from the machine
// code of CanvasRenderTarget2D.UpdateResource's native entry (mov rax,[rcx]; jmp [rax+0x318]) on this build.
constexpr size_t kUpdateResourceSlot = 0x318;
bool gProbeAllowed = true;              // the glassprobe test command can turn it off (to measure what it costs)
double gProbeInterval = 0;              // seconds between captures (0: every frame; 0.1 felt laggy at speed)
int32_t gProbeSize = 128;               // pixels per cube face: 128 softens the reflection a little (256 and 128 cost
                                        // the same, measured: the six scene renders cost, not the pixels)
bool gProbeLean = true;                 // the capture skips what a small curved reflection doesn't show (below)
bool gProbeLead = true;                 // captures where the ball will be halfway to the next capture

// What the capture leaves out (engine show flags, by name; unknown names are ignored): shadows, reflections of
// reflections, fog, particles, decals and lens effects, and far and fine detail. Lighting stays: without global
// illumination and the reflection environment the capture came out flat and dull (measured, side by side).
const wchar_t* const kLeanFlags[] = {L"DynamicShadows", L"ContactShadows", L"CapsuleShadows", L"LumenReflections",
                                     L"ScreenSpaceReflections", L"Fog", L"VolumetricFog", L"Particles", L"Decals",
                                     L"MotionBlur", L"DepthOfField", L"LensFlares", L"Bloom"};
constexpr float kLeanViewDistance = 30000;   // cm
constexpr float kLeanLodFactor = 3;

void MakeLean(Obj capture) {
    struct Flag {
        eng::FString name;
        uint8_t enabled;
        uint8_t pad[7];
    };
    std::vector<std::wstring> names;                     // the engine copies them during the call
    std::vector<Flag> flags;
    for (const wchar_t* n : kLeanFlags) names.emplace_back(n);
    for (const auto& n : names)
        if (flags.size() < std::size(kLeanFlags))
            flags.push_back({eng::FString{n.c_str(), static_cast<int32_t>(n.size() + 1), static_cast<int32_t>(n.size() + 1)}, 0, {}});
    struct {
        Flag* data;
        int32_t num, max;
    } array{flags.data(), static_cast<int32_t>(flags.size()), static_cast<int32_t>(flags.size())};
    Params p(eng::FunctionOn(capture, "SetShowFlagSettings"));
    p.Set("InShowFlagSettings", &array, sizeof array);       // the engine copies the array
    eng::Invoke(capture, p);
    const float distance = kLeanViewDistance, lod = kLeanLodFactor;
    eng::WriteBytes(capture, "MaxViewDistanceOverride", &distance, sizeof distance);
    eng::WriteBytes(capture, "LODDistanceFactor", &lod, sizeof lod);
}
double gNextProbeCheck = 0;
bool gBallWearsGlass = false;           // what the last ScanBall said
eng::Weak gProbeActor, gProbeTarget;
int gProbeGeneration = -1;
double gNextCapture = 0, gNextHide = 0;
size_t gProbeFed = 0;                   // gRefracting entries given the probe's texture so far

Obj SpawnActorOf(Obj cls, Obj worldContext) {
    const double zero[3] = {0, 0, 0}, one[3] = {1, 1, 1};
    Transform t = MakeTransform(zero, zero, one);
    Obj statics = Lib("GameplayStatics");
    Params begin(eng::FunctionOn(statics, "BeginDeferredActorSpawnFromClass"));
    begin.Set("WorldContextObject", worldContext);
    begin.Set("ActorClass", cls);
    begin.Set("SpawnTransform", t);
    begin.Set("CollisionHandlingOverride", uint8_t{1});
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

Obj MakeProbe() {
    Obj controller = game::PlayerController();
    Obj targetClass = eng::FindClass("TextureRenderTargetCube"), captureClass = eng::FindClass("SceneCaptureCube");
    if (!controller || !targetClass || !captureClass) return nullptr;
    Obj target = eng::Call(Lib("GameplayStatics"), "SpawnObject", targetClass, controller).ReturnObj();
    if (!target) return nullptr;
    const int32_t size = gProbeSize;
    eng::WriteBytes(target, "SizeX", &size, sizeof size);
    eng::WriteBool(target, "bHDR", true);
    void** table = *reinterpret_cast<void***>(target);
    void* update = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(table) + kUpdateResourceSlot);
    if (!eng::InImage(update)) {
        hostlog::Warn("glass probe: UpdateResource is not where it was measured; no live reflections");
        return nullptr;
    }
    reinterpret_cast<void (*)(Obj)>(update)(target);
    Obj actor = SpawnActorOf(captureClass, controller);
    Obj capture = actor ? eng::ReadObj(actor, "CaptureComponentCube") : nullptr;
    if (!capture) return nullptr;
    eng::WriteBytes(capture, "TextureTarget", &target, sizeof target);
    eng::WriteBool(capture, "bCaptureEveryFrame", false);
    eng::WriteBool(capture, "bCaptureOnMovement", false);
    if (gProbeLean) MakeLean(capture);
    gProbeTarget = eng::MakeWeak(target);
    hostlog::Info("glass probe: cube capture made (" + eng::PathOf(actor) + ")");
    return actor;
}

// The meshes built on the played ball (DynamicMeshActors within 3 m of it): whether one wears refracting glass, and,
// given a capture, hidden from it with the ball (they would fill it from inside). Every second. The glass's materials
// outlive the ball wearing them (the Customize page keeps its tiles' models), so "worn" is read from the meshes.
bool ScanBall(Obj capture, Obj ball) {
    if (capture) {
        eng::Call(capture, "ClearHiddenComponents");
        if (ball) eng::Call(capture, "HideActorComponents", ball, uint8_t{1});
    }
    static Obj meshClass = nullptr;
    if (!meshClass) meshClass = eng::FindClass("DynamicMeshActor");
    double bx = 0, by = 0, bz = 0;
    if (!meshClass || !race::BallPosition(&bx, &by, &bz)) return false;
    std::vector<Obj> glass;
    for (const auto& weak : gRefracting)
        if (Obj mid = eng::Get(weak)) glass.push_back(mid);
    bool wears = false;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) != meshClass || eng::IsDefaultObject(o)) return true;
        const Vec3 at = eng::Call(o, "K2_GetActorLocation").ReturnAs<Vec3>();
        if (std::abs(at.x - bx) >= 300 || std::abs(at.y - by) >= 300 || std::abs(at.z - bz) >= 300) return true;
        if (capture) eng::Call(capture, "HideActorComponents", o, uint8_t{1});
        if (Obj mesh = eng::ReadObj(o, "DynamicMeshComponent"); mesh && !wears) {
            const int32_t count = eng::Call(mesh, "GetNumMaterials").ReturnAs<int32_t>();
            for (int32_t i = 0; i < count && !wears; ++i) {
                Obj used = eng::Call(mesh, "GetMaterial", i).ReturnObj();
                wears = std::find(glass.begin(), glass.end(), used) != glass.end();
            }
        }
        return true;
    });
    return wears;
}

void FeedProbe(Obj target) {
    const std::wstring wide = L"Fake Cubemap";
    const Params named = eng::Call(Lib("KismetStringLibrary"), "Conv_StringToName",
                                   eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
    const uint8_t* n = named.Return();
    for (; gProbeFed < gRefracting.size(); ++gProbeFed)
        if (Obj mid = eng::Get(gRefracting[gProbeFed]); mid && n) {
            Params p(eng::FunctionOn(mid, "SetTextureParameterValue"));
            p.Set("ParameterName", n, 8);
            p.Set("Value", target);
            eng::Invoke(mid, p);
        }
}

void SetGlassProbe(bool on) {
    gProbeAllowed = on;
    gProbeFed = 0;
}

void DropProbe();

void TuneGlassProbeLook(bool lean, bool lead) {
    gProbeLean = lean;
    gProbeLead = lead;
    DropProbe();                                         // remade with the new settings
}

void TuneGlassProbe(double interval, int size) {
    gProbeInterval = interval;
    if (size > 0 && size != gProbeSize) {
        gProbeSize = size;
        DropProbe();                                     // remade at the new size
    }
}

bool AnyRefractingGlass() {
    for (const auto& weak : gRefracting)
        if (eng::Get(weak)) return true;
    return false;
}

void DropProbe() {
    if (Obj actor = eng::Get(gProbeActor)) eng::Call(actor, "K2_DestroyActor");
    gProbeActor = {};
    gProbeTarget = {};
    gProbeFed = 0;
}

void ProbeFrame() {
    if (gProbeGeneration != game::Generation()) {        // a new map: the old capture went with it
        gProbeGeneration = game::Generation();
        gProbeActor = {};
        gProbeTarget = {};
        gProbeFed = 0;
        gBallWearsGlass = false;
        gNextProbeCheck = 0;
    }
    static bool failed = false;
    if (!gProbeAllowed || failed || !AnyRefractingGlass()) {
        if (eng::Get(gProbeActor)) DropProbe();
        return;
    }
    Obj ball = race::PlayedBallActor();
    if (!ball) {
        if (eng::Get(gProbeActor)) DropProbe();
        return;
    }
    const double now = game::Seconds();
    Obj actor = eng::Get(gProbeActor);
    if (now >= gNextProbeCheck) {                        // the ball's meshes: glass worn? (and hide them from it)
        gNextProbeCheck = now + 1;
        gNextHide = now + 1;
        Obj capture = actor ? eng::ReadObj(actor, "CaptureComponentCube") : nullptr;
        gBallWearsGlass = ScanBall(capture, ball);
        if (!gBallWearsGlass && actor) DropProbe();
    }
    if (!gBallWearsGlass) return;
    actor = eng::Get(gProbeActor);
    if (!actor) {
        actor = MakeProbe();
        if (!actor) {
            failed = true;                               // logged in MakeProbe; the glass keeps its sky
            return;
        }
        gProbeActor = eng::MakeWeak(actor);
        gNextHide = 0;
    }
    if (!actor) return;
    Obj capture = eng::ReadObj(actor, "CaptureComponentCube");
    Obj target = eng::Get(gProbeTarget);
    if (!capture || !target) return;
    if (gNextHide == 0) {                               // just made: hide the ball and its meshes from it
        gNextHide = now + 1;
        ScanBall(capture, ball);
    }
    FeedProbe(target);
    if (now < gNextCapture) return;
    gNextCapture = now + gProbeInterval;
    double x = 0, y = 0, z = 0;
    race::BallPosition(&x, &y, &z);
    if (gProbeLead) {
        // Shown until the next capture, so taken where the ball will be halfway there (and a frame ahead).
        const Vec3 v = eng::Call(ball, "GetVelocity").ReturnAs<Vec3>();
        const double ahead = gProbeInterval / 2 + 1.0 / 60;
        x += v.x * ahead, y += v.y * ahead, z += v.z * ahead;
    }
    Params move(eng::FunctionOn(actor, "K2_SetActorLocation"));
    move.Set("NewLocation", Vec3{x, y, z});
    move.Set("bSweep", uint8_t{0});
    move.Set("bTeleport", uint8_t{1});
    eng::Invoke(actor, move);
    eng::Call(capture, "CaptureScene");
}


int TuneRefractingGlassTexture(const std::string& parameter, const std::string& asset) {
    int tuned = 0;
    for (const auto& weak : gRefracting)
        if (Obj mid = eng::Get(weak)) tuned += texture(mid, parameter.c_str(), eng::Widen(asset).c_str()) ? 1 : 0;
    return tuned;
}

int TuneRefractingGlass(const std::string& parameter, const std::vector<float>& values) {
    const std::wstring wide = eng::Widen(parameter);
    const Params named = eng::Call(Lib("KismetStringLibrary"), "Conv_StringToName",
                                   eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
    const uint8_t* name = named.Return();
    if (!name || values.empty()) return 0;
    int tuned = 0;
    for (const auto& weak : gRefracting)
        if (Obj mid = eng::Get(weak)) {
            if (values.size() == 1) {
                Params p(eng::FunctionOn(mid, "SetScalarParameterValue"));
                p.Set("ParameterName", name, 8);
                p.Set("Value", values[0]);
                tuned += eng::Invoke(mid, p) ? 1 : 0;
            } else {
                const float v[4] = {values[0], values.size() > 1 ? values[1] : 0, values.size() > 2 ? values[2] : 0,
                                    values.size() > 3 ? values[3] : 1};
                Params p(eng::FunctionOn(mid, "SetVectorParameterValue"));
                p.Set("ParameterName", name, 8);
                p.Set("Value", v);
                tuned += eng::Invoke(mid, p) ? 1 : 0;
            }
        }
    return tuned;
}

Obj SpawnMesh(const Colour& colour) {
    Obj controller = game::PlayerController();
    Obj actor = controller ? SpawnMeshActor(controller) : nullptr;
    return actor ? Coloured(actor, colour) : nullptr;
}

Obj SpawnTube(const std::vector<std::array<double, 3>>& path, double radius, const Colour& colour) {
    if (path.size() < 2) return nullptr;
    Obj actor = SpawnMesh(colour);
    if (!actor) return nullptr;
    if (!AppendTube(actor, path, radius, 6)) {
        eng::Call(actor, "K2_DestroyActor");
        return nullptr;
    }
    return actor;
}

Obj SpawnBall(double radius, const Colour& colour) {
    Obj controller = game::PlayerController();
    Obj actor = controller ? SpawnMeshActor(controller) : nullptr;
    Obj component = actor ? eng::ReadObj(actor, "DynamicMeshComponent") : nullptr;
    Obj mesh = component ? eng::Call(component, "GetDynamicMesh").ReturnObj() : nullptr;
    if (!mesh) {
        if (actor) eng::Call(actor, "K2_DestroyActor");
        return nullptr;
    }
    Part ball{};
    ball.shape = Shape::Sphere;
    ball.r = radius;
    Append(mesh, ball);
    return Coloured(actor, colour);
}

}  // namespace models
