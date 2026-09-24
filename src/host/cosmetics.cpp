#include "cosmetics.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "game.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "models.hpp"
#include "widgets.hpp"

using eng::Obj;
using eng::Params;
namespace w = ui::widgets;

namespace cosmetics {
namespace {

constexpr int kColumns = 4;                         // tiles per row, as the collection section
const wchar_t* kImageBallMaterial = L"/Game/Art/DataAssets/Skins/LBall/MI_LBall05.MI_LBall05";
const wchar_t* kWhite = L"/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture";
const wchar_t* kFlatNormal = L"/Engine/EngineMaterials/BaseFlattenNormalMap.BaseFlattenNormalMap";
const wchar_t* kGhostHatSource = L"/Game/Art/DataAssets/Accessories/PartyHat/DA_Accessory_PartyHat.DA_Accessory_PartyHat";
const wchar_t* kSphereMesh = L"/Engine/BasicShapes/Sphere.Sphere";

const char* const kAssetClass[3] = {"PDA_BallSkin_C", "PDA_Accessory_C", "PDA_GoalExplo_C"};
const char* const kToSave[3] = {"SkinToSave", "AccessoryToSave", "GoalExploToSave"};    // the page's pending choice

struct Custom {
    Kind kind = Kind::Ball;
    std::string id;
    eng::Weak asset;                                // the data asset (kept alive)
    eng::Weak material;                             // balls: the dynamic material with the image
    eng::Weak mesh;                                 // hats: the mesh
    double scale = 1;                               // hats and bfx: the size
    bool hasModel = false;                          // balls and hats: parts with depth or movement, built on the ball
    models::Model model;
};
std::vector<Custom> gCustoms;
struct Request {                                    // made before the game was ready (no player controller yet)
    Kind kind;
    std::string id, name, what;                     // what: the image (balls), mesh (hats) or base explosion (bfx)
    std::wstring preview;
    double scale;
    std::string model, system, sound;
};
std::vector<Request> gWaiting;
std::string gEquipped[3];                           // the custom cosmetic worn, per kind, or empty

// The section on the page, rebuilt when the page or its tab changes.
eng::Weak gPage, gHeader, gBorder, gGrid;
std::vector<eng::Weak> gTiles;
uint8_t gTileVisibility = 4;
int gSectionTab = -1;

struct ArrayHeader {
    void* data;
    int32_t num, max;
};

Obj Library(const char* name) { return eng::FindCdo(name); }

eng::FString Str(const std::wstring& s) { return {s.c_str(), static_cast<int32_t>(s.size() + 1), static_cast<int32_t>(s.size() + 1)}; }

struct Name {
    uint8_t bytes[8];
};
Name MakeName(const std::string& s) {
    const std::wstring w = eng::Widen(s);
    Name n{};
    const Params p = eng::Call(Library("KismetStringLibrary"), "Conv_StringToName", Str(w));
    if (const uint8_t* r = p.Return()) std::memcpy(n.bytes, r, sizeof n.bytes);
    return n;
}

// An array whose memory comes from the engine's own allocator, which is what frees it when its owner goes: a string
// the engine pads to the right length donates its buffer (LeftPad to 4n - 1 characters is 8n bytes with the
// terminator, room for n pointers).
ArrayHeader EngineArray(int count) {
    ArrayHeader a{nullptr, 0, 0};
    if (count <= 0) return a;
    const std::wstring empty;
    const Params p = eng::Call(Library("KismetStringLibrary"), "LeftPad", Str(empty), static_cast<int32_t>(4 * count - 1));
    size_t size = 0;
    const uint8_t* r = p.Return(&size);
    if (!r || size != 16) return a;
    std::memcpy(&a.data, r, sizeof a.data);         // the string itself is never freed: its buffer is the array now
    if (a.data) {
        std::memset(a.data, 0, static_cast<size_t>(count) * 8);
        a.num = a.max = count;
    }
    return a;
}

Obj Spawn(Obj cls, Obj outer) {
    return cls && outer ? eng::Call(Library("GameplayStatics"), "SpawnObject", cls, outer).ReturnObj() : nullptr;
}

bool SetObject(Obj o, const char* property, Obj value) { return eng::WriteBytes(o, property, &value, sizeof value); }

// Struct and value properties copied byte for byte (brushes, margins, sizes: no memory of their own).
void CopyProperty(Obj from, Obj to, const char* property) {
    const eng::Prop p = eng::FindProp(eng::ClassOf(from), property);
    if (!p || !from || !to) return;
    std::vector<uint8_t> bytes(static_cast<size_t>(p.size));
    if (eng::ReadBytes(from, property, bytes.data(), bytes.size())) eng::WriteBytes(to, property, bytes.data(), bytes.size());
}

// A slot's layout copied from another slot of the same kind through its setters: a slot added to a panel that is
// already on screen has its Slate slot built at once, so written properties alone would not show.
void CopySlot(Obj from, Obj to) {
    struct Margin {
        uint8_t bytes[16];
    } margin;
    struct ChildSize {
        uint8_t bytes[8];
    } size;
    uint8_t align = 0;
    if (!from || !to || eng::ClassOf(from) != eng::ClassOf(to)) return;
    Obj cls = eng::ClassOf(from);
    if (eng::FindProp(cls, "Padding") && eng::ReadBytes(from, "Padding", &margin, sizeof margin)) eng::Call(to, "SetPadding", margin);
    if (eng::ReadBytes(from, "HorizontalAlignment", &align, 1)) eng::Call(to, "SetHorizontalAlignment", align);
    if (eng::ReadBytes(from, "VerticalAlignment", &align, 1)) eng::Call(to, "SetVerticalAlignment", align);
    if (eng::FindFunction(cls, "SetSize") && eng::ReadBytes(from, "Size", &size, sizeof size)) eng::Call(to, "SetSize", size);
}

Obj GameInstance() {
    Obj controller = game::PlayerController();
    return controller ? eng::Call(Library("GameplayStatics"), "GetGameInstance", controller).ReturnObj() : nullptr;
}

Obj Texture(const std::wstring& file) {
    if (file.empty()) return nullptr;
    Obj t = eng::Call(Library("KismetRenderingLibrary"), "ImportFileAsTexture2D", game::PlayerController(), Str(file)).ReturnObj();
    if (t) KeepAlive(t);
    return t;
}

void SetVector(Obj material, const std::string& parameter, float r, float g, float b, float a) {
    Params p(eng::FunctionOn(material, "SetVectorParameterValue"));
    const Name name = MakeName(parameter);
    const float value[4] = {r, g, b, a};
    p.Set("ParameterName", name);
    p.Set("Value", value);
    eng::Invoke(material, p);
}

void SetTexture(Obj material, const std::string& parameter, Obj texture) {
    Params p(eng::FunctionOn(material, "SetTextureParameterValue"));
    const Name name = MakeName(parameter);
    p.Set("ParameterName", name);
    p.Set("Value", texture);
    eng::Invoke(material, p);
}

// The game's skin that uses a material, for copying what kind of ball it is.
Obj SkinUsing(Obj material) {
    Obj cls = eng::FindClass("PDA_BallSkin_C"), found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::ReadObj(o, "SkinMaterial") == material) found = o;
        return found == nullptr;
    });
    return found;
}

Obj Create(Kind kind, const std::string& id, const std::string& name, Obj preview) {
    const char* cls = kind == Kind::Ball ? "PDA_BallSkin_C" : kind == Kind::Hat ? "PDA_Accessory_C" : "PDA_GoalExplo_C";
    Obj asset = Spawn(eng::FindClass(cls), GameInstance());
    if (!asset) {
        hostlog::Warn(std::string("cosmetics: could not make a ") + cls + " for " + id);
        return nullptr;
    }
    KeepAlive(asset);
    const Name n = MakeName(name);
    eng::WriteBytes(asset, "CosmeticName", &n, sizeof n);
    if (preview) SetObject(asset, "PreviewTexture", preview);
    return asset;
}

void Added(Custom custom) {
    const Kind kind = custom.kind;
    const std::string id = custom.id;
    gCustoms.push_back(std::move(custom));
    gSectionTab = -1;                               // rebuild the section with it
    hostlog::Info("cosmetics: added " + std::string(kind == Kind::Ball ? "ball " : kind == Kind::Hat ? "hat " : "bfx ") + id);
}

const Custom* Find(Kind kind, const std::string& id) {
    for (const auto& c : gCustoms)
        if (c.kind == kind && c.id == id && eng::Get(c.asset)) return &c;
    return nullptr;
}
bool Exists(Kind kind, const std::string& id) {
    for (const auto& r : gWaiting)
        if (r.kind == kind && r.id == id) return true;
    return Find(kind, id) != nullptr;
}

bool Wait(Kind kind, const std::string& id, const std::string& name, const std::string& what, const std::wstring& preview,
          double scale, const std::string& model, const std::string& system = "", const std::string& sound = "") {
    if (game::PlayerController()) return false;
    gWaiting.push_back({kind, id, name, what, preview, scale, model, system, sound});
    return true;
}

bool ParseModel(Custom* c, const std::string& text) {
    if (text.empty()) return true;
    std::string error;
    if (!models::Parse(text, &c->model, &error)) {
        hostlog::Warn("cosmetics: " + c->id + ": the model is not valid (" + error + ")");
        return false;
    }
    c->hasModel = true;
    return true;
}

const Custom* ByAsset(Obj asset) {
    for (const auto& c : gCustoms)
        if (asset && eng::Get(c.asset) == asset) return &c;
    return nullptr;
}

const Custom* Worn(Kind kind) {
    const std::string& id = gEquipped[static_cast<int>(kind)];
    return id.empty() ? nullptr : Find(kind, id);
}

// A copy of a multicast delegate's bindings (TArray<FScriptDelegate>, 16 bytes each) from one widget to another, in a
// buffer of the engine's own: the page binds each of its tile buttons to its handlers, and a new button bound the same
// way is handled the same way.
void CopyBindings(Obj from, Obj to, const char* property) {
    ArrayHeader bindings{nullptr, 0, 0};
    if (!eng::ReadBytes(from, property, &bindings, sizeof bindings) || bindings.num <= 0) return;
    ArrayHeader copy = EngineArray(bindings.num * 2);
    if (!copy.data) return;
    std::memcpy(copy.data, bindings.data, static_cast<size_t>(bindings.num) * 16);
    copy.num = copy.max = bindings.num;
    eng::WriteBytes(to, property, &copy, sizeof copy);
}

// --- the custom section on the Customize page ---------------------------------------------------------------------
Obj LivePage() {
    Obj cls = eng::FindClass("WBP_1_CustomizeColor_C"), found = nullptr;
    eng::ForEachObject([&](Obj o) {
        if (eng::ClassOf(o) == cls && !eng::IsDefaultObject(o) && eng::PathOf(o).rfind("/Engine/Transient", 0) == 0) found = o;
        return found == nullptr;
    });
    return found;
}

const char* const kGrids[4] = {"CollectionCosmetics_Grid", "SpecialCosmetics_Grid", "MedalCosmetics_Grid", "BasicCosmetics_Grid"};

// Every tile the page placed in its own sections.
template <class F>
void ForEachGameTile(Obj page, F&& f) {
    Obj buttonClass = eng::FindClass("WBP_CustomizeButton_C");
    for (const char* name : kGrids) {
        Obj grid = eng::ReadObj(page, name);
        const int32_t n = grid ? eng::Call(grid, "GetChildrenCount").ReturnAs<int32_t>(0) : 0;
        for (int32_t i = 0; i < n; ++i)
            if (Obj tile = eng::Call(grid, "GetChildAt", i).ReturnObj(); tile && eng::ClassOf(tile) == buttonClass) f(tile);
    }
}

// A tile the page shows on this tab for a cosmetic of this kind.
Obj ShownTile(Obj page, Kind kind) {
    Obj found = nullptr, cls = eng::FindClass(kAssetClass[static_cast<int>(kind)]);
    ForEachGameTile(page, [&](Obj tile) {
        if (found || eng::Call(tile, "GetVisibility").ReturnAs<uint8_t>(w::kCollapsed) == w::kCollapsed) return;
        if (eng::IsA(eng::ReadObj(tile, "CosmeticData"), cls)) found = tile;
    });
    return found;
}

void RemoveSection() {
    for (eng::Weak* weak : {&gHeader, &gBorder})
        if (Obj widget = eng::Get(*weak)) eng::Call(widget, "RemoveFromParent");
    gHeader = gBorder = gGrid = {};
}

void BuildSection(Obj page, int tab) {
    RemoveSection();
    gPage = eng::MakeWeak(page);
    gSectionTab = tab;
    if (Count(static_cast<Kind>(tab)) == 0) return;
    Obj tree = eng::ReadObj(page, "WidgetTree");
    Obj refHeader = eng::ReadObj(page, "HB_CollectionSubheader"), refText = eng::ReadObj(page, "CollectionCategory_Text");
    Obj refGrid = eng::ReadObj(page, "CollectionCosmetics_Grid");
    Obj refBorder = eng::Call(refGrid, "GetParent").ReturnObj();
    Obj scroll = eng::Call(refHeader, "GetParent").ReturnObj();
    if (!tree || !refHeader || !refText || !refGrid || !refBorder || !scroll) {
        hostlog::Warn("cosmetics: the Customize page is not laid out as measured; no custom section");
        return;
    }
    // The tiles copy one the page shows for the same kind of cosmetic (tiles differ per tab: measured Type 1 for
    // collection balls, 3 for goal explosions). Until the page has shown its tiles there is none: tried again later.
    Obj refButton = ShownTile(page, static_cast<Kind>(tab));
    if (!refButton) return;
    // Header: the collection header's widgets (its accent image and its text), with "custom" as the text.
    Obj header = w::Spawn("HorizontalBox", tree);
    for (Obj slot : eng::ReadObjArray(refHeader, "Slots")) {
        Obj content = eng::ReadObj(slot, "Content");
        Obj copy = nullptr;
        if (content == refText) {
            copy = w::Spawn("TextBlock", tree);
            w::CopyFont(refText, copy);
            w::SetText(copy, "custom");
        } else if (eng::IsA(content, eng::FindClass("Image"))) {
            copy = w::Spawn("Image", tree);
            CopyProperty(content, copy, "Brush");
            CopyProperty(content, copy, "ColorAndOpacity");
        }
        if (!copy) continue;
        CopySlot(slot, eng::Call(header, "AddChildToHorizontalBox", copy).ReturnObj());
    }
    // Grid: a Border like the collection's around a UniformGridPanel like it.
    Obj border = w::Spawn("Border", tree), grid = w::Spawn("UniformGridPanel", tree);
    for (const char* p : {"Background", "BrushColor", "Padding", "HorizontalAlignment", "VerticalAlignment"}) CopyProperty(refBorder, border, p);
    for (const char* p : {"SlotPadding", "MinDesiredSlotWidth", "MinDesiredSlotHeight"}) CopyProperty(refGrid, grid, p);
    w::AddChild(border, grid);
    Obj headerSlot = eng::Call(scroll, "AddChild", header).ReturnObj();
    Obj borderSlot = eng::Call(scroll, "AddChild", border).ReturnObj();
    CopySlot(eng::ReadObj(refHeader, "Slot"), headerSlot);
    CopySlot(eng::ReadObj(refBorder, "Slot"), borderSlot);
    // The tiles: the game's own tile buttons, built for each custom cosmetic and bound to the page's handlers like the
    // shown tile, so choosing one goes through the game's own handler.
    Obj buttonClass = eng::FindClass("WBP_CustomizeButton_C");
    int32_t tiles = 0;
    gTiles.clear();
    gTileVisibility = eng::Call(refButton, "GetVisibility").ReturnAs<uint8_t>(w::kSelfHitTestInvisible);
    for (const auto& c : gCustoms) {
        Obj asset = eng::Get(c.asset);
        if (c.kind != static_cast<Kind>(tab) || !asset) continue;
        Obj button = eng::Call(Library("WidgetBlueprintLibrary"), "Create", page, buttonClass, game::PlayerController()).ReturnObj();
        if (!button) continue;
        CopyProperty(refButton, button, "Type");
        eng::Call(button, "BuildCustomizationButton", asset, tiles);
        CopyBindings(refButton, button, "ColorButtonClicked");
        CopyBindings(refButton, button, "CosmeticButtonUnhovered");
        gTiles.push_back(eng::MakeWeak(button));
        Obj slot = eng::Call(grid, "AddChildToUniformGrid", button, tiles / kColumns, tiles % kColumns).ReturnObj();
        CopySlot(eng::ReadObj(refButton, "Slot"), slot);
        ++tiles;
    }
    gHeader = eng::MakeWeak(header);
    gBorder = eng::MakeWeak(border);
    gGrid = eng::MakeWeak(grid);
    hostlog::Info("cosmetics: custom section with " + std::to_string(tiles) + " tile(s) on tab " + std::to_string(tab));
}

// --- which cosmetic the player chose ------------------------------------------------------------------------------
// The page's tile handler (ColorButtonClicked_Handler, a Blueprint function) is wrapped by swapping the native entry
// of that one UFunction. When the player picks a custom cosmetic the game previews it as usual, and the page's pending
// choice (what it saves, and what the profile and multiplayer get) is put back to the game's own, so a custom asset
// never leaves this session; the host then puts the custom one on (below). Picking a game cosmetic takes it off.
using NativeFunction = void (*)(Obj context, uint8_t* frame, void* result);
NativeFunction gHandlerOriginal = nullptr;
Obj gHandlerFunction = nullptr;                     // a UFunction of a Blueprint class loaded for the whole session
int gDataOffset = -1;                               // CosmeticData in its parameters
Obj gKindClass[3] = {};

int KindOfAsset(Obj asset) {
    for (int k = 0; k < 3; ++k)
        if (asset && gKindClass[k] && eng::IsA(asset, gKindClass[k])) return k;
    return -1;
}

// Runs inside the game's own call, outside the host's frame: reads and writes only.
void HookedHandler(Obj context, uint8_t* frame, void* result) {
    Obj data = nullptr, pending = nullptr, node = nullptr, self = nullptr;
    uint8_t* params = nullptr;
    int kind = -1;
    if (frame) {
        std::memcpy(&node, frame + layout::kFFrameFunctionOffset, sizeof node);
        std::memcpy(&self, frame + layout::kFFrameObjectOffset, sizeof self);
        std::memcpy(&params, frame + layout::kFFrameParametersOffset, sizeof params);
    }
    if (node == gHandlerFunction && self == context && params && gDataOffset >= 0) {
        std::memcpy(&data, params + gDataOffset, sizeof data);
        kind = KindOfAsset(data);
        if (kind >= 0) eng::ReadBytes(context, kToSave[kind], &pending, sizeof pending);
    }
    gHandlerOriginal(context, frame, result);
    if (kind < 0) return;
    if (const Custom* c = ByAsset(data)) {
        gEquipped[kind] = c->id;
        eng::WriteBytes(context, kToSave[kind], &pending, sizeof pending);
    } else {
        gEquipped[kind].clear();
    }
}

void WatchChoices() {
    static bool failed = false;
    if (gHandlerOriginal || failed) return;
    Obj page = eng::FindClass("WBP_1_CustomizeColor_C");
    Obj fn = eng::FindFunction(page, "ColorButtonClicked_Handler"), other = eng::FindFunction(page, "TabButtonClicked_Handler");
    if (!fn || !other) return;
    NativeFunction entry = nullptr, otherEntry = nullptr;
    std::memcpy(&entry, fn + layout::kUFunctionNativeFunctionOffset, sizeof entry);
    std::memcpy(&otherEntry, other + layout::kUFunctionNativeFunctionOffset, sizeof otherEntry);
    for (const auto& param : eng::ParamsOf(fn))
        if (param.name == "CosmeticData") gDataOffset = param.offset;
    // Both are Blueprint functions, so both enter the same interpreter entry in the game exe; anything else means the
    // layout is not as measured.
    if (!entry || entry != otherEntry || !eng::InImage(reinterpret_cast<void*>(entry)) || gDataOffset < 0) {
        failed = true;
        hostlog::Warn("cosmetics: the Customize page's handler is not as measured; custom cosmetics cannot be worn");
        return;
    }
    for (int k = 0; k < 3; ++k) gKindClass[k] = eng::FindClass(kAssetClass[k]);
    gHandlerFunction = fn;
    gHandlerOriginal = entry;
    NativeFunction hooked = &HookedHandler;
    std::memcpy(fn + layout::kUFunctionNativeFunctionOffset, &hooked, sizeof hooked);
    hostlog::Info("cosmetics: watching the Customize page's choices");
}

// --- wearing the custom cosmetics ---------------------------------------------------------------------------------
// The player's own balls (the menu ball and the racing ball; other players' balls are another class) wear the custom
// ball and hat over whatever the game gave them, and checkpoints play the custom goal explosion. What was replaced is
// remembered and put back when the custom one is taken off.
struct Replaced {
    eng::Weak component, mesh, material;
    eng::Weak skinActor;                            // a skin drawn by its own actor, hidden while ours is worn
    bool sphereHiddenInGame = false, sphereInvisible = false;  // how the game had hidden the sphere for that actor
    double scale[3] = {1, 1, 1};                    // hats: the slot's own scale
};
std::vector<Replaced> gReplaced;
std::vector<eng::Weak> gBalls, gExplosions;

Obj Sphere() {
    static bool missingLogged = false;
    static eng::Weak ref;
    Obj sphere = eng::Get(ref);
    if (!sphere && !missingLogged) {                // loaded once; a failed load is not retried
        if ((sphere = LoadAsset(kSphereMesh))) {
            KeepAlive(sphere);
            ref = eng::MakeWeak(sphere);
        } else {
            missingLogged = true;
            hostlog::Warn("cosmetics: Unreal's sphere mesh did not load; image balls keep the ball's own mesh");
        }
    }
    return sphere;
}

Replaced* RecordOf(Obj component) {
    for (auto& r : gReplaced)
        if (eng::Get(r.component) == component) return &r;
    return nullptr;
}

void Forget(Replaced* r) { gReplaced.erase(gReplaced.begin() + (r - gReplaced.data())); }

bool IsCustomMaterial(Obj material) {
    for (const auto& c : gCustoms)
        if (material && eng::Get(c.material) == material) return true;
    return false;
}

bool IsCustomMesh(Obj mesh) {
    for (const auto& c : gCustoms)
        if (mesh && eng::Get(c.mesh) == mesh) return true;
    return false;
}

// Some skins (measured: LBall's BP_LBall05) are an actor of their own on the racing ball (CustomSkinChild), with the
// ball's sphere hidden; a custom ball hides that actor and shows the sphere instead.
bool Hidden(Obj actor) {
    bool hidden = false;
    return eng::ReadBool(actor, "bHidden", &hidden) && hidden;
}

void WearBall(Obj ball, Obj sphere, const Custom* custom) {
    Obj skinActor = eng::ReadObj(ball, "CustomSkinChild");
    if (skinActor && !eng::IsLive(skinActor)) skinActor = nullptr;
    Obj material = eng::Call(sphere, "GetMaterial", int32_t{0}).ReturnObj();
    Obj mesh = eng::ReadObj(sphere, "StaticMesh");
    Obj wanted = custom ? eng::Get(custom->material) : nullptr;
    Replaced* r = RecordOf(sphere);
    if (wanted) {
        const bool sphereShown = eng::Call(sphere, "IsVisible").ReturnBool();
        const bool skinShown = skinActor && !Hidden(skinActor);
        if (material == wanted && mesh == Sphere() && sphereShown && !skinShown) return;
        if (!r) {                                    // the game's own look, to put back later
            bool hiddenInGame = false, visible = true;
            eng::ReadBool(sphere, "bHiddenInGame", &hiddenInGame);
            eng::ReadBool(sphere, "bVisible", &visible);
            gReplaced.push_back({eng::MakeWeak(sphere), eng::MakeWeak(mesh == Sphere() ? nullptr : mesh),
                                 eng::MakeWeak(IsCustomMaterial(material) ? nullptr : material), eng::MakeWeak(skinActor),
                                 hiddenInGame, !visible});
            r = &gReplaced.back();
        }
        if (skinShown) {
            eng::Call(skinActor, "SetActorHiddenInGame", uint8_t{1});
            r->skinActor = eng::MakeWeak(skinActor);
        }
        if (!sphereShown) {                          // measured: the racing ball's sphere is hidden in game
            eng::Call(sphere, "SetHiddenInGame", uint8_t{0}, uint8_t{0});
            eng::Call(sphere, "SetVisibility", uint8_t{1}, uint8_t{0});
        }
        if (material != wanted) eng::Call(sphere, "SetMaterial", int32_t{0}, wanted);
        if (Obj plain = Sphere(); plain && mesh != plain) eng::Call(sphere, "SetStaticMesh", plain);
    } else if (r) {
        if (IsCustomMaterial(material))              // otherwise the game has put its own on since
            if (Obj original = eng::Get(r->material)) eng::Call(sphere, "SetMaterial", int32_t{0}, original);
        if (mesh == Sphere())
            if (Obj original = eng::Get(r->mesh)) eng::Call(sphere, "SetStaticMesh", original);
        if (Obj actor = eng::Get(r->skinActor)) {
            eng::Call(actor, "SetActorHiddenInGame", uint8_t{0});
            if (r->sphereHiddenInGame) eng::Call(sphere, "SetHiddenInGame", uint8_t{1}, uint8_t{0});
            if (r->sphereInvisible) eng::Call(sphere, "SetVisibility", uint8_t{0}, uint8_t{0});
        }
        Forget(r);
    }
}

// Models built on a component (the ball's sphere, or its hat slot), one per component.
struct ModelOn {
    eng::Weak component;
    std::string id;
    models::Built built;
};
std::vector<ModelOn> gModels;

void RemoveModel(Obj component) {
    for (size_t i = 0; i < gModels.size(); ++i)
        if (eng::Get(gModels[i].component) == component) {
            models::Destroy(gModels[i].built);
            gModels.erase(gModels.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

void WearModel(Obj component, const Custom* c, Obj ball) {
    ModelOn* on = nullptr;
    for (auto& m : gModels)
        if (eng::Get(m.component) == component) on = &m;
    if (on && (on->id != c->id || !on->built.Alive())) {
        RemoveModel(component);
        on = nullptr;
    }
    if (!on) {
        gModels.push_back({eng::MakeWeak(component), c->id, models::Build(c->model, component)});
        on = &gModels.back();
        if (on->built.actors.empty()) return;
        hostlog::Info("cosmetics: built " + c->id + " on " + eng::PathOf(component));
    }
    models::Animate(c->model, on->built, ball, game::Seconds());
}

void WearHat(Obj ball, Obj slot, const Custom* hat) {
    Obj mesh = eng::ReadObj(slot, "StaticMesh");
    Replaced* r = RecordOf(slot);
    struct Vector {
        double v[3];
    };
    if (hat) {
        Obj wanted = eng::Get(hat->mesh);           // none for a hat that is a model only
        Vector scale{{1, 1, 1}};
        eng::ReadBytes(slot, "RelativeScale3D", scale.v, sizeof scale.v);
        if (!r) {
            gReplaced.push_back({eng::MakeWeak(slot), eng::MakeWeak(IsCustomMesh(mesh) ? nullptr : mesh), {}, {}, false, false,
                                 {scale.v[0], scale.v[1], scale.v[2]}});
        }
        if (mesh != wanted) eng::Call(slot, "SetStaticMesh", wanted);
        if (scale.v[0] != hat->scale) eng::Call(slot, "SetRelativeScale3D", Vector{{hat->scale, hat->scale, hat->scale}});
        if (hat->hasModel) WearModel(slot, hat, ball);
        else RemoveModel(slot);
    } else if (r) {
        if (!mesh || IsCustomMesh(mesh)) {             // otherwise the game has put its own on since
            eng::Call(slot, "SetStaticMesh", eng::Get(r->mesh));
            eng::Call(slot, "SetRelativeScale3D", Vector{{r->scale[0], r->scale[1], r->scale[2]}});
        }
        Forget(r);
        RemoveModel(slot);
    }
}

// The Customize page previews a goal explosion with the menu's checkpoint prop (SMA_MainMenuCP): it keeps the game's
// explosions (GoalExploData) and one prepared component for each (CachedGoalExplos), in the same order (measured), and
// plays the one that matches the choice. Custom ones are added to both, so the game's own preview plays them too.
void AddPreviews(Obj prop) {
    ArrayHeader data{nullptr, 0, 0}, cached{nullptr, 0, 0};
    if (!eng::ReadBytes(prop, "GoalExploData", &data, sizeof data) || !eng::ReadBytes(prop, "CachedGoalExplos", &cached, sizeof cached) ||
        data.num != cached.num || data.num <= 0)
        return;
    auto* assets = static_cast<Obj*>(data.data);
    auto* components = static_cast<Obj*>(cached.data);
    std::vector<Obj> missing;
    for (const auto& c : gCustoms)
        if (Obj asset = eng::Get(c.asset); c.kind == Kind::Bfx && asset &&
                                           std::find(assets, assets + data.num, asset) == assets + data.num)
            missing.push_back(asset);
    Obj reference = components[0];
    if (missing.empty() || !eng::IsLive(reference)) return;
    struct V {
        double x, y, z;
    } location{}, rotation{}, scale{1, 1, 1};
    eng::ReadBytes(reference, "RelativeLocation", &location, sizeof location);
    eng::ReadBytes(reference, "RelativeRotation", &rotation, sizeof rotation);
    struct Transform {
        uint8_t bytes[96];
    } transform{};
    const Params made = eng::Call(Library("KismetMathLibrary"), "MakeTransform", location, rotation, scale);
    if (const uint8_t* r = made.Return()) std::memcpy(transform.bytes, r, sizeof transform.bytes);
    const int total = data.num + static_cast<int>(missing.size());
    ArrayHeader newData = EngineArray(total), newCached = EngineArray(total);
    if (!newData.data || !newCached.data) return;
    std::memcpy(newData.data, assets, static_cast<size_t>(data.num) * sizeof(Obj));
    std::memcpy(newCached.data, components, static_cast<size_t>(cached.num) * sizeof(Obj));
    int n = data.num;
    for (Obj asset : missing) {
        Params add(eng::FunctionOn(prop, "AddComponentByClass"));
        add.Set("Class", eng::ClassOf(reference));
        add.Set("bManualAttachment", uint8_t{0});
        add.Set("RelativeTransform", transform);
        add.Set("bDeferredFinish", uint8_t{0});
        eng::Invoke(prop, add);
        Obj component = add.ReturnObj();
        if (!component) break;
        eng::Call(component, "InitParticle", asset);
        eng::Call(component, "Deactivate");
        static_cast<Obj*>(newData.data)[n] = asset;
        static_cast<Obj*>(newCached.data)[n] = component;
        ++n;
    }
    newData.num = newCached.num = n;
    eng::WriteBytes(prop, "GoalExploData", &newData, sizeof newData);
    eng::WriteBytes(prop, "CachedGoalExplos", &newCached, sizeof newCached);
    hostlog::Info("cosmetics: " + std::to_string(n - data.num) + " custom goal explosion(s) added to the Customize preview");
}

void FindTargets() {
    gBalls.clear();
    gExplosions.clear();
    Obj menuBall = eng::FindClass("BP_MenuBall_C"), rollingBall = eng::FindClass("BP_RollingBall_C");
    Obj explosion = eng::FindClass("NPSC_GoalExplo_C"), checkpoint = eng::FindClass("BP_Checkpoint_C");
    Obj menuProp = eng::FindClass("SMA_MainMenuCP_C");
    std::vector<Obj> props;
    eng::ForEachObject([&](Obj o) {
        Obj cls = eng::ClassOf(o);
        if ((cls == menuBall || cls == rollingBall) && cls && !eng::IsDefaultObject(o)) gBalls.push_back(eng::MakeWeak(o));
        if (cls == explosion && cls && checkpoint && eng::IsA(eng::OuterOf(o), checkpoint)) gExplosions.push_back(eng::MakeWeak(o));
        if (cls == menuProp && cls && !eng::IsDefaultObject(o)) props.push_back(o);
        return true;
    });
    for (Obj prop : props) AddPreviews(prop);          // after the walk: it creates components
}

void Wear() {
    const Custom* ball = Worn(Kind::Ball);
    const Custom* hat = Worn(Kind::Hat);
    const Custom* bfx = Worn(Kind::Bfx);
    for (const auto& weak : gBalls) {
        Obj actor = eng::Get(weak);
        if (!actor) continue;
        if (Obj sphere = eng::ReadObj(actor, "Sphere")) {
            WearBall(actor, sphere, ball);
            if (ball && ball->hasModel) WearModel(sphere, ball, actor);
            else RemoveModel(sphere);
        }
        if (Obj slot = eng::ReadObj(actor, "AccessorySlot")) WearHat(actor, slot, hat);
    }
    for (size_t i = gModels.size(); i-- > 0;)          // balls gone with their map
        if (!eng::Get(gModels[i].component)) {
            models::Destroy(gModels[i].built);
            gModels.erase(gModels.begin() + static_cast<std::ptrdiff_t>(i));
        }
    // Checkpoints set up their explosion from the player's choice; the custom one is set up over it. Its effect and
    // scale tell the two apart (InitParticle sets the component's Asset and scale from the data asset: measured).
    if (!bfx) return;
    Obj asset = eng::Get(bfx->asset);
    Obj effect = eng::ReadObj(asset, "NiagaraSystem");
    for (const auto& weak : gExplosions) {
        Obj explosion = eng::Get(weak);
        double scale[3] = {};
        if (explosion && eng::ReadBytes(explosion, "RelativeScale3D", scale, sizeof scale) &&
            (scale[0] != bfx->scale || eng::ReadObj(explosion, "Asset") != effect))
            eng::Call(explosion, "InitParticle", asset);
    }
}

// The page's highlight: the worn custom tile, and none of the game's while a custom one is worn.
void Highlight(Obj page, int tab) {
    const std::string& worn = gEquipped[tab];
    for (const auto& weak : gTiles)
        if (Obj tile = eng::Get(weak)) {
            const Custom* c = ByAsset(eng::ReadObj(tile, "CosmeticData"));
            bool active = false;
            eng::ReadBool(tile, "Active", &active);
            const bool wanted = c && c->id == worn;
            if (active != wanted) eng::Call(tile, "SetIsActive", static_cast<uint8_t>(wanted));
        }
    if (worn.empty()) return;
    ForEachGameTile(page, [&](Obj tile) {
        bool active = false;
        if (eng::ReadBool(tile, "Active", &active) && active) eng::Call(tile, "SetIsActive", uint8_t{0});
    });
}

}  // namespace

// Referenced from the game instance's ReferencedObjects (a UPROPERTY array the collector follows; the game instance
// lives for the whole session). The root-set flag alone is not enough on this build: measured, an object with it set
// was still collected by the next garbage collection.
void KeepAlive(Obj o) {
    Obj instance = GameInstance();
    ArrayHeader array{nullptr, 0, 0};
    if (!o || !instance || !eng::ReadBytes(instance, "ReferencedObjects", &array, sizeof array)) return;
    for (int32_t i = 0; i < array.num; ++i)
        if (static_cast<Obj*>(array.data)[i] == o) return;
    if (array.num == array.max) {                   // a bigger engine-allocated buffer; the old one is left as it is
        ArrayHeader bigger = EngineArray(array.max < 16 ? 32 : array.max * 2);
        if (!bigger.data) return;
        if (array.num) std::memcpy(bigger.data, array.data, static_cast<size_t>(array.num) * sizeof(Obj));
        bigger.num = array.num;
        array = bigger;
    }
    static_cast<Obj*>(array.data)[array.num++] = o;
    eng::WriteBytes(instance, "ReferencedObjects", &array, sizeof array);
}

Obj LoadAsset(const std::wstring& path) {
    Obj library = Library("KismetSystemLibrary");
    const Params made = eng::Call(library, "MakeSoftObjectPath", Str(path));
    size_t size = 0;
    const uint8_t* softPath = made.Return(&size);
    if (!softPath || size != 32) return nullptr;
    struct {
        uint8_t bytes[40];
    } reference{};
    std::memcpy(reference.bytes + 8, softPath, 32);
    return eng::Call(library, "LoadAsset_Blocking", reference).ReturnObj();
}

bool AddBall(const std::string& id, const std::string& name, const std::wstring& image, const std::wstring& preview,
             const std::string& model) {
    if (!id.empty() && Exists(Kind::Ball, id)) return true;    // already added (its plugin was reloaded)
    if (id.empty()) return false;
    if (Wait(Kind::Ball, id, name, eng::Narrow(image.c_str(), static_cast<int>(image.size())), preview, 1, model)) return true;
    Custom c;
    c.kind = Kind::Ball;
    c.id = id;
    c.scale = 1;
    if (!ParseModel(&c, model)) return false;
    Obj texture = Texture(image);
    Obj parent = LoadAsset(kImageBallMaterial);
    if (!texture || !parent) {
        hostlog::Warn("cosmetics: ball " + id + ": " + (texture ? "the ball material did not load" : "the image did not load"));
        return false;
    }
    Obj material = eng::Call(Library("KismetMaterialLibrary"), "CreateDynamicMaterialInstance", game::PlayerController(), parent,
                             MakeName("CustomBall_" + id), uint8_t{0})
                       .ReturnObj();
    if (!material) return false;
    KeepAlive(material);
    SetTexture(material, "Base Color", texture);
    if (Obj flat = LoadAsset(kFlatNormal)) SetTexture(material, "Normal Map", flat);
    // Plastic rather than LBall's mirror finish: MI_LBall05 reads roughness, metallic and occlusion from channels of
    // its "Pack Map" (measured: roughness G, metallic B, occlusion R, picked by these vector parameters). With a
    // white pack map, the channel vectors set the values directly.
    if (Obj white = LoadAsset(kWhite)) {
        SetTexture(material, "Pack Map", white);
        SetVector(material, "[Roughness] Roughness Channel", 0, 0.55f, 0, 0);
        SetVector(material, "[Metallic] Metallic Channel", 0, 0, 0, 0);
        SetVector(material, "[AO] Ambient Occlusion Channel", 1, 0, 0, 0);
    }
    Obj asset = Create(Kind::Ball, id, name, preview.empty() ? texture : Texture(preview));
    if (!asset) return false;
    SetObject(asset, "SkinMaterial", material);
    SetObject(asset, "SkinGhostVariant", material);
    if (Obj template_ = SkinUsing(parent)) CopyProperty(template_, asset, "BallType");
    c.asset = eng::MakeWeak(asset);
    c.material = eng::MakeWeak(material);
    Added(std::move(c));
    return true;
}

bool AddHat(const std::string& id, const std::string& name, const std::string& meshPath, double scale,
            const std::wstring& preview, const std::string& model) {
    if (!id.empty() && Exists(Kind::Hat, id)) return true;    // already added (its plugin was reloaded)
    if (id.empty() || (meshPath.empty() && model.empty())) return false;
    if (Wait(Kind::Hat, id, name, meshPath, preview, scale, model)) return true;
    Custom c;
    c.kind = Kind::Hat;
    c.id = id;
    c.scale = scale;
    if (!ParseModel(&c, model)) return false;
    Obj mesh = meshPath.empty() ? nullptr : LoadAsset(eng::Widen(meshPath));
    if (!meshPath.empty() && !mesh) {
        hostlog::Warn("cosmetics: hat " + id + ": mesh " + meshPath + " did not load");
        return false;
    }
    Obj asset = Create(Kind::Hat, id, name, Texture(preview));
    if (!asset) return false;
    if (mesh) SetObject(asset, "AccessoryMesh", mesh);
    // Ghosts (other players' and replays' balls) draw hats with one see-through material; a game hat's is reused.
    if (Obj template_ = LoadAsset(kGhostHatSource)) CopyProperty(template_, asset, "AccessoryGhostMaterial");
    c.asset = eng::MakeWeak(asset);
    c.mesh = eng::MakeWeak(mesh);
    if (mesh) KeepAlive(mesh);
    Added(std::move(c));
    return true;
}

bool AddBfx(const std::string& id, const std::string& name, const std::string& baseExplosion, double scale,
            const std::wstring& preview, const std::string& system, const std::string& sound) {
    if (!id.empty() && Exists(Kind::Bfx, id)) return true;    // already added (its plugin was reloaded)
    if (id.empty()) return false;
    if (Wait(Kind::Bfx, id, name, baseExplosion, preview, scale, "", system, sound)) return true;
    Obj base = LoadAsset(eng::Widen(baseExplosion));
    if (!base) {
        hostlog::Warn("cosmetics: bfx " + id + ": " + baseExplosion + " did not load");
        return false;
    }
    Obj asset = Create(Kind::Bfx, id, name, Texture(preview));
    if (!asset) return false;
    for (const char* p : {"NiagaraSystem", "SFX"}) CopyProperty(base, asset, p);
    // Another effect and sound than the base's, from the game's assets.
    for (const auto& [path, property] : {std::pair{system, "NiagaraSystem"}, std::pair{sound, "SFX"}}) {
        if (path.empty()) continue;
        Obj other = LoadAsset(eng::Widen(path));
        if (!other) {
            hostlog::Warn("cosmetics: bfx " + id + ": " + path + " did not load");
            return false;
        }
        KeepAlive(other);
        SetObject(asset, property, other);
    }
    const double s[3] = {scale, scale, scale};
    eng::WriteBytes(asset, "RelativeScale", s, sizeof s);
    Custom c;
    c.kind = Kind::Bfx;
    c.id = id;
    c.asset = eng::MakeWeak(asset);
    c.scale = scale;
    Added(std::move(c));
    return true;
}

int Count(Kind kind) {
    int n = 0;
    for (const auto& c : gCustoms) n += c.kind == kind && eng::Get(c.asset) ? 1 : 0;
    return n;
}

void Frame() {
    if (!gWaiting.empty() && game::PlayerController()) {
        const std::vector<Request> waiting = std::move(gWaiting);
        gWaiting.clear();
        for (const auto& r : waiting) {
            if (r.kind == Kind::Ball) AddBall(r.id, r.name, eng::Widen(r.what), r.preview, r.model);
            if (r.kind == Kind::Hat) AddHat(r.id, r.name, r.what, r.scale, r.preview, r.model);
            if (r.kind == Kind::Bfx) AddBfx(r.id, r.name, r.what, r.scale, r.preview, r.system, r.sound);
        }
    }
    if (gCustoms.empty()) return;
    WatchChoices();
    static double lastSearch = -100;
    if (game::Seconds() - lastSearch > 1) {         // the balls and checkpoints come and go with maps
        lastSearch = game::Seconds();
        FindTargets();
    }
    Wear();
    static double lastLook = -100;
    if (game::Seconds() - lastLook < 0.2) return;
    lastLook = game::Seconds();
    Obj page = eng::Get(gPage);
    if (!page) page = LivePage();
    if (!page) return;
    int32_t tab = 0;
    eng::ReadBytes(page, "ActiveTabIndex", &tab, sizeof tab);
    // Rebuilt when the page is new, the tab changed, or the game rebuilt its lists (our grid then lost its parent).
    Obj grid = eng::Get(gGrid);
    const char* why = page != eng::Get(gPage) ? "new page"
                      : tab != gSectionTab     ? "tab"
                      : !grid && Count(static_cast<Kind>(tab)) > 0 ? "no grid"
                      : grid && !eng::Call(eng::Get(gBorder), "GetParent").ReturnObj() ? "section removed"
                                                                                       : nullptr;
    if (why) BuildSection(page, tab);
    // A tile collapses itself when it is constructed; the page shows only the tiles it placed, so ours are shown here.
    for (const auto& weak : gTiles)
        if (Obj tile = eng::Get(weak))
            if (eng::Call(tile, "GetVisibility").ReturnAs<uint8_t>(gTileVisibility) != gTileVisibility)
                eng::Call(tile, "SetVisibility", gTileVisibility);
    if (tab >= 0 && tab < 3) Highlight(page, tab);
}

bool Equip(Kind kind, const std::string& id) {
    if (!id.empty() && !Exists(kind, id)) return false;     // a waiting one is worn once it is made
    gEquipped[static_cast<int>(kind)] = id;
    return true;
}

std::string Equipped(Kind kind) { return gEquipped[static_cast<int>(kind)]; }

bool ClickTile(int index) {
    Obj tile = index >= 0 && index < static_cast<int>(gTiles.size()) ? eng::Get(gTiles[static_cast<size_t>(index)]) : nullptr;
    return tile && eng::Call(tile, "BndEvt__WBP_BasicBallSelect_Hitbox_K2Node_ComponentBoundEvent_5_OnButtonClickedEvent__DelegateSignature").Invoked();
}

std::string Status() {
    return "cosmetics: " + std::to_string(Count(Kind::Ball)) + " ball(s), " + std::to_string(Count(Kind::Hat)) + " hat(s), " +
           std::to_string(Count(Kind::Bfx)) + " bfx; worn: ball '" + gEquipped[0] + "', hat '" + gEquipped[1] + "', bfx '" +
           gEquipped[2] + "'; section on tab " + std::to_string(gSectionTab) + "; " + std::to_string(gReplaced.size()) +
           " part(s) replaced";
}

}  // namespace cosmetics
