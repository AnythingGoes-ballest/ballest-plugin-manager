#include "meshfile.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>

#include "json.hpp"

namespace meshfile {

// ------------------------------------------------------------------------------------------------ small maths
using Mat4 = std::array<double, 16>;       // column-major, as glTF writes them: m[col * 4 + row]

Mat4 Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 m{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            m[c * 4 + r] = s;
        }
    return m;
}

Mat4 FromTRS(const double t[3], const double q[4], const double s[3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    Mat4 m = Identity();
    m[0] = (1 - 2 * (y * y + z * z)) * s[0];
    m[1] = (2 * (x * y + z * w)) * s[0];
    m[2] = (2 * (x * z - y * w)) * s[0];
    m[4] = (2 * (x * y - z * w)) * s[1];
    m[5] = (1 - 2 * (x * x + z * z)) * s[1];
    m[6] = (2 * (y * z + x * w)) * s[1];
    m[8] = (2 * (x * z + y * w)) * s[2];
    m[9] = (2 * (y * z - x * w)) * s[2];
    m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    return m;
}

void Point(const Mat4& m, const float* v, double* out) {
    for (int r = 0; r < 3; ++r) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r];
}
void Direction(const Mat4& m, const float* v, double* out) {
    for (int r = 0; r < 3; ++r) out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2];
}

void Slerp(const float* a, const float* b, double t, double* out) {
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    double sign = 1;
    if (d < 0) {
        d = -d;
        sign = -1;
    }
    double wa = 1 - t, wb = t;
    if (d < 0.9995) {
        const double angle = std::acos(d), s = std::sin(angle);
        wa = std::sin((1 - t) * angle) / s;
        wb = std::sin(t * angle) / s;
    }
    double len = 0;
    for (int i = 0; i < 4; ++i) {
        out[i] = wa * a[i] + wb * sign * b[i];
        len += out[i] * out[i];
    }
    len = std::sqrt(len);
    for (int i = 0; i < 4 && len > 0; ++i) out[i] /= len;
}

// ------------------------------------------------------------------------------------------------ the parsed file
struct Node {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    double t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
    bool hasMatrix = false;
    Mat4 matrix = Identity();
    int skin = -1;
};

// The vertex data of one item as the file has it, before posing.
struct Source {
    int node = -1;                         // the node it hangs from (-1: none, as in OBJ)
    int skin = -1;
    std::vector<float> positions, normals;  // 3 per vertex
    std::vector<uint16_t> joints;          // 4 per vertex (skinned)
    std::vector<float> weights;            // 4 per vertex (skinned)
};

struct Skin {
    std::vector<int> joints;
    std::vector<Mat4> inverseBind;
};

struct Channel {
    int node = -1;
    int path = 0;                          // 0 translation, 1 rotation, 2 scale
    int interpolation = 0;                 // 0 linear, 1 step, 2 cubic spline
    std::vector<float> times, values;
};

struct Animation {
    std::string name;
    std::vector<Channel> channels;
    double length = 0;
};

struct Scene {
    std::vector<Node> nodes;
    std::vector<int> roots;
    std::vector<Source> sources;           // one per item
    std::vector<Skin> skins;
    std::vector<Animation> animations;
};

namespace {

bool ReadWhole(const std::wstring& file, std::string* out) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 256ll * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = out->empty() || (ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size());
    CloseHandle(h);
    return ok;
}

bool WriteWhole(const std::wstring& file, const char* data, size_t size) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &wrote, nullptr) && wrote == size;
    CloseHandle(h);
    return ok;
}

std::wstring Folder(const std::wstring& file) {
    const size_t slash = file.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : file.substr(0, slash + 1);
}

std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// A relative file name from a model file (URIs escape spaces as %20), resolved next to the model.
std::wstring Beside(const std::wstring& modelFile, std::string name) {
    std::string decoded;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '%' && i + 2 < name.size() && std::isxdigit(static_cast<unsigned char>(name[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(name[i + 2]))) {
            decoded += static_cast<char>(std::stoi(name.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            decoded += name[i] == '/' ? '\\' : name[i];
        }
    }
    return Folder(modelFile) + Widen(decoded);
}

std::string Base64(const std::string& in) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    int bits = 0, have = 0;
    for (char c : in) {
        const int v = value(c);
        if (v < 0) continue;
        bits = (bits << 6) | v;
        have += 6;
        if (have >= 8) {
            have -= 8;
            out += static_cast<char>((bits >> have) & 0xFF);
        }
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A 64-bit FNV-1a of some bytes, for naming cached images.
std::wstring HashName(const std::string& bytes) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes) h = (h ^ c) * 1099511628211ull;
    wchar_t buf[32];
    swprintf(buf, 32, L"%016llx", static_cast<unsigned long long>(h));
    return buf;
}

double Num(const json::Value* v, double fallback) { return v && v->type == json::Value::Number ? v->number : fallback; }
int Int(const json::Value& o, const char* key, int fallback = -1) { return static_cast<int>(Num(o.Get(key), fallback)); }

// ------------------------------------------------------------------------------------------------ glTF
class Gltf {
public:
    Gltf(const std::wstring& file, const std::wstring& cacheDir, Scene* scene, std::vector<Material>* materials, std::vector<Item>* items)
        : file_(file), cacheDir_(cacheDir), scene_(*scene), materials_(*materials), items_(*items) {}

    bool Load(const std::string& bytes, std::string* error) {
        std::string text;
        if (bytes.size() >= 12 && std::memcmp(bytes.data(), "glTF", 4) == 0) {
            size_t at = 12;
            while (at + 8 <= bytes.size()) {
                uint32_t length = 0, type = 0;
                std::memcpy(&length, bytes.data() + at, 4);
                std::memcpy(&type, bytes.data() + at + 4, 4);
                if (at + 8 + length > bytes.size()) break;
                if (type == 0x4E4F534A) text.assign(bytes.data() + at + 8, length);          // JSON
                else if (type == 0x004E4942) bin_.assign(bytes.data() + at + 8, length);     // BIN
                at += 8 + ((length + 3) & ~3u);
            }
            if (text.empty()) return Fail(error, "the .glb is cut short or damaged");
        } else {
            text = bytes;
        }
        std::string jsonError;
        if (!json::Parse(text, root_, jsonError)) return Fail(error, "not glTF (" + jsonError + ")");
        const json::Value* asset = root_.Get("asset");
        if (!asset || asset->Str("version").rfind("2", 0) != 0) return Fail(error, "only glTF 2.0 is read");
        if (const json::Value* required = root_.Get("extensionsRequired"); required && !required->items.empty())
            return Fail(error, "needs the extension " + required->items[0].string + " (export without compression)");
        if (!Buffers(error) || !Materials() || !Nodes(error) || !Skins() || !Meshes(error) || !Animations()) return false;
        if (items_.empty()) return Fail(error, "no triangles");
        return true;
    }

private:
    static bool Fail(std::string* error, const std::string& why) {
        *error = why;
        return false;
    }

    const json::Value& Array(const char* key) {
        static const json::Value empty;
        const json::Value* v = root_.Get(key);
        return v && v->type == json::Value::Array ? *v : empty;
    }

    bool Buffers(std::string* error) {
        for (const auto& b : Array("buffers").items) {
            const std::string uri = b.Str("uri");
            if (uri.empty()) {
                buffers_.push_back(bin_);
            } else if (uri.rfind("data:", 0) == 0) {
                buffers_.push_back(Base64(uri.substr(uri.find(',') + 1)));
            } else {
                std::string data;
                if (!ReadWhole(Beside(file_, uri), &data)) return Fail(error, "its buffer " + uri + " is missing");
                buffers_.push_back(std::move(data));
            }
        }
        return true;
    }

    // The bytes of a buffer view: pointer and length, or null.
    const char* View(int index, size_t* length, size_t* stride) {
        const auto& views = Array("bufferViews").items;
        if (index < 0 || index >= static_cast<int>(views.size())) return nullptr;
        const json::Value& v = views[static_cast<size_t>(index)];
        const int buffer = Int(v, "buffer");
        if (buffer < 0 || buffer >= static_cast<int>(buffers_.size())) return nullptr;
        const size_t offset = static_cast<size_t>(Num(v.Get("byteOffset"), 0)), size = static_cast<size_t>(Num(v.Get("byteLength"), 0));
        if (offset + size > buffers_[static_cast<size_t>(buffer)].size()) return nullptr;
        *length = size;
        *stride = static_cast<size_t>(Num(v.Get("byteStride"), 0));
        return buffers_[static_cast<size_t>(buffer)].data() + offset;
    }

    // An accessor's values as floats (normalized integers scaled to 0..1 or -1..1), `components` per element.
    bool Floats(int index, std::vector<float>* out, int* components) {
        out->clear();
        const auto& accessors = Array("accessors").items;
        if (index < 0 || index >= static_cast<int>(accessors.size())) return false;
        const json::Value& a = accessors[static_cast<size_t>(index)];
        const std::string type = a.Str("type");
        const int n = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
        const int component = Int(a, "componentType", 0);
        const size_t count = static_cast<size_t>(Num(a.Get("count"), 0));
        const bool normalized = a.Get("normalized") && a.Get("normalized")->boolean;
        const int bytes = component == 5126 || component == 5125 ? 4 : component == 5123 || component == 5122 ? 2 : 1;
        if (n == 0 || a.Get("sparse")) return false;
        *components = n;
        out->assign(count * static_cast<size_t>(n), 0.0f);
        if (!a.Get("bufferView")) return true;                      // all zeros
        size_t length = 0, stride = 0;
        const char* base = View(Int(a, "bufferView"), &length, &stride);
        if (!base) return false;
        const size_t offset = static_cast<size_t>(Num(a.Get("byteOffset"), 0));
        if (stride == 0) stride = static_cast<size_t>(n * bytes);
        if (count > 0 && offset + (count - 1) * stride + static_cast<size_t>(n * bytes) > length) return false;
        for (size_t i = 0; i < count; ++i)
            for (int c = 0; c < n; ++c) {
                const char* p = base + offset + i * stride + static_cast<size_t>(c * bytes);
                double v = 0;
                switch (component) {
                    case 5126: { float f; std::memcpy(&f, p, 4); v = f; break; }
                    case 5125: { uint32_t u; std::memcpy(&u, p, 4); v = u; break; }
                    case 5123: { uint16_t u; std::memcpy(&u, p, 2); v = normalized ? u / 65535.0 : u; break; }
                    case 5122: { int16_t s; std::memcpy(&s, p, 2); v = normalized ? std::max(s / 32767.0, -1.0) : s; break; }
                    case 5121: { uint8_t u = static_cast<uint8_t>(*p); v = normalized ? u / 255.0 : u; break; }
                    case 5120: { int8_t s = static_cast<int8_t>(*p); v = normalized ? std::max(s / 127.0, -1.0) : s; break; }
                    default: return false;
                }
                (*out)[i * static_cast<size_t>(n) + static_cast<size_t>(c)] = static_cast<float>(v);
            }
        return true;
    }

    // A texture's image as a file: one beside the model, or an embedded one written to the cache.
    std::wstring ImageFile(int texture) {
        const auto& textures = Array("textures").items;
        if (texture < 0 || texture >= static_cast<int>(textures.size())) return L"";
        const int source = Int(textures[static_cast<size_t>(texture)], "source");
        const auto& images = Array("images").items;
        if (source < 0 || source >= static_cast<int>(images.size())) return L"";
        const json::Value& image = images[static_cast<size_t>(source)];
        const std::string uri = image.Str("uri");
        std::string data, mime = image.Str("mimeType");
        if (!uri.empty() && uri.rfind("data:", 0) != 0) return Beside(file_, uri);
        if (!uri.empty()) {
            mime = uri.substr(5, uri.find(';') - 5);
            data = Base64(uri.substr(uri.find(',') + 1));
        } else {
            size_t length = 0, stride = 0;
            const char* p = View(Int(image, "bufferView"), &length, &stride);
            if (!p) return L"";
            data.assign(p, length);
        }
        if (data.empty() || cacheDir_.empty()) return L"";
        CreateDirectoryW(cacheDir_.c_str(), nullptr);
        const std::wstring out = cacheDir_ + L"\\" + HashName(data) + (mime == "image/jpeg" ? L".jpg" : L".png");
        if (GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES && !WriteWhole(out, data.data(), data.size())) return L"";
        return out;
    }

    bool Materials() {
        for (const auto& m : Array("materials").items) {
            Material mat;
            mat.name = m.Str("name");
            if (const json::Value* pbr = m.Get("pbrMetallicRoughness")) {
                if (const json::Value* f = pbr->Get("baseColorFactor"); f && f->items.size() == 4) {
                    mat.r = static_cast<float>(f->items[0].number);
                    mat.g = static_cast<float>(f->items[1].number);
                    mat.b = static_cast<float>(f->items[2].number);
                    mat.a = static_cast<float>(f->items[3].number);
                }
                mat.metallic = static_cast<float>(Num(pbr->Get("metallicFactor"), 1));
                mat.rough = static_cast<float>(Num(pbr->Get("roughnessFactor"), 1));
                if (const json::Value* t = pbr->Get("baseColorTexture")) mat.textureFile = ImageFile(Int(*t, "index"));
            }
            if (const json::Value* e = m.Get("emissiveFactor"); e && e->items.size() == 3) {
                double strength = 1;
                if (const json::Value* ext = m.Get("extensions"))
                    if (const json::Value* s = ext->Get("KHR_materials_emissive_strength")) strength = Num(s->Get("emissiveStrength"), 1);
                mat.er = static_cast<float>(e->items[0].number * strength);
                mat.eg = static_cast<float>(e->items[1].number * strength);
                mat.eb = static_cast<float>(e->items[2].number * strength);
            }
            if (const json::Value* ext = m.Get("extensions")) {
                if (const json::Value* t = ext->Get("KHR_materials_transmission"))
                    mat.transmission = static_cast<float>(Num(t->Get("transmissionFactor"), 0));
                if (const json::Value* i = ext->Get("KHR_materials_ior")) mat.ior = static_cast<float>(Num(i->Get("ior"), 1.5));
                if (const json::Value* s = ext->Get("KHR_materials_specular"))
                    mat.specular = static_cast<float>(Num(s->Get("specularFactor"), 1));
            }
            if (m.Str("alphaMode") != "BLEND") mat.a = 1;
            materials_.push_back(mat);
        }
        return true;
    }

    bool Nodes(std::string* error) {
        const auto& nodes = Array("nodes").items;
        scene_.nodes.resize(nodes.size());
        meshOf_.assign(nodes.size(), -1);
        for (size_t i = 0; i < nodes.size(); ++i) {
            const json::Value& n = nodes[i];
            Node& node = scene_.nodes[i];
            node.name = n.Str("name");
            if (const json::Value* m = n.Get("matrix"); m && m->items.size() == 16) {
                node.hasMatrix = true;
                for (int k = 0; k < 16; ++k) node.matrix[static_cast<size_t>(k)] = m->items[static_cast<size_t>(k)].number;
            }
            if (const json::Value* t = n.Get("translation"); t && t->items.size() == 3)
                for (int k = 0; k < 3; ++k) node.t[k] = t->items[static_cast<size_t>(k)].number;
            if (const json::Value* r = n.Get("rotation"); r && r->items.size() == 4)
                for (int k = 0; k < 4; ++k) node.r[k] = r->items[static_cast<size_t>(k)].number;
            if (const json::Value* s = n.Get("scale"); s && s->items.size() == 3)
                for (int k = 0; k < 3; ++k) node.s[k] = s->items[static_cast<size_t>(k)].number;
            node.skin = Int(n, "skin");
            meshOf_[i] = Int(n, "mesh");
            if (const json::Value* c = n.Get("children"))
                for (const auto& child : c->items) {
                    const int ci = static_cast<int>(child.number);
                    if (ci < 0 || ci >= static_cast<int>(nodes.size())) return Fail(error, "a node's child is out of range");
                    node.children.push_back(ci);
                    scene_.nodes[static_cast<size_t>(ci)].parent = static_cast<int>(i);
                }
        }
        const auto& scenes = Array("scenes").items;
        const int which = Int(root_, "scene", 0);
        if (which >= 0 && which < static_cast<int>(scenes.size())) {
            if (const json::Value* list = scenes[static_cast<size_t>(which)].Get("nodes"))
                for (const auto& r : list->items) scene_.roots.push_back(static_cast<int>(r.number));
        } else {
            for (size_t i = 0; i < scene_.nodes.size(); ++i)
                if (scene_.nodes[i].parent < 0) scene_.roots.push_back(static_cast<int>(i));
        }
        return true;
    }

    bool Skins() {
        for (const auto& s : Array("skins").items) {
            Skin skin;
            if (const json::Value* j = s.Get("joints"))
                for (const auto& v : j->items) skin.joints.push_back(static_cast<int>(v.number));
            std::vector<float> m;
            int n = 0;
            if (s.Get("inverseBindMatrices") && Floats(Int(s, "inverseBindMatrices"), &m, &n) && n == 16) {
                for (size_t k = 0; k + 16 <= m.size(); k += 16) {
                    Mat4 mat;
                    for (size_t e = 0; e < 16; ++e) mat[e] = m[k + e];
                    skin.inverseBind.push_back(mat);
                }
            }
            skin.inverseBind.resize(skin.joints.size(), Identity());
            scene_.skins.push_back(std::move(skin));
        }
        return true;
    }

    int DefaultMaterial() {
        if (defaultMaterial_ < 0) {
            defaultMaterial_ = static_cast<int>(materials_.size());
            Material m;
            m.name = "default";
            materials_.push_back(m);
        }
        return defaultMaterial_;
    }

    // Every mesh node in the scene: each of its triangle primitives is an item.
    bool Meshes(std::string* error) {
        const auto& meshes = Array("meshes").items;
        std::function<void(int)> visit = [&](int ni) {
            if (ni < 0 || ni >= static_cast<int>(scene_.nodes.size())) return;
            const int mi = meshOf_[static_cast<size_t>(ni)];
            if (mi >= 0 && mi < static_cast<int>(meshes.size()))
                if (const json::Value* prims = meshes[static_cast<size_t>(mi)].Get("primitives"))
                    for (const auto& p : prims->items) AddPrimitive(p, ni);
            for (int c : scene_.nodes[static_cast<size_t>(ni)].children) visit(c);
        };
        for (int r : scene_.roots) visit(r);
        if (!primitiveError_.empty()) return Fail(error, primitiveError_);
        return true;
    }

    void AddPrimitive(const json::Value& p, int node) {
        if (Int(p, "mode", 4) != 4) return;                        // points and lines: nothing to draw
        const json::Value* attributes = p.Get("attributes");
        if (!attributes || !attributes->Get("POSITION")) return;
        Source src;
        Item item;
        int n = 0;
        if (!Floats(Int(*attributes, "POSITION"), &src.positions, &n) || n != 3) {
            primitiveError_ = "a mesh's positions could not be read";
            return;
        }
        const size_t vertices = src.positions.size() / 3;
        if (attributes->Get("NORMAL") && (!Floats(Int(*attributes, "NORMAL"), &src.normals, &n) || n != 3)) src.normals.clear();
        if (attributes->Get("TEXCOORD_0") && Floats(Int(*attributes, "TEXCOORD_0"), &item.uv, &n) && n == 2) {
        } else {
            item.uv.assign(vertices * 2, 0.0f);
        }
        std::vector<float> indices;
        if (p.Get("indices")) {
            if (!Floats(Int(p, "indices"), &indices, &n) || n != 1) {
                primitiveError_ = "a mesh's triangles could not be read";
                return;
            }
            for (float f : indices) item.indices.push_back(static_cast<uint32_t>(f));
        } else {
            for (size_t i = 0; i < vertices; ++i) item.indices.push_back(static_cast<uint32_t>(i));
        }
        item.indices.resize(item.indices.size() / 3 * 3);
        for (uint32_t i : item.indices)
            if (i >= vertices) {
                primitiveError_ = "a mesh has a triangle out of range";
                return;
            }
        const int skin = scene_.nodes[static_cast<size_t>(node)].skin;
        if (skin >= 0 && skin < static_cast<int>(scene_.skins.size()) && attributes->Get("JOINTS_0") && attributes->Get("WEIGHTS_0")) {
            std::vector<float> joints;
            if (Floats(Int(*attributes, "JOINTS_0"), &joints, &n) && n == 4 && Floats(Int(*attributes, "WEIGHTS_0"), &src.weights, &n) && n == 4) {
                src.skin = skin;
                for (float j : joints) src.joints.push_back(static_cast<uint16_t>(j));
            }
        }
        const int material = Int(p, "material");
        item.material = material >= 0 && material < static_cast<int>(materials_.size()) ? material : DefaultMaterial();
        item.vertices = vertices;
        src.node = node;
        if (src.normals.size() != src.positions.size()) SmoothNormals(item, &src);
        scene_.sources.push_back(std::move(src));
        items_.push_back(std::move(item));
    }

    bool Animations() {
        for (const auto& a : Array("animations").items) {
            Animation anim;
            anim.name = a.Str("name");
            const json::Value* samplers = a.Get("samplers");
            const json::Value* channels = a.Get("channels");
            if (!samplers || !channels) continue;
            for (const auto& c : channels->items) {
                const json::Value* target = c.Get("target");
                const int s = Int(c, "sampler");
                if (!target || s < 0 || s >= static_cast<int>(samplers->items.size())) continue;
                const std::string path = target->Str("path");
                Channel ch;
                ch.node = Int(*target, "node");
                ch.path = path == "translation" ? 0 : path == "rotation" ? 1 : path == "scale" ? 2 : -1;
                if (ch.path < 0 || ch.node < 0 || ch.node >= static_cast<int>(scene_.nodes.size())) continue;   // morph weights: not read
                const json::Value& sampler = samplers->items[static_cast<size_t>(s)];
                const std::string interp = sampler.Str("interpolation", "LINEAR");
                ch.interpolation = interp == "STEP" ? 1 : interp == "CUBICSPLINE" ? 2 : 0;
                int n = 0;
                if (!Floats(Int(sampler, "input"), &ch.times, &n) || n != 1 || !Floats(Int(sampler, "output"), &ch.values, &n)) continue;
                if (ch.times.empty()) continue;
                anim.length = std::max(anim.length, static_cast<double>(ch.times.back()));
                anim.channels.push_back(std::move(ch));
            }
            if (!anim.channels.empty()) scene_.animations.push_back(std::move(anim));
        }
        return true;
    }

    std::wstring file_, cacheDir_;
    Scene& scene_;
    std::vector<Material>& materials_;
    std::vector<Item>& items_;
    json::Value root_;
    std::string bin_;
    std::vector<std::string> buffers_;
    std::vector<int> meshOf_;
    int defaultMaterial_ = -1;
    std::string primitiveError_;

public:
    // Normals from the triangles, shared at shared vertices (smooth).
    static void SmoothNormals(const Item& item, Source* src) {
        src->normals.assign(src->positions.size(), 0.0f);
        const auto& p = src->positions;
        for (size_t t = 0; t + 2 < item.indices.size(); t += 3) {
            const uint32_t a = item.indices[t], b = item.indices[t + 1], c = item.indices[t + 2];
            const double ux = p[b * 3] - p[a * 3], uy = p[b * 3 + 1] - p[a * 3 + 1], uz = p[b * 3 + 2] - p[a * 3 + 2];
            const double vx = p[c * 3] - p[a * 3], vy = p[c * 3 + 1] - p[a * 3 + 1], vz = p[c * 3 + 2] - p[a * 3 + 2];
            const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            for (uint32_t v : {a, b, c}) {
                src->normals[v * 3] += static_cast<float>(nx);
                src->normals[v * 3 + 1] += static_cast<float>(ny);
                src->normals[v * 3 + 2] += static_cast<float>(nz);
            }
        }
        for (size_t v = 0; v + 2 < src->normals.size(); v += 3) {
            const double len = std::sqrt(src->normals[v] * src->normals[v] + src->normals[v + 1] * src->normals[v + 1] + src->normals[v + 2] * src->normals[v + 2]);
            if (len > 0)
                for (size_t k = 0; k < 3; ++k) src->normals[v + k] = static_cast<float>(src->normals[v + k] / len);
        }
    }
};

// ------------------------------------------------------------------------------------------------ OBJ
bool LoadMtl(const std::wstring& file, std::vector<Material>* materials) {
    std::string text;
    if (!ReadWhole(file, &text)) return false;
    std::stringstream lines(text);
    std::string line;
    Material* m = nullptr;
    while (std::getline(lines, line)) {
        std::stringstream words(line);
        std::string key;
        words >> key;
        if (key == "newmtl") {
            materials->push_back(Material{});
            m = &materials->back();
            std::getline(words >> std::ws, m->name);
            while (!m->name.empty() && (m->name.back() == '\r' || m->name.back() == ' ')) m->name.pop_back();
        } else if (!m) {
            continue;
        } else if (key == "Kd") {
            words >> m->r >> m->g >> m->b;
        } else if (key == "Ke") {
            words >> m->er >> m->eg >> m->eb;
        } else if (key == "d") {
            words >> m->a;
        } else if (key == "Tr") {
            float tr = 0;
            words >> tr;
            m->a = 1 - tr;
        } else if (key == "Ns") {
            float ns = 0;
            words >> ns;                    // Blender writes Ns = (1 - roughness)^2 * 1000
            m->rough = std::clamp(1.0f - std::sqrt(std::max(ns, 0.0f) / 1000.0f), 0.0f, 1.0f);
        } else if (key == "Pr") {
            words >> m->rough;
        } else if (key == "Pm") {
            words >> m->metallic;
        } else if (key == "map_Kd") {
            // Options first ("-bm 1", "-o 0 0 0", "-clamp on", ...), then the file name, which may have spaces.
            std::string rest;
            std::getline(words >> std::ws, rest);
            while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
            while (!rest.empty() && rest[0] == '-') {
                size_t at = rest.find(' ');
                while (at != std::string::npos) {                   // the option's values: numbers, on/off
                    const size_t next = rest.find_first_not_of(' ', at);
                    if (next == std::string::npos) break;
                    const size_t end = rest.find(' ', next);
                    const std::string word = rest.substr(next, end == std::string::npos ? std::string::npos : end - next);
                    const bool value = word == "on" || word == "off" || word.find_first_not_of("0123456789.-+eE") == std::string::npos;
                    if (!value || end == std::string::npos) {
                        at = next;
                        break;
                    }
                    at = end;
                }
                rest = at == std::string::npos ? "" : rest.substr(at);
                const size_t start = rest.find_first_not_of(' ');
                rest = start == std::string::npos ? "" : rest.substr(start);
            }
            if (!rest.empty()) m->textureFile = rest.size() > 1 && rest[1] == ':' ? Widen(rest) : Beside(file, rest);
        }
    }
    return true;
}

bool LoadObj(const std::wstring& file, const std::string& text, Scene* scene, std::vector<Material>* materials, std::vector<Item>* items,
             std::string* error) {
    std::vector<float> v, vt, vn;
    std::map<std::string, int> materialIndex;
    struct Building {
        Item item;
        Source src;
        std::map<std::string, uint32_t> vertexOf;
    };
    std::map<int, Building> byMaterial;
    int current = -1;
    auto materialNamed = [&](const std::string& name) {
        for (size_t i = 0; i < materials->size(); ++i)
            if ((*materials)[i].name == name) return static_cast<int>(i);
        Material m;
        m.name = name;
        materials->push_back(m);
        return static_cast<int>(materials->size() - 1);
    };
    std::stringstream lines(text);
    std::string line;
    int number = 0;
    while (std::getline(lines, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::stringstream words(line);
        std::string key;
        words >> key;
        if (key == "v") {
            float x = 0, y = 0, z = 0;
            words >> x >> y >> z;
            v.insert(v.end(), {x, y, z});
        } else if (key == "vt") {
            float s = 0, t = 0;
            words >> s >> t;
            vt.insert(vt.end(), {s, 1 - t});                  // OBJ's v runs up the image, the game's down
        } else if (key == "vn") {
            float x = 0, y = 0, z = 0;
            words >> x >> y >> z;
            vn.insert(vn.end(), {x, y, z});
        } else if (key == "mtllib") {
            std::string name;
            std::getline(words >> std::ws, name);
            LoadMtl(Beside(file, name), materials);
        } else if (key == "usemtl") {
            std::string name;
            std::getline(words >> std::ws, name);
            current = materialNamed(name);
        } else if (key == "f") {
            if (current < 0) current = materialNamed("default");
            Building& b = byMaterial[current];
            b.item.material = current;
            std::vector<uint32_t> corners;
            std::string corner;
            while (words >> corner) {
                auto it = b.vertexOf.find(corner);
                if (it == b.vertexOf.end()) {
                    int idx[3] = {0, 0, 0};
                    std::stringstream parts(corner);
                    std::string part;
                    for (int k = 0; k < 3 && std::getline(parts, part, '/'); ++k) idx[k] = part.empty() ? 0 : std::atoi(part.c_str());
                    auto resolve = [](int i, size_t count) { return i < 0 ? static_cast<long long>(count) + i : static_cast<long long>(i) - 1; };
                    const long long pi = resolve(idx[0], v.size() / 3), ti = resolve(idx[1], vt.size() / 2), ni = resolve(idx[2], vn.size() / 3);
                    if (pi < 0 || pi >= static_cast<long long>(v.size() / 3)) {
                        *error = "line " + std::to_string(number) + ": a face uses a vertex that isn't there";
                        return false;
                    }
                    const uint32_t id = static_cast<uint32_t>(b.src.positions.size() / 3);
                    for (int k = 0; k < 3; ++k) b.src.positions.push_back(v[static_cast<size_t>(pi) * 3 + static_cast<size_t>(k)]);
                    const bool hasT = idx[1] != 0 && ti >= 0 && ti < static_cast<long long>(vt.size() / 2);
                    b.item.uv.push_back(hasT ? vt[static_cast<size_t>(ti) * 2] : 0.0f);
                    b.item.uv.push_back(hasT ? vt[static_cast<size_t>(ti) * 2 + 1] : 0.0f);
                    const bool hasN = idx[2] != 0 && ni >= 0 && ni < static_cast<long long>(vn.size() / 3);
                    for (int k = 0; k < 3; ++k) b.src.normals.push_back(hasN ? vn[static_cast<size_t>(ni) * 3 + static_cast<size_t>(k)] : 0.0f);
                    it = b.vertexOf.emplace(corner, id).first;
                }
                corners.push_back(it->second);
            }
            for (size_t k = 1; k + 1 < corners.size(); ++k)             // a fan: faces of 4 and more sides
                b.item.indices.insert(b.item.indices.end(), {corners[0], corners[k], corners[k + 1]});
        }
    }
    for (auto& [material, b] : byMaterial) {
        if (b.item.indices.empty()) continue;
        b.item.vertices = b.src.positions.size() / 3;
        bool anyNormal = false;
        for (float f : b.src.normals) anyNormal |= f != 0;
        if (!anyNormal) Gltf::SmoothNormals(b.item, &b.src);
        scene->sources.push_back(std::move(b.src));
        items->push_back(std::move(b.item));
    }
    if (items->empty()) {
        *error = "no faces";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ posing
void Sample(const Channel& ch, double t, double* out) {
    const int n = ch.path == 1 ? 4 : 3;
    const size_t keys = ch.times.size();
    const size_t stride = ch.interpolation == 2 ? 3 : 1;           // cubic spline: in-tangent, value, out-tangent
    auto value = [&](size_t k, int c) { return static_cast<double>(ch.values[(k * stride + (stride == 3 ? 1 : 0)) * static_cast<size_t>(n) + static_cast<size_t>(c)]); };
    if (ch.values.size() < keys * stride * static_cast<size_t>(n)) return;
    if (t <= ch.times.front() || keys == 1) {
        for (int c = 0; c < n; ++c) out[c] = value(0, c);
        return;
    }
    if (t >= ch.times.back()) {
        for (int c = 0; c < n; ++c) out[c] = value(keys - 1, c);
        return;
    }
    const size_t k = static_cast<size_t>(std::upper_bound(ch.times.begin(), ch.times.end(), static_cast<float>(t)) - ch.times.begin()) - 1;
    const double t0 = ch.times[k], t1 = ch.times[k + 1], span = t1 - t0, u = span > 0 ? (t - t0) / span : 0;
    if (ch.interpolation == 1) {
        for (int c = 0; c < n; ++c) out[c] = value(k, c);
        return;
    }
    if (ch.interpolation == 2) {
        const double u2 = u * u, u3 = u2 * u;
        const double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        for (int c = 0; c < n; ++c) {
            const double outTangent = ch.values[(k * 3 + 2) * static_cast<size_t>(n) + static_cast<size_t>(c)];
            const double inTangent = ch.values[((k + 1) * 3) * static_cast<size_t>(n) + static_cast<size_t>(c)];
            out[c] = h00 * value(k, c) + h10 * span * outTangent + h01 * value(k + 1, c) + h11 * span * inTangent;
        }
        if (n == 4) {
            const double len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
            for (int c = 0; c < 4 && len > 0; ++c) out[c] /= len;
        }
        return;
    }
    if (n == 4) {
        float a[4], b[4];
        for (int c = 0; c < 4; ++c) {
            a[c] = static_cast<float>(value(k, c));
            b[c] = static_cast<float>(value(k + 1, c));
        }
        Slerp(a, b, u, out);
        return;
    }
    for (int c = 0; c < n; ++c) out[c] = value(k, c) * (1 - u) + value(k + 1, c) * u;
}

void ToGame(const double* in, float* out, double scale) {
    out[0] = static_cast<float>(in[2] * scale);
    out[1] = static_cast<float>(-in[0] * scale);
    out[2] = static_cast<float>(in[1] * scale);
}

}  // namespace

// ------------------------------------------------------------------------------------------------ Model
Model::Model() : scene_(new Scene) {}
Model::~Model() { delete scene_; }
Model::Model(Model&& o) noexcept : scene_(o.scene_), materials_(std::move(o.materials_)), items_(std::move(o.items_)) { o.scene_ = nullptr; }
Model& Model::operator=(Model&& o) noexcept {
    if (this != &o) {
        delete scene_;
        scene_ = o.scene_;
        o.scene_ = nullptr;
        materials_ = std::move(o.materials_);
        items_ = std::move(o.items_);
    }
    return *this;
}

bool Model::Load(const std::wstring& file, const std::wstring& cacheDir, std::string* error) {
    delete scene_;
    scene_ = new Scene;
    materials_.clear();
    items_.clear();
    std::string bytes;
    if (!ReadWhole(file, &bytes)) {
        *error = "the file could not be read";
        return false;
    }
    std::wstring lower = file;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    auto ends = [&](const wchar_t* ext) { return lower.size() >= wcslen(ext) && lower.compare(lower.size() - wcslen(ext), std::wstring::npos, ext) == 0; };
    if (ends(L".obj")) return LoadObj(file, bytes, scene_, &materials_, &items_, error);
    if (ends(L".glb") || ends(L".gltf")) {
        Gltf gltf(file, cacheDir, scene_, &materials_, &items_);
        return gltf.Load(bytes, error);
    }
    *error = "not a model file this reads (.glb, .gltf or .obj; FBX: export glTF from Blender instead)";
    return false;
}

int Model::FindNode(const std::string& name) const {
    for (size_t i = 0; i < scene_->nodes.size(); ++i)
        if (scene_->nodes[i].name == name) return static_cast<int>(i);
    return -1;
}

std::vector<std::string> Model::Nodes() const {
    std::vector<std::string> names;
    for (const auto& n : scene_->nodes)
        if (!n.name.empty()) names.push_back(n.name);
    return names;
}

bool Model::ItemUnder(size_t item, int node) const {
    if (item >= scene_->sources.size()) return false;
    for (int n = scene_->sources[item].node; n >= 0; n = scene_->nodes[static_cast<size_t>(n)].parent)
        if (n == node) return true;
    return false;
}

std::vector<std::string> Model::Animations() const {
    std::vector<std::string> names;
    for (const auto& a : scene_->animations) names.push_back(a.name);
    return names;
}

int Model::FindAnimation(const std::string& fragment) const {
    const std::string f = Lower(fragment);
    if (f.empty()) return -1;
    for (size_t i = 0; i < scene_->animations.size(); ++i)
        if (Lower(scene_->animations[i].name) == f) return static_cast<int>(i);
    for (size_t i = 0; i < scene_->animations.size(); ++i)
        if (Lower(scene_->animations[i].name).find(f) != std::string::npos) return static_cast<int>(i);
    return -1;
}

double Model::Length(int animation) const {
    return animation >= 0 && animation < static_cast<int>(scene_->animations.size()) ? scene_->animations[static_cast<size_t>(animation)].length : 0;
}

Pose Model::At(int animation, double seconds) const {
    const Scene& s = *scene_;
    // Each node's own transform: the file's, or the animation's at this moment.
    std::vector<Node> nodes = s.nodes;
    std::vector<bool> animated(nodes.size(), false);
    if (animation >= 0 && animation < static_cast<int>(s.animations.size()))
        for (const auto& ch : s.animations[static_cast<size_t>(animation)].channels) {
            Node& n = nodes[static_cast<size_t>(ch.node)];
            Sample(ch, seconds, ch.path == 0 ? n.t : ch.path == 1 ? n.r : n.s);
            animated[static_cast<size_t>(ch.node)] = true;
        }
    std::vector<Mat4> world(nodes.size(), Identity());
    std::vector<bool> done(nodes.size(), false);
    std::function<const Mat4&(int)> worldOf = [&](int i) -> const Mat4& {
        const size_t u = static_cast<size_t>(i);
        if (done[u]) return world[u];
        done[u] = true;
        const Node& n = nodes[u];
        const Mat4 local = n.hasMatrix && !animated[u] ? n.matrix : FromTRS(n.t, n.r, n.s);
        world[u] = n.parent >= 0 ? Mul(worldOf(n.parent), local) : local;
        return world[u];
    };
    Pose pose;
    pose.positions.resize(items_.size());
    pose.normals.resize(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
        const Source& src = s.sources[i];
        const size_t count = src.positions.size() / 3;
        auto& outP = pose.positions[i];
        auto& outN = pose.normals[i];
        outP.resize(count * 3);
        outN.resize(count * 3);
        std::vector<Mat4> joints;
        if (src.skin >= 0) {
            const Skin& skin = s.skins[static_cast<size_t>(src.skin)];
            for (size_t j = 0; j < skin.joints.size(); ++j) joints.push_back(Mul(worldOf(skin.joints[j]), skin.inverseBind[j]));
        }
        const Mat4 rigid = src.node >= 0 ? worldOf(src.node) : Identity();
        for (size_t v = 0; v < count; ++v) {
            double p[3] = {0, 0, 0}, n[3] = {0, 0, 0};
            if (!joints.empty()) {
                double total = 0;
                for (int k = 0; k < 4; ++k) total += src.weights[v * 4 + static_cast<size_t>(k)];
                for (int k = 0; k < 4; ++k) {
                    const double w = total > 0 ? src.weights[v * 4 + static_cast<size_t>(k)] / total : (k == 0 ? 1 : 0);
                    const size_t j = src.joints[v * 4 + static_cast<size_t>(k)];
                    if (w == 0 || j >= joints.size()) continue;
                    double pj[3], nj[3];
                    Point(joints[j], &src.positions[v * 3], pj);
                    Direction(joints[j], &src.normals[v * 3], nj);
                    for (int c = 0; c < 3; ++c) {
                        p[c] += w * pj[c];
                        n[c] += w * nj[c];
                    }
                }
            } else {
                Point(rigid, &src.positions[v * 3], p);
                Direction(rigid, &src.normals[v * 3], n);
            }
            const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 0)
                for (double& c : n) c /= len;
            ToGame(p, &outP[v * 3], 100);
            ToGame(n, &outN[v * 3], 1);
        }
    }
    return pose;
}

}  // namespace meshfile
