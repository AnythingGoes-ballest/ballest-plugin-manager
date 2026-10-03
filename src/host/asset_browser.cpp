#include "editor.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "cosmetics.hpp"
#include "game.hpp"
#include "json.hpp"
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

struct EditedProperty {
    eng::Weak actor;
    std::string path, kind, value;
};
std::vector<EditedProperty> gEditedProperties;
std::map<std::wstring, std::filesystem::file_time_type> gKnownSaveTimes;
std::wstring gActiveSaveFile;
ULONGLONG gNextSaveScan = 0;
int gPersistenceGeneration = -1;
bool NativeActorProperty(const EditedProperty& edited, std::string* arrayName, std::string* propertyType);
void PersistenceFrame();

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
Obj Resolve(int id) {
    Obj actorClass = eng::FindClass("Actor");
    Obj object = id >= 0 && id < eng::NumObjects() ? eng::ObjectAt(id) : nullptr;
    return object && actorClass && eng::IsA(object, actorClass) ? object : nullptr;
}
int IdOf(Obj actor) { return eng::MakeWeak(actor).index; }

struct Quat { double x = 0, y = 0, z = 0, w = 1; };
Quat ToQuat(const Rot& rotation) {
    return eng::Call(Math(), "Conv_RotatorToQuaternion", rotation).ReturnAs<Quat>();
}
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

    // Depending on cooking, a Blueprint path may resolve to UBlueprint or directly to its generated UClass.
    Obj generated = nullptr;
    if (IsAssetA(asset, "Blueprint")) generated = eng::ReadObj(asset, "GeneratedClass");
    else if (IsAssetA(asset, "Class")) generated = asset;
    if (generated && IsSubclassOf(generated, eng::FindClass("Actor"))) {
        Obj actor = SpawnEditorActor(generated);
        if (!actor) return "Could not spawn actor Blueprint: " + assetPath;
        hostlog::Info("editor: spawned actor blueprint " + assetPath);
        return "Spawned " + eng::ObjName(generated) + " (piece " + std::to_string(IdOf(actor)) + ")";
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

    if (IsAssetA(asset, "SoundBase")) {
        Obj statics = eng::FindCdo("GameplayStatics");
        eng::Params p(eng::FunctionOn(statics, "SpawnSoundAtLocation"));
        p.Set("WorldContextObject", Pawn());
        p.Set("Sound", asset);
        p.Set("Location", location);
        p.Set("Rotation", Rot{});
        p.Set("VolumeMultiplier", 1.0f);
        p.Set("PitchMultiplier", 1.0f);
        p.Set("StartTime", 0.0f);
        p.Set("bAutoDestroy", uint8_t{1});
        if (eng::Invoke(statics, p) && p.ReturnObj()) {
            hostlog::Info("editor: previewed sound " + assetPath);
            return "Playing sound " + eng::ObjName(asset) + " (not saved with the level)";
        }
        return "Could not play sound asset: " + assetPath;
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
    Obj asset = assetPath.empty() ? nullptr : cosmetics::LoadAsset(eng::Widen(assetPath));
    return DescribeObject(assetPath, asset);
}

std::string DescribeMesh(const std::string& assetPath) {
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

template <class T>
bool ParseSigned(const std::string& text, T* out) {
    errno = 0; char* end = nullptr;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end || v < static_cast<long long>(std::numeric_limits<T>::min()) ||
        v > static_cast<long long>(std::numeric_limits<T>::max())) return false;
    *out = static_cast<T>(v); return true;
}

template <class T>
bool ParseUnsigned(const std::string& text, T* out) {
    if (!text.empty() && text[0] == '-') return false;
    errno = 0; char* end = nullptr;
    const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end || v > static_cast<unsigned long long>(std::numeric_limits<T>::max())) return false;
    *out = static_cast<T>(v); return true;
}

bool ParseNumber(const std::string& text, double* out) {
    errno = 0; char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (errno || end == text.c_str() || *end || !std::isfinite(v)) return false;
    *out = v; return true;
}

std::vector<std::string> SplitComponents(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        const size_t end = comma == std::string::npos ? text.size() : comma;
        size_t first = text.find_first_not_of(" \t", start), last = text.find_last_not_of(" \t", end ? end - 1 : 0);
        if (first == std::string::npos || first >= end || last == std::string::npos) return {};
        out.push_back(text.substr(first, last - first + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

template <class T, size_t N>
bool ParseRealComponents(const std::string& text, std::array<T, N>* out) {
    const auto fields = SplitComponents(text);
    if (fields.size() != N) return false;
    for (size_t i = 0; i < N; ++i) {
        double value = 0;
        if (!ParseNumber(fields[i], &value) || std::fabs(value) > static_cast<double>(std::numeric_limits<T>::max())) return false;
        (*out)[i] = static_cast<T>(value);
    }
    return true;
}

template <class T, size_t N>
bool ParseIntegerComponents(const std::string& text, std::array<T, N>* out) {
    const auto fields = SplitComponents(text);
    if (fields.size() != N) return false;
    for (size_t i = 0; i < N; ++i)
        if constexpr (std::is_signed_v<T>) {
            if (!ParseSigned(fields[i], &(*out)[i])) return false;
        } else if (!ParseUnsigned(fields[i], &(*out)[i])) return false;
    return true;
}

bool WritePropertyBytes(Obj base, const eng::Prop& property, const void* value, size_t size) {
    if (!base || !property || property.size != static_cast<int32_t>(size) ||
        !eng::MemoryReadable(base + property.offset, size)) return false;
    std::memcpy(base + property.offset, value, size);
    return true;
}

bool WriteNumeric(Obj object, const eng::Prop& property, const std::string& kind, const std::string& value) {
    if (kind == "ByteProperty") { uint8_t v{}; return ParseUnsigned(value, &v) && WritePropertyBytes(object, property, &v, sizeof v); }
    if (kind == "Int8Property") { int8_t v{}; return ParseSigned(value, &v) && WritePropertyBytes(object, property, &v, sizeof v); }
    if (kind == "Int16Property") { int16_t v{}; return ParseSigned(value, &v) && WritePropertyBytes(object, property, &v, sizeof v); }
    if (kind == "IntProperty") { int32_t v{}; return ParseSigned(value, &v) && WritePropertyBytes(object, property, &v, sizeof v); }
    if (kind == "Int64Property") { int64_t v{}; return ParseSigned(value, &v) && WritePropertyBytes(object, property, &v, sizeof v); }
    if (kind == "UInt16Property") { uint16_t v{}; return ParseUnsigned(value, &v) && WritePropertyBytes(object, property, &v, sizeof v); }
    if (kind == "UInt32Property") { uint32_t v{}; return ParseUnsigned(value, &v) && WritePropertyBytes(object, property, &v, sizeof v); }
    if (kind == "UInt64Property") { uint64_t v{}; return ParseUnsigned(value, &v) && WritePropertyBytes(object, property, &v, sizeof v); }
    if (kind == "FloatProperty") {
        double n{};
        if (!ParseNumber(value, &n) || std::fabs(n) > std::numeric_limits<float>::max()) return false;
        const float v = static_cast<float>(n);
        return WritePropertyBytes(object, property, &v, sizeof v);
    }
    if (kind == "DoubleProperty") { double v{}; return ParseNumber(value, &v) && WritePropertyBytes(object, property, &v, sizeof v); }
    return false;
}

bool WriteStruct(Obj object, const eng::Prop& property, const std::string& value) {
    const std::string type = eng::ObjName(eng::StructOf(property));
    if ((type == "Vector" || type == "Rotator") && property.size == 24) { std::array<double, 3> v{}; return ParseRealComponents(value, &v) && WritePropertyBytes(object, property, v.data(), sizeof v); }
    if (type == "Vector2D" && property.size == 16) { std::array<double, 2> v{}; return ParseRealComponents(value, &v) && WritePropertyBytes(object, property, v.data(), sizeof v); }
    if ((type == "Vector4" || type == "Quat") && property.size == 32) { std::array<double, 4> v{}; return ParseRealComponents(value, &v) && WritePropertyBytes(object, property, v.data(), sizeof v); }
    if (type == "LinearColor" && property.size == 16) { std::array<float, 4> v{}; return ParseRealComponents(value, &v) && WritePropertyBytes(object, property, v.data(), sizeof v); }
    if (type == "Color" && property.size == 4) { std::array<uint8_t, 4> v{}; return ParseIntegerComponents(value, &v) && WritePropertyBytes(object, property, v.data(), sizeof v); }
    if (type == "IntPoint" && property.size == 8) { std::array<int32_t, 2> v{}; return ParseIntegerComponents(value, &v) && WritePropertyBytes(object, property, v.data(), sizeof v); }
    if (type == "IntVector" && property.size == 12) { std::array<int32_t, 3> v{}; return ParseIntegerComponents(value, &v) && WritePropertyBytes(object, property, v.data(), sizeof v); }
    if (type == "IntVector4" && property.size == 16) { std::array<int32_t, 4> v{}; return ParseIntegerComponents(value, &v) && WritePropertyBytes(object, property, v.data(), sizeof v); }
    return false;
}

bool ClassDerivesFrom(Obj candidate, Obj required) {
    if (!candidate || !required) return candidate == required;
    for (Obj cls = candidate; cls; cls = eng::SuperOf(cls))
        if (cls == required) return true;
    return false;
}

bool WriteReference(Obj object, const eng::Prop& property, const std::string& value) {
    Obj referenced = nullptr;
    if (value != "None" && value != "none" && value != "null" && value != "0") {
        referenced = eng::FindObjectByPath(value);
        if (!referenced) return false;
        const std::string kind = eng::KindOf(property);
        if (kind == "ObjectProperty") {
            if (!eng::IsA(referenced, eng::ObjectClassOf(property))) return false;
        } else if (kind == "ClassProperty") {
            Obj classClass = eng::FindClass("Class");
            if (!classClass || !eng::IsA(referenced, classClass) || !ClassDerivesFrom(referenced, eng::ClassMetaOf(property))) return false;
        } else {
            return false;
        }
    }
    return WritePropertyBytes(object, property, &referenced, sizeof referenced);
}

bool WriteName(Obj object, const eng::Prop& property, const std::string& value) {
    const std::wstring wide = eng::Widen(value);
    const eng::FString input{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
    struct FNameValue { uint32_t index; int32_t number; } converted{};
    const eng::Params result = eng::Call(eng::FindCdo("KismetStringLibrary"), "Conv_StringToName", input);
    size_t size = 0;
    const uint8_t* returned = result.Return(&size);
    if (!result.Invoked() || !returned || size != sizeof converted) return false;
    std::memcpy(&converted, returned, sizeof converted);
    return WritePropertyBytes(object, property, &converted, sizeof converted);
}

bool WriteString(Obj object, const eng::Prop& property, const std::string& value) {
    const std::wstring wide = eng::Widen(value), empty;
    const eng::FString input{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
    const eng::FString suffix{empty.c_str(), 1, 1};
    const eng::Params owned = eng::Call(eng::FindCdo("KismetStringLibrary"), "Concat_StrStr", input, suffix);
    size_t size = 0;
    const uint8_t* returned = owned.Return(&size);
    // Concat_StrStr gives the result its own allocation. Transfer that value into the property instead of pointing the
    // property at the temporary input buffer. The previous allocation is intentionally retained because this header-only
    // host has no safe reflected FString destructor; leaking on an explicit edit is preferable to freeing unknown memory.
    return owned.Invoked() && returned && size == 16 && WritePropertyBytes(object, property, returned, size);
}

bool WriteText(Obj object, const eng::Prop& property, const std::string& value) {
    const eng::Params text = eng::MakeText(value);
    size_t size = 0;
    const uint8_t* returned = text.Return(&size);
    // Transfer the conversion result's reference to the property. Do not ReleaseText here: the property now owns it.
    return text.Invoked() && returned && size == 16 && WritePropertyBytes(object, property, returned, size);
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
                // Asset Browser intentionally exposes an input for every reflected field. `SetProperty` still validates
                // the exact type and returns false when no safe text assignment exists for that representation.
                out.push_back({path, kind, value, true, reason});
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
            out.push_back({path + "." + name, childKind, DisplayProperty(base, name, property, childKind), true,
                           ReadOnlyReason(property, childKind)});
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
            out.push_back({path + "[" + std::to_string(i) + "]", childKind,
                           DisplayProperty(element, "", inner, childKind), true, ReadOnlyReason(inner, childKind)});
        }
    }
    return out;
}

std::string PropertyObjectName(int id) {
    Obj actor = Resolve(id);
    return actor ? eng::ObjName(actor) : "";
}

bool SetProperty(int id, const std::string& path, const std::string& value) {
    ResolvedProperty resolved;
    if (!ResolvePropertyPath(Resolve(id), path, &resolved)) return false;
    const eng::Prop& p = resolved.property;
    const std::string kind = eng::KindOf(p);
    if ((eng::FlagsOf(p) & (layout::kPropertyFlagIsParameter | layout::kPropertyFlagIsReturnValue)) != 0) return false;
    bool changed = false;
    if (kind == "BoolProperty") {
        bool v = false;
        if (value == "true" || value == "1" || value == "on" || value == "yes") v = true;
        else if (value != "false" && value != "0" && value != "off" && value != "no") return false;
        changed = eng::WriteBoolValue(resolved.base, p, v);
    } else if (NumericKind(kind)) {
        changed = WriteNumeric(resolved.base, p, kind, value);
    } else if (kind == "EnumProperty") {
        const eng::Prop underlying = eng::EnumUnderlyingOf(p);
        changed = underlying && underlying.size == p.size && WriteNumeric(resolved.base, p, eng::KindOf(underlying), value);
    } else if (kind == "NameProperty") {
        changed = WriteName(resolved.base, p, value);
    } else if (kind == "StrProperty") {
        changed = WriteString(resolved.base, p, value);
    } else if (kind == "TextProperty") {
        changed = WriteText(resolved.base, p, value);
    } else if (kind == "ObjectProperty" || kind == "ClassProperty") {
        changed = WriteReference(resolved.base, p, value);
    } else if (kind == "StructProperty") {
        changed = WriteStruct(resolved.base, p, value);
    }
    if (changed) {
        if (eng::FindFunction(eng::ClassOf(resolved.owner), "MarkRenderStateDirty")) eng::Call(resolved.owner, "MarkRenderStateDirty");
        if (eng::FindFunction(eng::ClassOf(resolved.owner), "UpdateComponentToWorld")) eng::Call(resolved.owner, "UpdateComponentToWorld");
        hostlog::Info("editor: set " + path + " = " + value + " on " + eng::ObjName(resolved.actor));
    }
    return changed;
}

bool SetEditedProperty(int id, const std::string& path, const std::string& value) {
    if (!SetProperty(id, path, value)) return false;
    Obj actor = Resolve(id);
    ResolvedProperty resolved;
    if (!actor || !ResolvePropertyPath(actor, path, &resolved)) return false;
    const EditedProperty submitted{eng::MakeWeak(actor), path, eng::KindOf(resolved.property),
                                   DisplayProperty(resolved.base, resolved.name, resolved.property,
                                                   eng::KindOf(resolved.property))};
    std::string arrayName, propertyType;
    if (!NativeActorProperty(submitted, &arrayName, &propertyType)) {
        hostlog::Info("editor: applied runtime-only property " + path +
                      "; Ballest native saves support only top-level actor Bool/Float/Int/Text/Name/String fields");
        return true;
    }
    for (EditedProperty& edited : gEditedProperties)
        if (eng::Get(edited.actor) == actor && edited.path == path) {
            edited = submitted;
            return true;
        }
    gEditedProperties.push_back(submitted);
    return true;
}

namespace {

std::wstring UserSavedMapsDir() {
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return (length ? std::wstring(local, length) : std::wstring(L".")) + L"\\Ballest\\Saved\\UserSavedMaps";
}

bool ReadFile(const std::wstring& path, std::string* out) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return false;
    out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return static_cast<bool>(file) || file.eof();
}

bool WriteFileAtomically(const std::wstring& path, const std::string& text) {
    const std::wstring temporary = path + L".asset-browser.tmp";
    {
        std::ofstream file(temporary.c_str(), std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

json::Value StringValue(const std::string& value) {
    json::Value out;
    out.type = json::Value::String;
    out.string = value;
    return out;
}

json::Value NumberValue(double value) {
    json::Value out;
    out.type = json::Value::Number;
    out.number = value;
    return out;
}

json::Value BoolValue(bool value) {
    json::Value out;
    out.type = json::Value::Bool;
    out.boolean = value;
    return out;
}

json::Value* EnsureArray(json::Value& object, const std::string& name) {
    if (json::Value* existing = object.Get(name)) {
        if (existing->type != json::Value::Array) {
            existing->type = json::Value::Array;
            existing->items.clear();
            existing->members.clear();
        }
        return existing;
    }
    json::Value array;
    array.type = json::Value::Array;
    object.members.emplace_back(name, std::move(array));
    return &object.members.back().second;
}

const json::Value* Member(const json::Value& object, const std::string& name, json::Value::Type type) {
    const json::Value* value = object.Get(name);
    return value && value->type == type ? value : nullptr;
}

bool NumberMember(const json::Value& object, const std::string& name, double* out) {
    const json::Value* value = Member(object, name, json::Value::Number);
    if (!value) return false;
    *out = value->number;
    return true;
}

std::string NormalizedClassPath(std::string path) {
    const size_t quote = path.find('\'');
    if (quote != std::string::npos) path.erase(0, quote + 1);
    if (!path.empty() && path.back() == '\'') path.pop_back();
    return path;
}

bool ItemMatchesActor(const json::Value& item, Obj actor) {
    if (!actor || item.type != json::Value::Object) return false;
    const json::Value* actorToSpawn = Member(item, "actorToSpawn", json::Value::String);
    const json::Value* transform = Member(item, "itemTransform", json::Value::Object);
    const json::Value* translation = transform ? Member(*transform, "translation", json::Value::Object) : nullptr;
    const json::Value* rotation = transform ? Member(*transform, "rotation", json::Value::Object) : nullptr;
    const json::Value* scale = transform ? Member(*transform, "scale3D", json::Value::Object) : nullptr;
    if (!actorToSpawn || !translation || !rotation || !scale ||
        NormalizedClassPath(actorToSpawn->string) != eng::PathOf(eng::ClassOf(actor))) return false;
    double tx = 0, ty = 0, tz = 0, rx = 0, ry = 0, rz = 0, rw = 1, sx = 1, sy = 1, sz = 1;
    if (!NumberMember(*translation, "x", &tx) || !NumberMember(*translation, "y", &ty) ||
        !NumberMember(*translation, "z", &tz) || !NumberMember(*rotation, "x", &rx) ||
        !NumberMember(*rotation, "y", &ry) || !NumberMember(*rotation, "z", &rz) ||
        !NumberMember(*rotation, "w", &rw) || !NumberMember(*scale, "x", &sx) ||
        !NumberMember(*scale, "y", &sy) || !NumberMember(*scale, "z", &sz)) return false;
    const Vec3 liveLocation = eng::Call(actor, "K2_GetActorLocation").ReturnAs<Vec3>();
    const Vec3 liveScale = eng::Call(actor, "GetActorScale3D").ReturnAs<Vec3>();
    const Quat liveRotation = ToQuat(eng::Call(actor, "K2_GetActorRotation").ReturnAs<Rot>());
    const double dot = std::fabs(liveRotation.x * rx + liveRotation.y * ry + liveRotation.z * rz + liveRotation.w * rw);
    return Near(liveLocation, {tx, ty, tz}, 0.1) && Near(liveScale, {sx, sy, sz}, 0.001) && dot > 0.99999;
}

json::Value* MatchingItem(json::Value& root, Obj actor) {
    json::Value* items = root.Get("items");
    if (!items || items->type != json::Value::Array) return nullptr;
    for (json::Value& item : items->items)
        if (ItemMatchesActor(item, actor)) return &item;
    return nullptr;
}

bool RemoveMember(json::Value& object, const std::string& name) {
    for (auto member = object.members.begin(); member != object.members.end(); ++member) {
        if (member->first != name) continue;
        object.members.erase(member);
        return true;
    }
    return false;
}

bool NativeActorProperty(const EditedProperty& edited, std::string* arrayName, std::string* propertyType) {
    if (edited.path.rfind("Actor.", 0) != 0 || edited.path.find('.', 6) != std::string::npos ||
        edited.path.find('[', 6) != std::string::npos) return false;
    if (edited.kind == "BoolProperty") { *arrayName = "boolProperties"; *propertyType = "Bool"; return true; }
    if (edited.kind == "FloatProperty" || edited.kind == "DoubleProperty") {
        *arrayName = "floatProperties"; *propertyType = "Float"; return true;
    }
    if (edited.kind == "ByteProperty" || edited.kind == "Int8Property" || edited.kind == "Int16Property" ||
        edited.kind == "IntProperty" || edited.kind == "UInt16Property" || edited.kind == "EnumProperty") {
        *arrayName = "intProperties"; *propertyType = "Int"; return true;
    }
    if (edited.kind == "TextProperty") { *arrayName = "textProperties"; *propertyType = "Text"; return true; }
    if (edited.kind == "NameProperty") { *arrayName = "nameProperties"; *propertyType = "Name"; return true; }
    if (edited.kind == "StrProperty") { *arrayName = "stringProperties"; *propertyType = "String"; return true; }
    return false;
}

bool UpsertNativeProperty(json::Value& item, const EditedProperty& edited) {
    std::string arrayName, propertyType;
    if (!NativeActorProperty(edited, &arrayName, &propertyType)) return false;
    json::Value* properties = EnsureArray(item, arrayName);
    const std::string name = edited.path.substr(6);
    json::Value current;
    if (edited.kind == "BoolProperty") {
        current = BoolValue(edited.value == "true");
    } else if (arrayName == "floatProperties" || arrayName == "intProperties") {
        double number = 0;
        if (!ParseNumber(edited.value, &number)) return false;
        current = NumberValue(number);
    } else {
        current = StringValue(edited.value);
    }
    for (json::Value& entry : properties->items) {
        if (entry.type != json::Value::Object || entry.Str("propertyName") != name) continue;
        if (json::Value* value = entry.Get("currentValue")) *value = current;
        else entry.members.emplace_back("currentValue", current);
        return true;
    }
    json::Value entry;
    entry.type = json::Value::Object;
    entry.members.emplace_back("currentValue", current);
    entry.members.emplace_back("propertyName", StringValue(name));
    entry.members.emplace_back("propertyType", StringValue(propertyType));
    properties->items.push_back(std::move(entry));
    return true;
}

bool PatchSavedFile(const std::wstring& path) {
    std::string text, error;
    json::Value root;
    if (!ReadFile(path, &text) || !json::Parse(text, root, error) || root.type != json::Value::Object) {
        hostlog::Warn("editor: could not read saved property data: " + error);
        return false;
    }
    bool changed = false;
    for (EditedProperty& edited : gEditedProperties) {
        Obj actor = eng::Get(edited.actor);
        ResolvedProperty resolved;
        if (!actor || !ResolvePropertyPath(actor, edited.path, &resolved)) continue;
        edited.kind = eng::KindOf(resolved.property);
        edited.value = DisplayProperty(resolved.base, resolved.name, resolved.property, edited.kind);
        json::Value* item = MatchingItem(root, actor);
        if (!item) continue;
        changed = RemoveMember(*item, "unrealProperties") || changed;
        changed = UpsertNativeProperty(*item, edited) || changed;
    }
    if (!changed) return true;
    if (!WriteFileAtomically(path, json::Stringify(root))) {
        hostlog::Warn("editor: could not update Ballest native property arrays");
        return false;
    }
    hostlog::Info("editor: wrote user-edited properties to Ballest native arrays in " +
                  eng::Narrow(path.c_str(), static_cast<int>(path.size())));
    return true;
}

std::vector<std::wstring> SavedFiles() {
    std::vector<std::wstring> out;
    const std::wstring pattern = UserSavedMapsDir() + L"\\*.balledit";
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(UserSavedMapsDir() + L"\\" + found.cFileName);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return out;
}

std::string CurrentMapName() {
    Obj cls = eng::FindClass("W_SaveLoad_C");
    std::string fallback;
    eng::ForEachObject([&](Obj object) {
        if (eng::ClassOf(object) != cls || eng::IsDefaultObject(object) || !eng::IsLive(object)) return true;
        eng::Params current = eng::Call(object, "GetCurrentMapName");
        size_t size = 0;
        const uint8_t* name = current.Get("MapName", &size);
        if (current.Invoked() && name && size == 16) {
            const std::string value = eng::ReadFString(name);
            if (!value.empty()) {
                fallback = value;
            }
        }
        for (const char* property : {"LastSavedLevelName", "TargetLevel"}) {
            const eng::Prop p = eng::FindProp(eng::ClassOf(object), property);
            if (eng::KindOf(p) != "StrProperty" || p.size != 16) continue;
            const std::string value = eng::ReadFString(object + p.offset);
            if (!value.empty()) fallback = value;
        }
        return true;
    });
    return fallback;
}

std::wstring SaveFileForCurrentMap() {
    const std::string current = CurrentMapName();
    if (current.empty()) return gActiveSaveFile;
    const std::wstring wanted = eng::Widen(current);
    for (const std::wstring& file : SavedFiles()) {
        const size_t slash = file.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos ? file : file.substr(slash + 1);
        if (name == wanted || name == wanted + L".balledit" || name.find(L"&" + wanted) != std::wstring::npos)
            return file;
    }
    return gActiveSaveFile;
}

void PersistenceFrame() {
    if (gPersistenceGeneration != game::Generation()) {
        gPersistenceGeneration = game::Generation();
        gEditedProperties.clear();
        gActiveSaveFile.clear();
        gKnownSaveTimes.clear();
        for (const std::wstring& file : SavedFiles()) {
            std::error_code error;
            gKnownSaveTimes[file] = std::filesystem::last_write_time(file, error);
        }
    }
    if (GetTickCount64() >= gNextSaveScan) {
        const std::wstring current = SaveFileForCurrentMap();
        if (!current.empty() && current != gActiveSaveFile) {
            gActiveSaveFile = current;
            hostlog::Info("editor: active saved map " + eng::Narrow(current.c_str(), static_cast<int>(current.size())));
        }
    }
    if (GetTickCount64() < gNextSaveScan) return;
    gNextSaveScan = GetTickCount64() + 500;
    for (const std::wstring& file : SavedFiles()) {
        std::error_code error;
        const auto written = std::filesystem::last_write_time(file, error);
        if (error) continue;
        const auto known = gKnownSaveTimes.find(file);
        if (known == gKnownSaveTimes.end()) {
            gKnownSaveTimes[file] = written;
            gActiveSaveFile = file;
            if (!gEditedProperties.empty() && PatchSavedFile(file)) {
                std::error_code refreshedError;
                gKnownSaveTimes[file] = std::filesystem::last_write_time(file, refreshedError);
            }
            continue;
        }
        if (written == known->second) continue;
        known->second = written;
        gActiveSaveFile = file;
        if (!gEditedProperties.empty() && PatchSavedFile(file)) {
            std::error_code refreshedError;
            known->second = std::filesystem::last_write_time(file, refreshedError);
        }
    }
}

}  // namespace

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
    if (Open()) {
        PersistenceFrame();
        return;
    }
    gRepairReported.clear();
    gCircuitDecalCache.clear();
    gMeshCache.clear();
}

}  // namespace editor
