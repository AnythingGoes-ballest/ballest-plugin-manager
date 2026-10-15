// 3D model files, as Blender and most other tools export them: glTF 2.0 (.glb, or .gltf with its .bin and images next
// to it, or embedded as data: URIs) and Wavefront OBJ (.obj with its .mtl). Read into triangles per material, with
// normals and texture coordinates, and posed: at rest, or at a moment of one of the file's animations (node
// transforms, and skinned meshes deformed by their joints). No engine code here: models.cpp builds the result.
//
// Space: glTF and OBJ (as Blender writes both) are metres, +Y up, the model's front towards +Z, right-handed. The
// result is the game's: centimetres, +Z up, front towards +X, left-handed. So (x, y, z) becomes (100 z, -100 x, 100 y).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace meshfile {

struct Material {
    std::string name;
    float r = 0.8f, g = 0.8f, b = 0.8f, a = 1;      // linear
    float metallic = 0, rough = 0.5f;
    float er = 0, eg = 0, eb = 0;                    // emission (linear)
    std::wstring textureFile;                        // the base colour image, as a file ("" for none)
    // glTF KHR_materials_transmission / _ior / _specular (Blender writes them for its glass): 0 when not given.
    float transmission = 0, ior = 1.5f, specular = 1;
};

// Triangles of one material of one mesh node: what stays the same whatever the pose.
struct Item {
    int material = 0;
    std::vector<float> uv;                           // 2 per vertex (u right, v down, as glTF and the game)
    std::vector<uint32_t> indices;                   // 3 per triangle
    size_t vertices = 0;
};

// Where the vertices of every item are at one pose.
struct Pose {
    std::vector<std::vector<float>> positions, normals;     // per item, 3 per vertex
};

struct Scene;                                        // the parsed file (meshfile.cpp)

class Model {
public:
    Model();
    ~Model();
    Model(Model&&) noexcept;
    Model& operator=(Model&&) noexcept;
    Model(const Model&) = delete;

    // False with the reason in `error`. `cacheDir`: where images embedded in a .glb are written, to be loaded as files.
    bool Load(const std::wstring& file, const std::wstring& cacheDir, std::string* error);

    const std::vector<Material>& Materials() const { return materials_; }
    const std::vector<Item>& Items() const { return items_; }
    int FindNode(const std::string& name) const;            // a node by its exact name; -1 if none
    std::vector<std::string> Nodes() const;                 // the named nodes' names
    bool ItemUnder(size_t item, int node) const;            // the item hangs from that node or one under it
    std::vector<std::string> Animations() const;
    int FindAnimation(const std::string& fragment) const;   // by name, case-insensitive substring; -1 if none
    double Length(int animation) const;                     // seconds
    Pose At(int animation, double seconds) const;           // animation -1: the rest pose

private:
    Scene* scene_ = nullptr;
    std::vector<Material> materials_;
    std::vector<Item> items_;
};

}  // namespace meshfile
