#include "draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "cosmetics.hpp"

#include "game.hpp"
#include "log.hpp"
#include "models.hpp"

namespace draw {
namespace {

using eng::Obj;

struct Shape {
    int owner = -1;
    eng::Weak actor;
    eng::Weak material;                 // its dynamic material, once Glow has found it
    std::shared_ptr<models::Model> model;   // a model's (Model): what it is, and its parts built on the actor
    std::shared_ptr<models::Built> built;
    bool tube = false;                  // made by Tube (Retube sweeps only these again)
};
std::map<int, Shape> gShapes;
int gNextId = 1;
int gGeneration = -1;

int gCameraOwner = -1;
eng::Weak gCamera;

// Custom (.wav) sounds playing, and when their samples run out. Their procedural sound waves play on, silent, after the
// samples (measured: still playing 6 s into a 2.1 s file), so each is stopped then (and, auto-destroyed, goes away).
struct Playing {
    eng::Weak component;
    double end;
};
std::vector<Playing> gPlaying;

struct Vec3 {
    double x, y, z;
};
struct Rot {
    double pitch, yaw, roll;
};

Obj Actor(int owner, int id) {
    auto it = gShapes.find(id);
    return it != gShapes.end() && it->second.owner == owner ? eng::Get(it->second.actor) : nullptr;
}

int Keep(int owner, Obj actor) {
    if (!actor) return 0;
    const int id = gNextId++;
    gShapes[id] = {owner, eng::MakeWeak(actor), {}, nullptr, nullptr, false};
    return id;
}

// A shape's actor and a model's parts go together.
void Destroy(Shape& shape) {
    if (shape.built) models::Destroy(*shape.built);
    if (Obj a = eng::Get(shape.actor)) eng::Call(a, "K2_DestroyActor");
}

struct Vec3d {
    double x, y, z;
};

Obj Pawn() {
    Obj controller = game::PlayerController();
    return controller ? eng::Call(controller, "K2_GetPawn").ReturnObj() : nullptr;
}

void ViewThrough(Obj target) {
    Obj controller = game::PlayerController();
    if (!controller || !target) return;
    eng::Params p(eng::FunctionOn(controller, "SetViewTargetWithBlend"));
    p.Set("NewViewTarget", target);
    p.Set("BlendTime", 0.0f);
    eng::Invoke(controller, p);
}

}  // namespace

void Frame() {
    const double now = game::Seconds();
    for (size_t i = 0; i < gPlaying.size();) {
        Obj c = eng::Get(gPlaying[i].component);
        if (c && now < gPlaying[i].end) {
            i++;
            continue;
        }
        if (c) eng::Call(c, "Stop");
        gPlaying.erase(gPlaying.begin() + static_cast<std::ptrdiff_t>(i));
    }
    // Models' spinning groups and animations (no ball: as still).
    for (auto& [id, shape] : gShapes)
        if (shape.built && shape.model && eng::Get(shape.actor)) models::Animate(*shape.model, *shape.built, nullptr, game::Seconds());
    if (game::Generation() == gGeneration) return;
    gGeneration = game::Generation();
    gShapes.clear();                    // their actors went with the old map
    gCamera = {};
    gCameraOwner = -1;
}

void RemoveOwner(int owner) {
    Clear(owner);
    ReleaseCamera(owner);
}

// Shapes are for looking at: nothing collides with them.
Obj NoCollision(Obj actor) {
    if (Obj mesh = actor ? eng::ReadObj(actor, "DynamicMeshComponent") : nullptr) eng::Call(mesh, "SetCollisionEnabled", uint8_t{0});
    return actor;
}

int Tube(int owner, const std::vector<std::array<double, 3>>& path, double radius, float r, float g, float b, bool glow, float opacity) {
    const int id = Keep(owner, NoCollision(models::SpawnTube(path, radius, {r, g, b, glow, 8, opacity})));
    if (id) gShapes[id].tube = true;
    return id;
}

int Segments(int owner, const std::vector<std::array<double, 3>>& pairs, double radius, float r, float g, float b, bool glow, float opacity) {
    if (pairs.size() < 2) return 0;
    Obj mesh = models::SpawnMesh({r, g, b, glow && opacity >= 1, 8, opacity});
    if (!mesh) return 0;
    for (size_t k = 0; k + 1 < pairs.size(); k += 2) {
        const auto& a = pairs[k];
        const auto& c = pairs[k + 1];
        const double dx = c[0] - a[0], dy = c[1] - a[1], dz = c[2] - a[2];
        if (dx * dx + dy * dy + dz * dz < 4) continue;          // (a tube needs some length)
        models::AppendTube(mesh, {a, c}, radius, 6);
    }
    return Keep(owner, NoCollision(mesh));
}

bool Retube(int owner, int id, const std::vector<std::array<double, 3>>& path, double radius) {
    auto it = gShapes.find(id);
    if (it == gShapes.end() || it->second.owner != owner || !it->second.tube) return false;
    Obj actor = eng::Get(it->second.actor);
    Obj component = actor ? eng::ReadObj(actor, "DynamicMeshComponent") : nullptr;
    Obj mesh = component ? eng::Call(component, "GetDynamicMesh").ReturnObj() : nullptr;
    if (!mesh) return false;
    if (!eng::Call(mesh, "Reset").Invoked()) return false;
    // (an empty mesh, or the old tube would stay under the new one, growing every call)
    if (eng::Call(mesh, "GetTriangleCount").ReturnAs<int32_t>(-1) != 0) return false;
    if (path.size() >= 2) models::AppendTube(actor, path, radius, 6);   // (false: too short to sweep, left empty)
    return true;
}

int Ball(int owner, double radius, float r, float g, float b, bool glow) {
    return Keep(owner, NoCollision(models::SpawnBall(radius, {r, g, b, glow, 8})));
}

bool Move(int owner, int id, double x, double y, double z) {
    Obj a = Actor(owner, id);
    if (!a) return false;
    eng::Params p(eng::FunctionOn(a, "K2_SetActorLocation"));
    p.Set("NewLocation", Vec3{x, y, z});
    p.Set("bSweep", uint8_t{0});
    p.Set("bTeleport", uint8_t{1});
    return eng::Invoke(a, p);
}

bool Glow(int owner, int id, float r, float g, float b, float bright) {
    auto it = gShapes.find(id);
    if (it == gShapes.end() || it->second.owner != owner) return false;
    Obj material = eng::Get(it->second.material);
    if (!material) {
        material = models::GlowMaterial(eng::Get(it->second.actor));
        it->second.material = eng::MakeWeak(material);
    }
    return material && models::SetGlow(material, r, g, b, bright);
}

bool Fade(int owner, int id, float opacity) {
    auto it = gShapes.find(id);
    if (it == gShapes.end() || it->second.owner != owner) return false;
    Obj material = eng::Get(it->second.material);
    if (!material) {
        material = models::GlowMaterial(eng::Get(it->second.actor));      // the shape's dynamic material, glass or glow
        it->second.material = eng::MakeWeak(material);
    }
    return material && models::SetOpacity(material, opacity);
}

int Model(int owner, const std::string& text, std::string* error) {
    auto model = std::make_shared<models::Model>();
    if (!models::Parse(text, model.get(), error)) return 0;
    Obj holder = models::SpawnHolder();
    Obj root = holder ? eng::ReadObj(holder, "DynamicMeshComponent") : nullptr;
    if (!root) {
        *error = "the model's actor could not be made";
        return 0;
    }
    eng::Call(root, "SetCollisionEnabled", uint8_t{0});
    const int id = Keep(owner, holder);
    gShapes[id].model = model;
    gShapes[id].built = std::make_shared<models::Built>(models::Build(*model, root));
    return id;
}

bool Turn(int owner, int id, double pitch, double yaw, double roll) {
    Obj a = Actor(owner, id);
    if (!a) return false;
    eng::Params p(eng::FunctionOn(a, "K2_SetActorRotation"));
    p.Set("NewRotation", Rot{pitch, yaw, roll});
    p.Set("bTeleportPhysics", uint8_t{1});
    return eng::Invoke(a, p);
}

bool Scale(int owner, int id, double scale) {
    Obj a = Actor(owner, id);
    return a && eng::Call(a, "SetActorScale3D", Vec3d{scale, scale, scale}).Invoked();
}

bool Effect(const std::string& system, double x, double y, double z, double scale, double nx, double ny, double nz) {
    Obj asset = system.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(system));
    Obj controller = game::PlayerController();
    if (!asset || !controller) return false;
    // Up along the normal: pitch and roll from it (yaw left as it is).
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    Rot rot{0, 0, 0};
    if (len > 0) {
        rot.pitch = std::atan2(nx / len, nz / len) * 57.29577951308232;
        rot.roll = -std::atan2(ny / len, std::sqrt(nx * nx + nz * nz) / len) * 57.29577951308232;
    }
    Obj lib = eng::FindCdo("NiagaraFunctionLibrary");
    eng::Params p(eng::FunctionOn(lib, "SpawnSystemAtLocation"));
    p.Set("WorldContextObject", controller);
    p.Set("SystemTemplate", asset);
    p.Set("Location", Vec3d{x, y, z});
    p.Set("Rotation", rot);
    p.Set("Scale", Vec3d{scale, scale, scale});
    p.Set("bAutoDestroy", uint8_t{1});
    p.Set("bAutoActivate", uint8_t{1});
    return eng::Invoke(lib, p) && p.ReturnObj();
}

namespace {
// A .wav file's samples: 16-bit PCM, interleaved.
struct Wav {
    int32_t rate = 0, channels = 0;
    std::vector<uint8_t> pcm;
};
std::map<std::string, std::shared_ptr<Wav>> gWavs;     // by path, read once
eng::Weak gLastSound;                                   // the last custom sound's audio component (for measuring)

uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

// Reads a RIFF WAVE file of 16-bit PCM (mono or stereo, any rate). Null (and a log line) if it isn't one.
std::shared_ptr<Wav> ReadWav(const std::string& path) {
    if (auto it = gWavs.find(path); it != gWavs.end()) return it->second;
    std::shared_ptr<Wav> wav;
    std::vector<uint8_t> file;
    if (FILE* f = _wfopen(eng::Widen(path).c_str(), L"rb")) {
        uint8_t chunk[65536];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) file.insert(file.end(), chunk, chunk + n);
        std::fclose(f);
    }
    std::string why = file.empty() ? "can't be read" : "";
    if (why.empty() && (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) || std::memcmp(file.data() + 8, "WAVE", 4)))
        why = "isn't a WAVE file";
    auto out = std::make_shared<Wav>();
    int bits = 0, format = 0;
    for (size_t at = 12; why.empty() && at + 8 <= file.size();) {
        const uint32_t size = U32(&file[at + 4]);
        const uint8_t* body = &file[at + 8];
        if (at + 8 + size > file.size()) break;
        if (!std::memcmp(&file[at], "fmt ", 4) && size >= 16) {
            format = U16(body);
            out->channels = U16(body + 2);
            out->rate = static_cast<int32_t>(U32(body + 4));
            bits = U16(body + 14);
        } else if (!std::memcmp(&file[at], "data", 4)) {
            out->pcm.assign(body, body + size);
        }
        at += 8 + size + (size & 1);
    }
    if (why.empty() && (format != 1 || bits != 16)) why = "isn't 16-bit PCM (export it as 16-bit PCM WAV)";
    if (why.empty() && (out->channels < 1 || out->channels > 2 || out->rate < 8000 || out->pcm.empty())) why = "has no sound in it";
    if (why.empty()) wav = out;
    else hostlog::Warn("sound: " + path + " " + why);
    gWavs[path] = wav;
    return wav;
}

// Plays a plugin's own .wav through the game's audio: a procedural sound wave from Steam Integration Kit (in the game:
// SIK_UserLibrary.ConstructSIKSoundWaveProcedural) with the samples queued on it, then SpawnSound2D.
bool PlayWav(const std::string& path, double volume, double pitch) {
    const auto wav = ReadWav(path);
    Obj controller = game::PlayerController();
    Obj lib = eng::FindCdo("SIK_UserLibrary");
    if (!wav || !controller || !lib) return false;
    const int32_t frames = static_cast<int32_t>(wav->pcm.size() / (2 * wav->channels));
    eng::Params make(eng::FunctionOn(lib, "ConstructSIKSoundWaveProcedural"));
    make.Set("SampleRate", wav->rate);
    make.Set("NumChannels", wav->channels);
    make.Set("Duration", static_cast<float>(static_cast<double>(frames) / wav->rate));
    if (!eng::Invoke(lib, make)) return false;
    Obj wave = make.ReturnObj();
    if (!wave) return false;
    struct Array {
        const uint8_t* data;
        int32_t num, max;
    } samples{wav->pcm.data(), static_cast<int32_t>(wav->pcm.size()), static_cast<int32_t>(wav->pcm.size())};
    eng::Params queue(eng::FunctionOn(wave, "SIK_QueueAudio"));
    queue.Set("AudioData", samples);
    if (!eng::Invoke(wave, queue)) return false;
    Obj statics = eng::FindCdo("GameplayStatics");
    eng::Params p(eng::FunctionOn(statics, "SpawnSound2D"));
    p.Set("WorldContextObject", controller);
    p.Set("Sound", wave);
    p.Set("VolumeMultiplier", static_cast<float>(volume));
    p.Set("PitchMultiplier", static_cast<float>(pitch));
    p.Set("bAutoDestroy", uint8_t{1});
    if (!eng::Invoke(statics, p)) return false;
    Obj component = p.ReturnObj();
    if (!component) return false;
    gLastSound = eng::MakeWeak(component);
    const double seconds = static_cast<double>(frames) / wav->rate / std::max(pitch, 0.05);
    gPlaying.push_back({eng::MakeWeak(component), game::Seconds() + seconds + 0.1});
    return true;
}

bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != tail[i]) return false;
    return true;
}
}  // namespace

std::string LastSoundState() {
    Obj c = eng::Get(gLastSound);
    if (!c) return "no custom sound (or it finished and was destroyed)";
    const bool playing = eng::Call(c, "IsPlaying").ReturnAs<uint8_t>(0) != 0;
    const int state = eng::Call(c, "GetPlayState").ReturnAs<uint8_t>(255);
    return std::string("custom sound ") + (playing ? "playing" : "not playing") + ", play state " + std::to_string(state);
}

bool Sound(const std::string& sound, double volume, double pitch) {
    if (EndsWith(sound, ".wav")) return PlayWav(sound, volume, pitch);
    Obj asset = sound.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(sound));
    Obj controller = game::PlayerController();
    if (!asset || !controller) return false;
    Obj statics = eng::FindCdo("GameplayStatics");
    eng::Params p(eng::FunctionOn(statics, "PlaySound2D"));
    p.Set("WorldContextObject", controller);
    p.Set("Sound", asset);
    p.Set("VolumeMultiplier", static_cast<float>(volume));
    p.Set("PitchMultiplier", static_cast<float>(pitch));
    return eng::Invoke(statics, p);
}

bool Shake(double scale) {
    static eng::Weak shakeClass;
    Obj cls = eng::Get(shakeClass);
    if (!cls) {
        cls = cosmetics::LoadAsset(L"/Game/Art/VFX/CameraShake/Shake_BallestCam.Shake_BallestCam_C");
        if (cls) cosmetics::KeepAlive(cls);
        shakeClass = eng::MakeWeak(cls);
    }
    Obj controller = game::PlayerController();
    Obj manager = controller ? eng::ReadObj(controller, "PlayerCameraManager") : nullptr;
    if (!cls || !manager) return false;
    eng::Params p(eng::FunctionOn(manager, "StartCameraShake"));
    p.Set("ShakeClass", cls);
    p.Set("Scale", static_cast<float>(scale));
    return eng::Invoke(manager, p);
}

int Adopt(int owner, Obj actor) { return Keep(owner, actor); }
Obj ActorOf(int owner, int id) { return Actor(owner, id); }

bool Show(int owner, int id, bool shown) {
    Obj a = Actor(owner, id);
    if (!a) return false;
    auto it = gShapes.find(id);
    if (it->second.built)                           // a model's parts are actors of their own, attached to it
        for (Obj part : models::Actors(*it->second.built)) eng::Call(part, "SetActorHiddenInGame", static_cast<uint8_t>(!shown));
    return eng::Call(a, "SetActorHiddenInGame", static_cast<uint8_t>(!shown)).Invoked();
}

void Remove(int owner, int id) {
    auto it = gShapes.find(id);
    if (it == gShapes.end() || it->second.owner != owner) return;
    Destroy(it->second);
    gShapes.erase(it);
}

void Clear(int owner) {
    for (auto it = gShapes.begin(); it != gShapes.end();) {
        if (it->second.owner == owner) {
            Destroy(it->second);
            it = gShapes.erase(it);
        } else {
            ++it;
        }
    }
}

bool Project(double x, double y, double z, double* sx, double* sy) {
    Obj controller = game::PlayerController();
    if (!controller) return false;
    Obj layout = eng::FindCdo("WidgetLayoutLibrary");
    eng::Params p(eng::FunctionOn(layout, "ProjectWorldLocationToWidgetPosition"));
    p.Set("PlayerController", controller);
    p.Set("WorldLocation", Vec3{x, y, z});
    p.Set("bPlayerViewportRelative", uint8_t{0});
    if (!eng::Invoke(layout, p) || !p.ReturnBool()) return false;
    struct {
        double x, y;
    } screen{};
    const uint8_t* s = p.Get("ScreenPosition");
    if (!s) return false;
    std::memcpy(&screen, s, sizeof screen);
    *sx = screen.x;
    *sy = screen.y;
    return true;
}

bool TakeCamera(int owner) {
    if (gCameraOwner >= 0 && gCameraOwner != owner) return false;
    if (Obj camera = eng::Get(gCamera)) {
        ViewThrough(camera);
        gCameraOwner = owner;
        return true;
    }
    Obj controller = game::PlayerController();
    Obj cls = eng::FindClass("CameraActor");
    if (!controller || !cls) return false;
    Obj statics = eng::FindCdo("GameplayStatics");
    // Where the game is looking now, so taking the camera doesn't jump.
    Obj manager = eng::ReadObj(controller, "PlayerCameraManager");
    const Vec3 at = manager ? eng::Call(manager, "GetCameraLocation").ReturnAs<Vec3>() : Vec3{0, 0, 0};
    const Rot facing = manager ? eng::Call(manager, "GetCameraRotation").ReturnAs<Rot>() : Rot{0, 0, 0};
    struct Transform {
        uint8_t bytes[96];
    } t{};
    const Vec3 one{1, 1, 1};
    const eng::Params made = eng::Call(eng::FindCdo("KismetMathLibrary"), "MakeTransform", at, facing, one);
    if (const uint8_t* r = made.Return()) std::memcpy(t.bytes, r, sizeof t.bytes);
    eng::Params begin(eng::FunctionOn(statics, "BeginDeferredActorSpawnFromClass"));
    begin.Set("WorldContextObject", controller);
    begin.Set("ActorClass", cls);
    begin.Set("SpawnTransform", t);
    begin.Set("CollisionHandlingOverride", uint8_t{1});
    begin.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, begin);
    Obj camera = begin.ReturnObj();
    if (!camera) return false;
    eng::Params finish(eng::FunctionOn(statics, "FinishSpawningActor"));
    finish.Set("Actor", camera);
    finish.Set("SpawnTransform", t);
    finish.Set("TransformScaleMethod", uint8_t{1});
    eng::Invoke(statics, finish);
    gCamera = eng::MakeWeak(camera);
    gCameraOwner = owner;
    // A CameraActor keeps a 16:9 picture by default, which puts black bars at the sides of wider screens.
    if (Obj component = eng::ReadObj(camera, "CameraComponent")) eng::Call(component, "SetConstraintAspectRatio", uint8_t{0});
    ViewThrough(camera);
    hostlog::Info("draw: camera taken");
    return true;
}

bool SetCamera(int owner, double x, double y, double z, double pitch, double yaw, double fov) {
    Obj camera = gCameraOwner == owner ? eng::Get(gCamera) : nullptr;
    if (!camera) return false;
    eng::Params p(eng::FunctionOn(camera, "K2_SetActorLocationAndRotation"));
    p.Set("NewLocation", Vec3{x, y, z});
    p.Set("NewRotation", Rot{pitch, yaw, 0});
    p.Set("bSweep", uint8_t{0});
    p.Set("bTeleport", uint8_t{1});
    eng::Invoke(camera, p);
    if (Obj component = eng::ReadObj(camera, "CameraComponent")) eng::Call(component, "SetFieldOfView", static_cast<float>(fov));
    return true;
}

void ReleaseCamera(int owner) {
    if (gCameraOwner != owner) return;
    gCameraOwner = -1;
    if (Obj pawn = Pawn()) ViewThrough(pawn);
    if (Obj camera = eng::Get(gCamera)) eng::Call(camera, "K2_DestroyActor");
    gCamera = {};
    hostlog::Info("draw: camera released");
}

bool HasCamera(int owner) { return gCameraOwner == owner && eng::Get(gCamera); }

}  // namespace draw
