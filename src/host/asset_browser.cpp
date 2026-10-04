#include "editor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cosmetics.hpp"
#include "game.hpp"
#include "layout.hpp"
#include "log.hpp"

using eng::Obj;

namespace editor {
namespace {

eng::Weak gAssetHandler;
std::set<int32_t> gRepairReported;
std::map<int32_t, std::string> gCircuitDecalCache;

struct CachedMesh {
    Vec3 location;
    Rot rotation;
    std::string path;
};
std::vector<CachedMesh> gMeshCache;

Obj Math() { return eng::FindCdo("KismetMathLibrary"); }
Obj Pawn() {
    Obj controller = game::PlayerController();
    Obj pawn = controller ? eng::Call(controller, "K2_GetPawn").ReturnObj() : nullptr;
    return pawn && eng::FindProp(eng::ClassOf(pawn), "SKGMLEHandler") ? pawn : nullptr;
}
Obj Handler() {
    Obj pawn = Pawn();
    gAssetHandler = eng::MakeWeak(pawn ? eng::ReadObj(pawn, "SKGMLEHandler") : nullptr);
    return eng::Get(gAssetHandler);
}
std::vector<Obj> AllActors();
Obj Resolve(int id) {
    if (!Handler() || !Pawn()) return nullptr;
    Obj object = id >= 0 && id < eng::NumObjects() ? eng::ObjectAt(id) : nullptr;
    if (!object) return nullptr;
    const std::vector<Obj> pieces = AllActors();
    return std::find(pieces.begin(), pieces.end(), object) != pieces.end() ? object : nullptr;
}
int IdOf(Obj actor) { return eng::MakeWeak(actor).index; }

Obj MeshComponent(Obj actor) {
    if (!actor) return nullptr;
    Obj cls = eng::ClassOf(actor);
    if (cls == eng::FindClass("BP_BaseItem_C")) return eng::ReadObj(actor, "Main");
    return eng::ReadObj(actor, "StaticMeshComponent");
}

bool IsMeshCarrier(Obj actor) {
    Obj staticActor = eng::FindClass("StaticMeshActor"), baseItem = eng::FindClass("BP_BaseItem_C");
    Obj cls = actor ? eng::ClassOf(actor) : nullptr;
    return cls && (cls == staticActor || cls == baseItem);
}

bool IsSubclassOf(Obj cls, Obj parent) {
    for (Obj c = cls; c; c = eng::SuperOf(c))
        if (c == parent) return true;
    return false;
}

bool IsAssetA(Obj asset, const char* className) {
    Obj cls = eng::FindClass(className);
    return asset && cls && eng::IsA(asset, cls);
}

bool ReassertMesh(Obj actor, Obj component) {
    if (!actor || !component || !eng::ReadObj(component, "StaticMesh")) return false;
    eng::Call(component, "SetVisibility", uint8_t{1}, uint8_t{1});
    eng::Call(component, "SetHiddenInGame", uint8_t{0}, uint8_t{1});
    eng::Call(component, "SetRenderInMainPass", uint8_t{1});
    eng::Call(component, "SetRenderInDepthPass", uint8_t{1});
    eng::Call(component, "SetCollisionEnabled", uint8_t{3});
    eng::Call(component, "SetGenerateOverlapEvents", uint8_t{1});
    eng::Call(actor, "SetActorHiddenInGame", uint8_t{0});
    eng::Call(actor, "SetActorEnableCollision", uint8_t{1});
    return true;
}

Obj StaticMesh(const std::string& assetPath) {
    Obj mesh = assetPath.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(assetPath));
    Obj cls = eng::FindClass("StaticMesh");
    return mesh && cls && eng::IsA(mesh, cls) ? mesh : nullptr;
}

Obj BaseItemClass() {
    if (Obj cls = eng::FindClass("BP_BaseItem_C")) return cls;
    cosmetics::LoadAsset(L"/Game/LevelEditor/PackageAssets/MultiplayerLevelEditor/ExampleObjects/BP_BaseItem.BP_BaseItem");
    return eng::FindClass("BP_BaseItem_C");
}

Obj SpawnEditorActor(Obj cls) {
    Obj handler = Handler(), pawn = Pawn(), actorClass = eng::FindClass("Actor");
    if (!handler || !pawn || !cls || !actorClass || !IsSubclassOf(cls, actorClass)) return nullptr;

    const Vec3 location = eng::Call(pawn, "K2_GetActorLocation").ReturnAs<Vec3>();
    const Rot rotation = eng::Call(pawn, "K2_GetActorRotation").ReturnAs<Rot>();
    const eng::Params made = eng::Call(Math(), "MakeTransform", location, rotation, Vec3{1, 1, 1});
    size_t transformSize = 0;
    const uint8_t* transform = made.Return(&transformSize);
    if (!transform) return nullptr;

    eng::Params spawn(eng::FunctionOn(handler, "SpawnActor"));
    if (!spawn.SetArg(0, cls) || !spawn.SetArg(1, transform, transformSize) || !spawn.SetArg(2, uint8_t{0}) ||
        !eng::Invoke(handler, spawn))
        return nullptr;
    Obj actor = spawn.ReturnObj();
    if (actor) {
        eng::Call(actor, "K2_SetActorLocation", location, uint8_t{0});
        eng::Call(actor, "K2_SetActorRotation", rotation, uint8_t{0});
        const int id = IdOf(actor);
        Select({id});
    }
    return actor;
}

bool IsDecalMaterial(Obj material) {
    if (!material || !IsAssetA(material, "MaterialInterface")) return false;
    Obj base = IsAssetA(material, "Material") ? material : eng::Call(material, "GetBaseMaterial").ReturnObj();
    uint8_t domain = 0;
    return base && eng::ReadBytes(base, "MaterialDomain", &domain, sizeof domain) && domain == 1;
}

bool OverrideCircuitDecalStyle(Obj actor, Obj material) {
    Obj circuit = actor ? eng::ReadObj(actor, "UGCCircuitDecal") : nullptr;
    if (!circuit || !eng::IsA(circuit, eng::FindClass("BallestUGCCircuitDecalComponent"))) return false;

    const eng::Prop palette = eng::FindProp(eng::ClassOf(circuit), "MaterialPalette");
    const eng::Prop option = eng::InnerOf(palette);
    Obj optionStruct = eng::StructOf(option);
    const eng::Prop normal = eng::FindProp(optionStruct, "NormalMaterial");
    const eng::Prop facing = eng::FindProp(optionStruct, "FacingOnlyMaterial");
    struct ArrayValue { Obj data; int32_t num, max; } entries{};
    if (eng::KindOf(palette) != "ArrayProperty" || palette.size != sizeof entries ||
        eng::KindOf(option) != "StructProperty" || option.size <= 0 || !optionStruct ||
        eng::KindOf(normal) != "ObjectProperty" || normal.size != sizeof material ||
        eng::KindOf(facing) != "ObjectProperty" || facing.size != sizeof material ||
        !eng::MemoryReadable(circuit + palette.offset, sizeof entries)) return false;
    std::memcpy(&entries, circuit + palette.offset, sizeof entries);

    if (!entries.data || entries.num <= 0 || entries.num > entries.max || entries.max > 1024) return false;
    const size_t stride = static_cast<size_t>(option.size);
    for (int32_t index = 0; index < entries.num; ++index) {
        const size_t offset = static_cast<size_t>(index) * stride;
        if (offset / stride != static_cast<size_t>(index) || !eng::MemoryReadable(entries.data + offset, stride)) return false;
        Obj entry = entries.data + offset;
        std::memcpy(entry + normal.offset, &material, sizeof material);
        std::memcpy(entry + facing.offset, &material, sizeof material);
        Obj writtenNormal = nullptr, writtenFacing = nullptr;
        std::memcpy(&writtenNormal, entry + normal.offset, sizeof writtenNormal);
        std::memcpy(&writtenFacing, entry + facing.offset, sizeof writtenFacing);
        if (writtenNormal != material || writtenFacing != material) return false;
    }
    return eng::Call(circuit, "RefreshDecal").Invoked();
}

bool ApplyCircuitDecalCarrierMaterial(Obj actor, Obj material) {
    Obj carrier = actor ? eng::ReadObj(actor, "Main") : nullptr;
    Obj meshClass = eng::FindClass("MeshComponent");
    if (!carrier || !meshClass || !eng::IsA(carrier, meshClass)) return false;
    const eng::Params applied = eng::Call(carrier, "SetMaterial", int32_t{0}, material);
    return applied.Invoked() && eng::Call(carrier, "GetMaterial", int32_t{0}).ReturnObj() == material;
}

bool PersistCircuitDecalMaterial(Obj actor, Obj material, const std::string& assetPath) {
    Obj statics = eng::FindCdo("SKGMLEStatics"), strings = eng::FindCdo("KismetStringLibrary");
    Obj fn = statics ? eng::FunctionOn(statics, "GetItemMaterialData") : nullptr;
    if (!actor || !material || !statics || !strings || !fn || !ApplyCircuitDecalCarrierMaterial(actor, material)) return false;

    eng::Params generated(fn);
    if (!generated.SetArg(0, actor) || !eng::Invoke(statics, generated)) return false;
    size_t dataSize = 0;
    const uint8_t* returned = generated.Return(&dataSize);
    const eng::Prop returnProp = eng::FindProp(fn, "ReturnValue");
    Obj dataStruct = eng::StructOf(returnProp);
    if (!returned || dataSize != 24 || !dataStruct) return false;

    uint8_t data[24];
    std::memcpy(data, returned, sizeof data);
    if (const eng::Prop actorProp = eng::FindProp(dataStruct, "Actor"); actorProp && actorProp.size == sizeof actor)
        std::memcpy(data + actorProp.offset, &actor, sizeof actor);

    struct ArrayHeader { uint8_t* data; int32_t num, max; } components{}, materials{}, paths{};
    const eng::Prop componentsProp = eng::FindProp(dataStruct, "MeshComponents");
    const eng::Prop componentProp = eng::InnerOf(componentsProp);
    Obj componentStruct = eng::StructOf(componentProp);
    if (eng::KindOf(componentsProp) != "ArrayProperty" || componentsProp.size != sizeof components ||
        eng::KindOf(componentProp) != "StructProperty" || componentProp.size <= 0 || !componentStruct ||
        componentsProp.offset + sizeof components > sizeof data) return false;
    std::memcpy(&components, data + componentsProp.offset, sizeof components);
    if (!components.data || components.num < 1 || components.num > components.max || components.max > 1024 ||
        !eng::MemoryReadable(components.data, static_cast<size_t>(componentProp.size))) return false;

    const eng::Prop materialsProp = eng::FindProp(componentStruct, "Materials");
    const eng::Prop materialEntryProp = eng::InnerOf(materialsProp);
    Obj materialEntryStruct = eng::StructOf(materialEntryProp);
    const eng::Prop materialProp = eng::FindProp(materialEntryStruct, "Material");
    if (eng::KindOf(materialsProp) != "ArrayProperty" || materialsProp.size != sizeof materials ||
        eng::KindOf(materialEntryProp) != "StructProperty" || materialEntryProp.size <= 0 || !materialEntryStruct ||
        eng::KindOf(materialProp) != "ObjectProperty" || materialProp.size != sizeof material ||
        materialsProp.offset + materialsProp.size > componentProp.size) return false;
    std::memcpy(&materials, components.data + materialsProp.offset, sizeof materials);
    if (!materials.data || materials.num < 1 || materials.num > materials.max || materials.max > 1024 ||
        !eng::MemoryReadable(materials.data, static_cast<size_t>(materialEntryProp.size)) ||
        materialProp.offset + materialProp.size > materialEntryProp.size) return false;
    std::memcpy(materials.data + materialProp.offset, &material, sizeof material);

    const eng::Prop pathsProp = eng::FindProp(componentStruct, "MaterialAssetPaths");
    const eng::Prop pathProp = eng::InnerOf(pathsProp);
    if (eng::KindOf(pathsProp) != "ArrayProperty" || pathsProp.size != sizeof paths ||
        eng::KindOf(pathProp) != "StrProperty" || pathProp.size != 16 ||
        pathsProp.offset + pathsProp.size > componentProp.size) return false;
    std::memcpy(&paths, components.data + pathsProp.offset, sizeof paths);
    if (!paths.data || paths.num < 1 || paths.num > paths.max || paths.max > 1024 ||
        !eng::MemoryReadable(paths.data, static_cast<size_t>(pathProp.size))) return false;

    const std::wstring wide = eng::Widen(assetPath), empty;
    const eng::FString source{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
    const eng::FString suffix{empty.c_str(), 1, 1};
    const eng::Params owned = eng::Call(strings, "Concat_StrStr", source, suffix);
    size_t stringSize = 0;
    const uint8_t* persistentString = owned.Return(&stringSize);
    if (!persistentString || stringSize != static_cast<size_t>(pathProp.size)) return false;
    std::memcpy(paths.data, persistentString, stringSize);

    if (!eng::WriteBytes(actor, "Map Editor Item Material", data, sizeof data)) return false;
    eng::Params loaded(eng::FunctionOn(actor, "OnMaterialLoaded"));
    return loaded.SetArg(0, data, sizeof data) && eng::Invoke(actor, loaded);
}

int SpawnDecalMaterial(Obj material, const std::string& assetPath) {
    if (!material || !IsDecalMaterial(material)) return -1;
    const std::string decalActorPath = "/Game/MapItems/Decals/BP_UGCCircuitDecal.BP_UGCCircuitDecal";
    Obj blueprint = cosmetics::LoadAsset(eng::Widen(decalActorPath));
    Obj cls = blueprint && IsAssetA(blueprint, "Blueprint") ? eng::ReadObj(blueprint, "GeneratedClass") : nullptr;
    if (!cls) {
        cosmetics::LoadAsset(eng::Widen(decalActorPath + "_C"));
        cls = eng::FindClass("BP_UGCCircuitDecal_C");
    }
    Obj actor = SpawnEditorActor(cls);
    Obj component = actor ? eng::ReadObj(actor, "CircuitDecal") : nullptr;
    if (!actor || !component || !eng::IsA(component, eng::FindClass("DecalComponent"))) {
        if (actor) eng::Call(actor, "K2_DestroyActor");
        return -1;
    }

    // Ballest serializes the carrier's Map Editor Item Material record, not DefaultMaterial or the native palette.
    // Store the requested decal path in that existing material-path array so it can be restored after load/Undo/Redo.
    if (!PersistCircuitDecalMaterial(actor, material, assetPath)) {
        eng::Call(actor, "K2_DestroyActor");
        return -1;
    }
    // The Blueprint's native circuit component normally resolves DecalStyle through a three-entry MaterialPalette.
    // Replace both variants of every entry so RefreshDecal and later style changes retain this spawned asset.
    // DefaultMaterial and the final visual assignment remain as compatibility and immediate-render safeguards.
    eng::WriteBytes(actor, "DefaultMaterial", &material, sizeof material);
    if (!OverrideCircuitDecalStyle(actor, material)) {
        eng::Call(actor, "K2_DestroyActor");
        return -1;
    }
    const eng::Params applied = eng::Call(component, "SetDecalMaterial", material);
    if (!applied.Invoked() || eng::ReadObj(component, "DecalMaterial") != material) {
        eng::Call(actor, "K2_DestroyActor");
        return -1;
    }
    eng::Call(component, "SetVisibility", uint8_t{1}, uint8_t{1});
    eng::Call(component, "SetHiddenInGame", uint8_t{0}, uint8_t{1});
    const int id = IdOf(actor);
    hostlog::Info("editor: spawned projected decal " + assetPath);
    return id;
}

bool ApplyStaticMesh(Obj component, Obj mesh) {
    if (!component || !mesh) return false;
    const eng::Params applied = eng::Call(component, "SetStaticMesh", mesh);
    if (applied.Invoked() && applied.ReturnBool()) return true;
    // A newly created native StaticMeshActor on some Ballest builds rejects SetStaticMesh during its spawn frame,
    // even though its reflected StaticMesh property is already live. The editor/save code reads this property too.
    if (!eng::WriteBytes(component, "StaticMesh", &mesh, sizeof mesh)) return false;
    eng::Call(component, "SetStaticMesh", mesh);     // refresh render/physics state now that the property is populated
    return eng::ReadObj(component, "StaticMesh") == mesh;
}

bool PersistBaseItemMesh(Obj actor, const std::string& assetPath) {
    Obj statics = eng::FindCdo("SKGMLEStatics"), strings = eng::FindCdo("KismetStringLibrary");
    Obj fn = statics ? eng::FunctionOn(statics, "GetItemMaterialData") : nullptr;
    if (!actor || !statics || !strings || !fn) {
        hostlog::Warn("editor: persist missing actor/statics/strings/function");
        return false;
    }

    eng::Params generated(fn);
    if (!generated.SetArg(0, actor) || !eng::Invoke(statics, generated)) {
        hostlog::Warn("editor: GetItemMaterialData call failed");
        return false;
    }
    size_t dataSize = 0;
    const uint8_t* returned = generated.Return(&dataSize);
    eng::Prop returnProp = eng::FindProp(fn, "ReturnValue");
    Obj dataStruct = eng::StructOf(returnProp);
    if (!returned || dataSize != 24 || !dataStruct) {
        hostlog::Warn("editor: material data return invalid (size " + std::to_string(dataSize) + ")");
        return false;
    }

    uint8_t data[24];
    std::memcpy(data, returned, sizeof data);
    if (const eng::Prop actorProp = eng::FindProp(dataStruct, "Actor"); actorProp && actorProp.size == sizeof actor)
        std::memcpy(data + actorProp.offset, &actor, sizeof actor);

    const eng::Prop arrayProp = eng::FindProp(dataStruct, "MeshComponents");
    const eng::Prop innerProp = eng::InnerOf(arrayProp);
    Obj componentStruct = eng::StructOf(innerProp);
    struct ArrayHeader {
        uint8_t* data;
        int32_t num, max;
    } components{};
    if (!arrayProp || arrayProp.size != sizeof components || !innerProp || !componentStruct ||
        arrayProp.offset + sizeof components > sizeof data) {
        hostlog::Warn("editor: mesh array reflection invalid (array " + eng::KindOf(arrayProp) + " size " +
                      std::to_string(arrayProp.size) + ", inner " + eng::KindOf(innerProp) + " size " +
                      std::to_string(innerProp.size) + ")");
        return false;
    }
    std::memcpy(&components, data + arrayProp.offset, sizeof components);
    const eng::Prop pathProp = eng::FindProp(componentStruct, "MeshAssetPath");
    if (!components.data || components.num < 1 || innerProp.size <= 0 || !pathProp || pathProp.size != 16 ||
        pathProp.offset + pathProp.size > innerProp.size) {
        hostlog::Warn("editor: generated mesh data invalid (num " + std::to_string(components.num) + ", element " +
                      std::to_string(innerProp.size) + ", path " + eng::KindOf(pathProp) + " @" +
                      std::to_string(pathProp.offset) + " size " + std::to_string(pathProp.size) + ")");
        return false;
    }

    const std::wstring wide = eng::Widen(assetPath), empty;
    const eng::FString source{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
    const eng::FString suffix{empty.c_str(), 1, 1};
    const eng::Params owned = eng::Call(strings, "Concat_StrStr", source, suffix);
    size_t stringSize = 0;
    const uint8_t* persistentString = owned.Return(&stringSize);
    if (!persistentString || stringSize != static_cast<size_t>(pathProp.size)) {
        hostlog::Warn("editor: persistent FString allocation failed (size " + std::to_string(stringSize) + ")");
        return false;
    }
    std::memcpy(components.data + pathProp.offset, persistentString, stringSize);

    if (!eng::WriteBytes(actor, "Map Editor Item Material", data, sizeof data)) {
        hostlog::Warn("editor: Map Editor Item Material write failed");
        return false;
    }
    eng::Params loaded(eng::FunctionOn(actor, "OnMaterialLoaded"));
    if (!loaded.SetArg(0, data, sizeof data) || !eng::Invoke(actor, loaded)) {
        hostlog::Warn("editor: OnMaterialLoaded call failed");
        return false;
    }
    hostlog::Info("editor: persisted BP_BaseItem mesh path " + assetPath);
    return true;
}

std::string PersistedBaseItemMesh(Obj actor) {
    uint8_t data[24];
    if (!actor || !eng::ReadBytes(actor, "Map Editor Item Material", data, sizeof data)) return "";
    const eng::Prop materialProp = eng::FindProp(eng::ClassOf(actor), "Map Editor Item Material");
    Obj dataStruct = eng::StructOf(materialProp);
    const eng::Prop arrayProp = eng::FindProp(dataStruct, "MeshComponents"), innerProp = eng::InnerOf(arrayProp);
    Obj componentStruct = eng::StructOf(innerProp);
    struct ArrayHeader {
        uint8_t* data;
        int32_t num, max;
    } components{};
    if (!arrayProp || arrayProp.size != sizeof components || !innerProp || !componentStruct ||
        arrayProp.offset + sizeof components > sizeof data)
        return "";
    std::memcpy(&components, data + arrayProp.offset, sizeof components);
    const eng::Prop pathProp = eng::FindProp(componentStruct, "MeshAssetPath");
    if (!components.data || components.num < 1 || innerProp.size <= 0 || !pathProp || pathProp.size != 16 ||
        pathProp.offset + pathProp.size > innerProp.size)
        return "";
    return eng::ReadFString(components.data + pathProp.offset);
}

// A TArray<AActor*> returned by value: { data, num, max }.
std::vector<Obj> ObjArray(const uint8_t* tarray) {
    std::vector<Obj> out;
    if (!tarray) return out;
    Obj* data = nullptr;
    int32_t num = 0;
    std::memcpy(&data, tarray, sizeof data);
    std::memcpy(&num, tarray + 8, sizeof num);
    for (int32_t i = 0; data && i < num && i < 100000; ++i) out.push_back(data[i]);
    return out;
}

std::vector<Obj> AllActors() {
    if (!Handler()) return {};
    // Current Ballest builds keep the AllActors property as a cache. Ask the handler to refresh it so actors added
    // through AddLocalActor, load, Undo or Redo are visible immediately; older builds still fall back to the property.
    const eng::Params refreshed = eng::Call(Handler(), "GetAllActors", uint8_t{1}, uint8_t{1});
    const auto actors = ObjArray(refreshed.Return());
    return refreshed.Invoked() ? actors : eng::ReadObjArray(Handler(), "AllActors");
}

bool Near(const Vec3& a, const Vec3& b, double tolerance) {
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

bool Near(const Rot& a, const Rot& b, double tolerance) {
    return std::fabs(a.pitch - b.pitch) <= tolerance && std::fabs(a.yaw - b.yaw) <= tolerance &&
           std::fabs(a.roll - b.roll) <= tolerance;
}

void CacheMesh(Obj actor, const std::string& path) {
    if (!actor || path.empty()) return;
    const Vec3 location = eng::Call(actor, "K2_GetActorLocation").ReturnAs<Vec3>();
    const Rot rotation = eng::Call(actor, "K2_GetActorRotation").ReturnAs<Rot>();
    for (auto& cached : gMeshCache)
        if (Near(cached.location, location, 0.01) && Near(cached.rotation, rotation, 0.01)) {
            cached.path = path;
            return;
        }
    gMeshCache.push_back({location, rotation, path});
}

std::string CachedMeshPath(Obj actor) {
    if (!actor) return "";
    const Vec3 location = eng::Call(actor, "K2_GetActorLocation").ReturnAs<Vec3>();
    const Rot rotation = eng::Call(actor, "K2_GetActorRotation").ReturnAs<Rot>();
    for (auto it = gMeshCache.rbegin(); it != gMeshCache.rend(); ++it)
        if (Near(it->location, location, 0.01) && Near(it->rotation, rotation, 0.01)) return it->path;
    return "";
}

}  // namespace

int SpawnMesh(const std::string& assetPath) {
    Obj handler = Handler(), pawn = Pawn(), cls = BaseItemClass(), mesh = StaticMesh(assetPath);
    if (!handler || !pawn || !cls || !mesh) return -1;

    const Vec3 location = eng::Call(pawn, "K2_GetActorLocation").ReturnAs<Vec3>();
    const Rot rotation = eng::Call(pawn, "K2_GetActorRotation").ReturnAs<Rot>();
    const Vec3 scale{1, 1, 1};
    const eng::Params made = eng::Call(Math(), "MakeTransform", location, rotation, scale);
    size_t transformSize = 0;
    const uint8_t* transform = made.Return(&transformSize);
    if (!transform) return -1;

    eng::Params spawn(eng::FunctionOn(handler, "SpawnActor"));
    if (!spawn.SetArg(0, cls) || !spawn.SetArg(1, transform, transformSize) || !spawn.SetArg(2, uint8_t{0}) ||
        !eng::Invoke(handler, spawn))
        return -1;
    Obj actor = spawn.ReturnObj();
    Obj component = MeshComponent(actor);
    if (!actor) {
        hostlog::Warn("editor: SpawnActor returned no BP_BaseItem");
        return -1;
    }
    eng::Call(actor, "K2_SetActorLocation", location, uint8_t{0});
    eng::Call(actor, "K2_SetActorRotation", rotation, uint8_t{0});
    if (!component) {
        hostlog::Warn("editor: spawned BP_BaseItem has no Main mesh component");
        eng::Call(actor, "K2_DestroyActor");
        return -1;
    }
    if (!ApplyStaticMesh(component, mesh)) {
        hostlog::Warn("editor: could not apply StaticMesh property for " + assetPath);
        eng::Call(actor, "K2_DestroyActor");
        return -1;
    }
    if (!ReassertMesh(actor, component)) {
        hostlog::Warn("editor: mesh applied but render state could not be reasserted for " + assetPath);
        if (actor) eng::Call(actor, "K2_DestroyActor");
        return -1;
    }
    if (!PersistBaseItemMesh(actor, assetPath)) {
        hostlog::Warn("editor: mesh visible but BP_BaseItem save data could not be populated for " + assetPath);
        eng::Call(actor, "K2_DestroyActor");
        return -1;
    }
    CacheMesh(actor, assetPath);
    ReassertMesh(actor, component);
    const int id = IdOf(actor);
    Select({id});
    hostlog::Info("editor: spawned mesh " + assetPath);
    return id;
}

std::string SpawnAsset(const std::string& assetPath) {
    if (!Handler() || !Pawn()) return "Open the level editor to use an asset.";
    Obj asset = assetPath.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(assetPath));
    // Cooked Blueprint packages commonly omit the UBlueprint object named by the catalog and retain only the
    // generated class beside it (ObjectName_C). Accept the catalog path directly by trying that cooked form.
    if (!asset && assetPath.find('.') != std::string::npos &&
        (assetPath.size() < 2 || assetPath.compare(assetPath.size() - 2, 2, "_C") != 0))
        asset = cosmetics::LoadAsset(eng::Widen(assetPath + "_C"));
    if (!asset) return "Could not load asset: " + assetPath;

    if (IsAssetA(asset, "StaticMesh")) {
        const int id = SpawnMesh(assetPath);
        return id < 0 ? "Could not spawn StaticMesh: " + assetPath
                      : "Spawned " + eng::ObjName(asset) + " (piece " + std::to_string(id) + ")";
    }

    const Vec3 location = eng::Call(Pawn(), "K2_GetActorLocation").ReturnAs<Vec3>();
    if (IsAssetA(asset, "NiagaraSystem")) {
        Obj lib = eng::FindCdo("NiagaraFunctionLibrary");
        eng::Params p(eng::FunctionOn(lib, "SpawnSystemAtLocation"));
        p.Set("WorldContextObject", Pawn());
        p.Set("SystemTemplate", asset);
        p.Set("Location", location);
        p.Set("Rotation", Vec3{});
        p.Set("Scale", Vec3{1, 1, 1});
        p.Set("bAutoDestroy", uint8_t{1});
        p.Set("bAutoActivate", uint8_t{1});
        if (eng::Invoke(lib, p) && p.ReturnObj()) {
            hostlog::Info("editor: previewed Niagara system " + assetPath);
            return "Previewing Niagara system " + eng::ObjName(asset) + " (not saved with the level)";
        }
        return "Could not preview Niagara system: " + assetPath;
    }

    if (IsAssetA(asset, "MaterialInterface")) {
        if (IsDecalMaterial(asset)) {
            const int id = SpawnDecalMaterial(asset, assetPath);
            return id < 0 ? "Could not create projected decal: " + assetPath
                          : "Spawned projected decal " + eng::ObjName(asset) + " (piece " + std::to_string(id) + ")";
        }
        const int id = SpawnMesh("/Engine/EngineMeshes/MaterialSphere.MaterialSphere");
        if (id >= 0 && SetMaterial(id, assetPath)) {
            hostlog::Info("editor: spawned material preview " + assetPath);
            return "Spawned material preview for " + eng::ObjName(asset) + " (piece " + std::to_string(id) + ")";
        }
        return "Could not create material preview: " + assetPath;
    }

    const std::string type = eng::ObjName(eng::ClassOf(asset));
    if (IsAssetA(asset, "World"))
        return "Loaded level asset " + eng::ObjName(asset) + "; levels must be opened, not spawned into the current level.";
    return "Loaded " + type + " " + eng::ObjName(asset) + "; this asset type is not an independent world object.";
}

bool SetMesh(int id, const std::string& assetPath) {
    Obj actor = Resolve(id), component = MeshComponent(actor), mesh = StaticMesh(assetPath);
    if (!IsMeshCarrier(actor) || !component || !mesh || !ApplyStaticMesh(component, mesh) || !ReassertMesh(actor, component)) return false;
    if (eng::ClassOf(actor) == eng::FindClass("BP_BaseItem_C") && !PersistBaseItemMesh(actor, assetPath)) return false;
    CacheMesh(actor, assetPath);
    return true;
}

bool SetMaterial(int id, const std::string& assetPath) {
    Obj actor = Resolve(id);
    Obj material = assetPath.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(assetPath));
    Obj meshClass = eng::FindClass("MeshComponent");
    if (!actor || !material || !IsAssetA(material, "MaterialInterface") || !meshClass) return false;

    std::vector<Obj> components;
    const eng::Params found = eng::Call(actor, "K2_GetComponentsByClass", meshClass);
    if (found.Invoked()) components = ObjArray(found.Return());
    if (Obj primary = MeshComponent(actor); primary &&
        std::find(components.begin(), components.end(), primary) == components.end())
        components.push_back(primary);

    bool changed = false;
    for (Obj component : components) {
        if (!component || !eng::IsA(component, meshClass)) continue;
        const int32_t slots = eng::Call(component, "GetNumMaterials").ReturnAs<int32_t>();
        for (int32_t slot = 0; slot < slots && slot < 64; ++slot)
            changed = eng::Call(component, "SetMaterial", slot, material).Invoked() || changed;
    }
    if (!changed) return false;

    // GetItemMaterialData captures the component overrides. Preserve the custom mesh path before feeding the record
    // back through OnMaterialLoaded, otherwise a BP_BaseItem made by SpawnMesh could lose its cooked mesh on reload.
    if (eng::ClassOf(actor) == eng::FindClass("BP_BaseItem_C")) {
        std::string meshPath = PersistedBaseItemMesh(actor);
        if (meshPath.empty()) meshPath = CachedMeshPath(actor);
        if (!meshPath.empty() && !PersistBaseItemMesh(actor, meshPath)) return false;
    }
    hostlog::Info("editor: applied material " + assetPath + " to " + eng::ObjName(actor));
    return true;
}

std::string DescribeObject(const std::string& assetPath, Obj asset) {
    if (!asset) return "";
    Obj cls = eng::ClassOf(asset);
    std::string inheritance;
    for (Obj c = cls; c; c = eng::SuperOf(c))
        inheritance += (inheritance.empty() ? "" : " -> ") + eng::ObjName(c);
    return "Path: " + assetPath + "\nResolved: " + eng::PathOf(asset) + "\nType: " + eng::ObjName(cls) +
           "\nInheritance: " + inheritance;
}

std::string DescribeAsset(const std::string& assetPath) {
    if (!Handler() || !Pawn()) return "";
    Obj asset = assetPath.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(assetPath));
    return DescribeObject(assetPath, asset);
}

std::string DescribeMesh(const std::string& assetPath) {
    if (!Handler() || !Pawn()) return "";
    return DescribeObject(assetPath, StaticMesh(assetPath));
}

namespace {

struct PropertyTarget { std::string prefix; Obj object = nullptr; };

std::vector<PropertyTarget> PropertyTargets(Obj actor) {
    std::vector<PropertyTarget> out;
    if (!actor) return out;
    out.push_back({"Actor", actor});
    Obj componentClass = eng::FindClass("ActorComponent");
    if (!componentClass) return out;
    const eng::Params found = eng::Call(actor, "K2_GetComponentsByClass", componentClass);
    std::set<std::string> used;
    for (Obj component : ObjArray(found.Return())) {
        if (!component || !eng::IsA(component, componentClass)) continue;
        std::string name = eng::ObjName(component);
        if (!used.insert(name).second) {
            const std::string base = name;
            int suffix = 2;
            while (!used.insert(base + "#" + std::to_string(suffix)).second) ++suffix;
            name = base + "#" + std::to_string(suffix);
        }
        out.push_back({"Component:" + name, component});
    }
    return out;
}

bool NumericKind(const std::string& kind) {
    return kind == "BoolProperty" || kind == "ByteProperty" || kind == "Int8Property" ||
           kind == "Int16Property" || kind == "IntProperty" || kind == "Int64Property" ||
           kind == "UInt16Property" || kind == "UInt32Property" || kind == "UInt64Property" ||
           kind == "FloatProperty" || kind == "DoubleProperty";
}

bool EditableStruct(const eng::Prop& property) {
    if (eng::KindOf(property) != "StructProperty") return false;
    const std::string type = eng::ObjName(eng::StructOf(property));
    return ((type == "Vector" || type == "Rotator") && property.size == 24) ||
           (type == "Vector2D" && property.size == 16) ||
           ((type == "Vector4" || type == "Quat") && property.size == 32) ||
           (type == "LinearColor" && property.size == 16) || (type == "Color" && property.size == 4) ||
           (type == "IntPoint" && property.size == 8) || (type == "IntVector" && property.size == 12) ||
           (type == "IntVector4" && property.size == 16);
}

bool SupportedProperty(const eng::Prop& property, const std::string& kind) {
    if (kind == "BoolProperty") return property.size >= 1;
    if (kind == "ByteProperty" || kind == "Int8Property") return property.size == 1;
    if (kind == "Int16Property" || kind == "UInt16Property") return property.size == 2;
    if (kind == "IntProperty" || kind == "UInt32Property" || kind == "FloatProperty") return property.size == 4;
    if (kind == "Int64Property" || kind == "UInt64Property" || kind == "DoubleProperty") return property.size == 8;
    if (kind == "EnumProperty") {
        const eng::Prop underlying = eng::EnumUnderlyingOf(property);
        return underlying && underlying.size == property.size && SupportedProperty(underlying, eng::KindOf(underlying));
    }
    if (kind == "NameProperty") return property.size == 8;
    if (kind == "StrProperty" || kind == "TextProperty") return property.size == 16;
    if ((kind == "ObjectProperty" || kind == "ClassProperty") && property.size == static_cast<int32_t>(sizeof(Obj))) return true;
    return EditableStruct(property);
}

std::string ReadOnlyReason(const eng::Prop& property, const std::string& kind) {
    const uint64_t flags = eng::FlagsOf(property);
    if (flags & (layout::kPropertyFlagIsParameter | layout::kPropertyFlagIsReturnValue)) return "function parameter";
    if (SupportedProperty(property, kind)) return "";
    if (kind == "ArrayProperty" || kind == "MapProperty" || kind == "SetProperty") return "resizable container";
    if (kind.find("DelegateProperty") != std::string::npos) return "delegate";
    if (kind == "StructProperty") {
        const std::string type = eng::ObjName(eng::StructOf(property));
        return "unsupported struct" + (type.empty() ? std::string() : " " + type);
    }
    return "unsupported reflected type";
}

template <class T>
T PropertyValue(Obj object, const eng::Prop& property) {
    T value{};
    if (object && property && property.size == static_cast<int32_t>(sizeof value) &&
        eng::MemoryReadable(object + property.offset, sizeof value))
        std::memcpy(&value, object + property.offset, sizeof value);
    return value;
}

std::string Number(double value) {
    std::ostringstream out;
    out << std::setprecision(12) << value;
    return out.str();
}

template <class T>
std::string IntegerProperty(Obj object, const eng::Prop& property) {
    return std::to_string(PropertyValue<T>(object, property));
}

std::string DisplayNumeric(Obj object, const eng::Prop& property, const std::string& kind) {
    if (kind == "ByteProperty" && property.size == 1) return IntegerProperty<uint8_t>(object, property);
    if (kind == "Int8Property" && property.size == 1) return IntegerProperty<int8_t>(object, property);
    if (kind == "Int16Property" && property.size == 2) return IntegerProperty<int16_t>(object, property);
    if (kind == "IntProperty" && property.size == 4) return IntegerProperty<int32_t>(object, property);
    if (kind == "Int64Property" && property.size == 8) return IntegerProperty<int64_t>(object, property);
    if (kind == "UInt16Property" && property.size == 2) return IntegerProperty<uint16_t>(object, property);
    if (kind == "UInt32Property" && property.size == 4) return IntegerProperty<uint32_t>(object, property);
    if (kind == "UInt64Property" && property.size == 8) return IntegerProperty<uint64_t>(object, property);
    if (kind == "FloatProperty" && property.size == 4) return Number(PropertyValue<float>(object, property));
    if (kind == "DoubleProperty" && property.size == 8) return Number(PropertyValue<double>(object, property));
    return "<invalid numeric storage>";
}

template <class T>
std::string Components(const T* values, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) out += (out.empty() ? "" : ", ") + Number(static_cast<double>(values[i]));
    return out;
}

std::string DisplayProperty(Obj object, const std::string& name, const eng::Prop& p, const std::string& kind) {
    (void)name;
    if (kind == "BoolProperty") { bool v = false; return eng::ReadBoolValue(object, p, &v) ? (v ? "true" : "false") : "<unreadable>"; }
    if (NumericKind(kind) && kind != "BoolProperty") return DisplayNumeric(object, p, kind);
    if (kind == "EnumProperty") return DisplayNumeric(object, {p.field, p.offset, p.size}, eng::KindOf(eng::EnumUnderlyingOf(p)));
    if (kind == "NameProperty" && p.size == 8) {
        struct FNameValue { uint32_t index; int32_t number; } v = PropertyValue<FNameValue>(object, p);
        return eng::Name(v.index, v.number);
    }
    if (kind == "StrProperty" && p.size == 16) return eng::ReadFString(object + p.offset);
    if (kind == "TextProperty" && p.size == 16) {
        Obj library = eng::FindCdo("KismetTextLibrary");
        eng::Params converted(eng::FunctionOn(library, "Conv_TextToString"));
        if (!converted.SetArg(0, object + p.offset, static_cast<size_t>(p.size)) || !eng::Invoke(library, converted))
            return "<unreadable text>";
        return eng::ReadFString(converted.Return());
    }
    if ((kind == "ObjectProperty" || kind == "ClassProperty") && p.size == static_cast<int32_t>(sizeof(Obj))) {
        Obj v = PropertyValue<Obj>(object, p);
        return v && eng::IsLive(v) ? eng::PathOf(v) : "None";
    }
    if (kind == "ArrayProperty" && p.size == 16) {
        struct ArrayValue { void* data; int32_t num, max; } v = PropertyValue<ArrayValue>(object, p);
        return v.num >= 0 && v.num <= v.max && v.max < 10000000 ? "[" + std::to_string(v.num) + " element(s)]" : "<invalid array>";
    }
    if (kind == "StructProperty") {
        const std::string type = eng::ObjName(eng::StructOf(p));
        if ((type == "Vector" || type == "Rotator") && p.size == 24) {
            const Vec3 v = PropertyValue<Vec3>(object, p);
            return Number(v.x) + ", " + Number(v.y) + ", " + Number(v.z);
        }
        if (type == "LinearColor" && p.size == 16) {
            struct Color { float r, g, b, a; } v = PropertyValue<Color>(object, p);
            return Number(v.r) + ", " + Number(v.g) + ", " + Number(v.b) + ", " + Number(v.a);
        }
        if (type == "Vector2D" && p.size == 16) { const auto v = PropertyValue<std::array<double, 2>>(object, p); return Components(v.data(), v.size()); }
        if ((type == "Vector4" || type == "Quat") && p.size == 32) { const auto v = PropertyValue<std::array<double, 4>>(object, p); return Components(v.data(), v.size()); }
        if (type == "Color" && p.size == 4) { const auto v = PropertyValue<std::array<uint8_t, 4>>(object, p); return Components(v.data(), v.size()); }
        if (type == "IntPoint" && p.size == 8) { const auto v = PropertyValue<std::array<int32_t, 2>>(object, p); return Components(v.data(), v.size()); }
        if (type == "IntVector" && p.size == 12) { const auto v = PropertyValue<std::array<int32_t, 3>>(object, p); return Components(v.data(), v.size()); }
        if (type == "IntVector4" && p.size == 16) { const auto v = PropertyValue<std::array<int32_t, 4>>(object, p); return Components(v.data(), v.size()); }
        return "<" + (type.empty() ? std::string("struct") : type) + ", " + std::to_string(p.size) + " bytes>";
    }
    return "<" + kind + ", " + std::to_string(p.size) + " bytes>";
}

struct ResolvedProperty {
    Obj actor = nullptr;
    Obj owner = nullptr;
    Obj base = nullptr;
    std::string name;
    eng::Prop property;
};

bool ArrayValueAt(Obj base, const eng::Prop& arrayProperty, int32_t index, Obj* element, eng::Prop* inner) {
    struct ArrayValue { Obj data; int32_t num, max; } array{};
    if (!base || eng::KindOf(arrayProperty) != "ArrayProperty" || arrayProperty.size != 16 ||
        !eng::MemoryReadable(base + arrayProperty.offset, sizeof array)) return false;
    std::memcpy(&array, base + arrayProperty.offset, sizeof array);
    *inner = eng::InnerOf(arrayProperty);
    if (!*inner || inner->size <= 0 || index < 0 || index >= array.num || array.num < 0 || array.num > array.max ||
        array.max > 10000000 || !array.data) return false;
    const size_t stride = static_cast<size_t>(inner->size);
    const size_t at = static_cast<size_t>(index) * stride;
    if (at / stride != static_cast<size_t>(index) || !eng::MemoryReadable(array.data + at, stride)) return false;
    *element = array.data + at;
    inner->offset = 0;
    return true;
}

bool ResolvePropertyPath(Obj actor, const std::string& path, ResolvedProperty* resolved) {
    const size_t dot = path.find('.');
    if (!actor || dot == std::string::npos || dot == 0 || dot + 1 >= path.size()) return false;
    const std::string prefix = path.substr(0, dot);
    Obj object = nullptr;
    for (const PropertyTarget& target : PropertyTargets(actor))
        if (target.prefix == prefix) { object = target.object; break; }
    if (!object) return false;

    Obj base = object;
    Obj structure = eng::ClassOf(object);
    size_t at = dot + 1;
    while (at < path.size()) {
        const size_t end = path.find_first_of(".[", at);
        const std::string name = path.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (name.empty()) return false;
        eng::Prop property = eng::FindProp(structure, name);
        if (!property || eng::KindOf(property).empty()) return false;
        if (end == std::string::npos) {
            *resolved = {actor, object, base, name, property};
            return true;
        }
        if (path[end] == '.') {
            if (eng::KindOf(property) != "StructProperty" || !eng::StructOf(property) ||
                !eng::MemoryReadable(base + property.offset, static_cast<size_t>(property.size))) return false;
            base += property.offset;
            structure = eng::StructOf(property);
            at = end + 1;
            continue;
        }

        const size_t close = path.find(']', end + 1);
        if (close == std::string::npos || close == end + 1) return false;
        char* parsedEnd = nullptr;
        errno = 0;
        const long parsed = std::strtol(path.c_str() + end + 1, &parsedEnd, 10);
        if (errno || parsedEnd != path.c_str() + close || parsed < 0 || parsed > std::numeric_limits<int32_t>::max()) return false;
        Obj element = nullptr;
        eng::Prop inner;
        if (!ArrayValueAt(base, property, static_cast<int32_t>(parsed), &element, &inner)) return false;
        if (close + 1 == path.size()) {
            *resolved = {actor, object, element, "[" + std::to_string(parsed) + "]", inner};
            return true;
        }
        if (path[close + 1] != '.' || eng::KindOf(inner) != "StructProperty" || !eng::StructOf(inner)) return false;
        base = element;
        structure = eng::StructOf(inner);
        at = close + 2;
    }
    return false;
}

}  // namespace

namespace {

std::string LowerAscii(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

}  // namespace

std::vector<PropertyInfo> InspectPropertiesPage(int id, const std::string& filter, size_t offset, size_t limit) {
    std::vector<PropertyInfo> out;
    if (!limit) return out;
    const std::string query = LowerAscii(filter);
    size_t matched = 0;
    for (const PropertyTarget& target : PropertyTargets(Resolve(id))) {
        std::set<std::string> seen;
        for (const std::string& name : eng::PropertyNames(eng::ClassOf(target.object))) {
            if (!seen.insert(name).second) continue;
            const eng::Prop p = eng::FindProp(eng::ClassOf(target.object), name);
            const std::string kind = eng::KindOf(p);
            if (p && !kind.empty()) {
                const std::string path = target.prefix + "." + name;
                // The normal unfiltered selection path skips values outside the requested page entirely. Value text
                // is only needed early when it participates in a filter match.
                if (query.empty() && matched++ < offset) continue;
                const std::string reason = ReadOnlyReason(p, kind);
                const std::string value = DisplayProperty(target.object, name, p, kind);
                if (!query.empty()) {
                    if (LowerAscii(path + " " + kind + " " + value).find(query) == std::string::npos) continue;
                    if (matched++ < offset) continue;
                }
                out.push_back({path, kind, value, false, reason.empty() ? "read-only inspection" : reason});
                if (out.size() >= limit) return out;
            }
        }
    }
    return out;
}

std::vector<PropertyInfo> InspectProperties(int id) {
    return InspectPropertiesPage(id, "", 0, std::numeric_limits<size_t>::max());
}

std::vector<PropertyInfo> InspectPropertyChildren(int id, const std::string& path, size_t limit) {
    std::vector<PropertyInfo> out;
    ResolvedProperty resolved;
    if (!limit || !ResolvePropertyPath(Resolve(id), path, &resolved)) return out;
    const std::string kind = eng::KindOf(resolved.property);
    if (kind == "StructProperty") {
        Obj structure = eng::StructOf(resolved.property);
        Obj base = resolved.base + resolved.property.offset;
        if (!structure || !eng::MemoryReadable(base, static_cast<size_t>(resolved.property.size))) return out;
        std::set<std::string> seen;
        for (const std::string& name : eng::PropertyNames(structure)) {
            if (!seen.insert(name).second) continue;
            const eng::Prop property = eng::FindProp(structure, name);
            const std::string childKind = eng::KindOf(property);
            if (!property || childKind.empty() || property.offset < 0 || property.size <= 0 ||
                property.offset > resolved.property.size || property.size > resolved.property.size - property.offset) continue;
            const std::string reason = ReadOnlyReason(property, childKind);
            out.push_back({path + "." + name, childKind, DisplayProperty(base, name, property, childKind), false,
                           reason.empty() ? "read-only inspection" : reason});
            if (out.size() >= limit) break;
        }
    } else if (kind == "ArrayProperty") {
        struct ArrayValue { Obj data; int32_t num, max; } array{};
        if (!eng::MemoryReadable(resolved.base + resolved.property.offset, sizeof array)) return out;
        std::memcpy(&array, resolved.base + resolved.property.offset, sizeof array);
        const eng::Prop reflectedInner = eng::InnerOf(resolved.property);
        if (!reflectedInner || reflectedInner.size <= 0 || array.num < 0 || array.num > array.max || array.max > 10000000 ||
            (array.num > 0 && !array.data)) return out;
        const size_t count = std::min(limit, static_cast<size_t>(array.num));
        for (size_t i = 0; i < count; ++i) {
            Obj element = nullptr;
            eng::Prop inner;
            if (!ArrayValueAt(resolved.base, resolved.property, static_cast<int32_t>(i), &element, &inner)) break;
            const std::string childKind = eng::KindOf(inner);
            if (childKind.empty()) continue;
            const std::string reason = ReadOnlyReason(inner, childKind);
            out.push_back({path + "[" + std::to_string(i) + "]", childKind,
                           DisplayProperty(element, "", inner, childKind), false,
                           reason.empty() ? "read-only inspection" : reason});
        }
    }
    return out;
}

std::string PropertyObjectName(int id) {
    Obj actor = Resolve(id);
    return actor ? eng::ObjName(actor) : "";
}

int RepairMeshes() {
    int repaired = 0;
    Obj baseItem = eng::FindClass("BP_BaseItem_C");
    for (Obj actor : AllActors()) {
        if (!IsMeshCarrier(actor)) continue;
        Obj component = MeshComponent(actor);
        if (!component) continue;
        if (!eng::ReadObj(component, "StaticMesh") && eng::ClassOf(actor) == baseItem) {
            std::string path = PersistedBaseItemMesh(actor);
            if (path.empty()) path = CachedMeshPath(actor);
            Obj mesh = StaticMesh(path);
            if (mesh && ApplyStaticMesh(component, mesh)) {
                PersistBaseItemMesh(actor, path);
                CacheMesh(actor, path);
                hostlog::Info("editor: restored saved mesh " + path);
                gRepairReported.erase(IdOf(actor));
            } else if (gRepairReported.insert(IdOf(actor)).second) {
                hostlog::Warn("editor: could not restore saved BP_BaseItem mesh; decoded path '" + path + "'");
            }
        } else if (eng::ClassOf(actor) == baseItem) {
            const std::string path = PersistedBaseItemMesh(actor);
            if (!path.empty()) CacheMesh(actor, path);
        }
        if (ReassertMesh(actor, component)) ++repaired;
    }
    return repaired;
}

void AssetBrowserFrame() {
    if (Open()) return;
    gRepairReported.clear();
    gCircuitDecalCache.clear();
    gMeshCache.clear();
}

}  // namespace editor
