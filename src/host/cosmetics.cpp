#include "cosmetics.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>

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
// The game's glass, V2 (translucent; ColorGlass and Opacity, read from the package), for clear balls. Drawn in the same
// translucency pass as the stadium water, so the two sort by distance (M_Glass's earlier pass put water over it).
const wchar_t* kGlassMaterial = L"/Game/Art/Materials/Masters/M_GlassV2.M_GlassV2";
constexpr float kClearBallOpacity = 0.07f;    // V2 darkens more than M_Glass: 0.12 looked smoky (measured)

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
    // Extras: another kind of cosmetic in a slot of its own ("arms"), worn on the ball as well as its ball and hat. Its
    // asset is a PDA_Accessory_C, so its tile is the game's own and the page's handler takes it (as a hat, put back).
    std::string extra;
    bool none = false;                              // an extra slot's "none" tile
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
struct ExtraRequest {                               // an extra made before the game was ready
    std::string slot, id, name;
    std::wstring preview;
    std::string model;
};
std::vector<ExtraRequest> gWaitingExtras;
std::map<std::string, std::string> gEquippedExtra;  // per extra slot, the extra worn
bool gFixMenuHat = false;                           // an extra was picked: the page previewed it as a hat (none)

// Public and local choices: the Customize page's two modes. Public is the game's own choice (the profile, which is
// what hiscores, replays and other players get); local is worn on the player's own balls only, and is a custom
// cosmetic (gEquipped) or another of the game's (gLocalGame), per kind, or nothing (the public one). The page opens
// in local mode.
enum Mode { kLocal = 0, kPublic = 1 };
int gMode = kLocal;
eng::Weak gLocalGame[3];
std::string gLocalPath[3];                          // its object path, as saved
bool gLocalLoaded = false, gLocalChanged = false;
const char* const kKindName[3] = {"ball", "hat", "bfx"};

// The section on the page, rebuilt when the page or its tab changes.
eng::Weak gPage, gSectionPage;
struct Section {                                    // a header and a grid of tiles in the page's scroll box
    eng::Weak header, border, grid;
};
std::vector<Section> gSections;
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

// Value properties copied byte for byte: only for plain data (numbers, colours, margins, object pointers). Never for a
// brush: an FSlateBrush holds a shared, reference-counted resource handle, so a byte copy shares it without counting it
// and it is freed twice when both widgets go (measured: the game crashed after the menu was left and loaded again).
void CopyProperty(Obj from, Obj to, const char* property) {
    const eng::Prop p = eng::FindProp(eng::ClassOf(from), property);
    if (!p || !from || !to) return;
    std::vector<uint8_t> bytes(static_cast<size_t>(p.size));
    if (eng::ReadBytes(from, property, bytes.data(), bytes.size())) eng::WriteBytes(to, property, bytes.data(), bytes.size());
}

// A slot's layout copied from another slot of the same kind through its setters: a slot added to a panel that is
// already on screen has its Slate slot built at once, so written properties alone would not show.
// A brush (Image.Brush, Border.Background) copied through the widget's own SetBrush, which copies it properly.
void CopyBrush(Obj from, Obj to, const char* property) {
    const eng::Prop p = eng::FindProp(eng::ClassOf(from), property);
    if (!p || !to) return;
    std::vector<uint8_t> brush(static_cast<size_t>(p.size));
    if (!eng::ReadBytes(from, property, brush.data(), brush.size())) return;
    Params set(eng::FunctionOn(to, "SetBrush"));
    if (set.SetArg(0, brush.data(), brush.size())) eng::Invoke(to, set);
}

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

void SetScalar(Obj material, const std::string& parameter, float value) {
    Params p(eng::FunctionOn(material, "SetScalarParameterValue"));
    const Name name = MakeName(parameter);
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
        if (c.kind == kind && c.extra.empty() && c.id == id && eng::Get(c.asset)) return &c;
    return nullptr;
}

const Custom* FindExtra(const std::string& slot, const std::string& id) {
    for (const auto& c : gCustoms)
        if (c.extra == slot && !c.none && c.id == id && eng::Get(c.asset)) return &c;
    return nullptr;
}

// The extra slots, in the order their first extra was added.
std::vector<std::string> ExtraSlots() {
    std::vector<std::string> out;
    for (const auto& c : gCustoms)
        if (!c.extra.empty() && std::find(out.begin(), out.end(), c.extra) == out.end()) out.push_back(c.extra);
    return out;
}

const Custom* WornExtra(const std::string& slot) {
    const auto it = gEquippedExtra.find(slot);
    return it == gEquippedExtra.end() || it->second.empty() ? nullptr : FindExtra(slot, it->second);
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

// The local choice's asset for a kind: a custom cosmetic's, a game one, or null (the public one).
Obj LocalAsset(int kind) {
    if (const Custom* c = Worn(static_cast<Kind>(kind))) return eng::Get(c->asset);
    return eng::Get(gLocalGame[kind]);
}

// Only writes the choice down: the handler that calls it runs inside the game's own call. Frame saves it.
void SetLocalGame(int kind, Obj asset) {
    gLocalGame[kind] = eng::MakeWeak(asset);
    gLocalPath[kind] = asset ? eng::PathOf(asset) : "";
    gLocalChanged = true;
}

std::wstring LocalFile() { return hostlog::DataDir() + L"\\cosmetics_local.txt"; }

void SaveLocal() {
    FILE* f = _wfopen(LocalFile().c_str(), L"wb");
    if (!f) {
        hostlog::Warn("cosmetics: the local choices could not be saved");
        return;
    }
    for (int k = 0; k < 3; ++k) std::fprintf(f, "%s=%s\n", kKindName[k], gLocalPath[k].c_str());
    std::fclose(f);
}

// Last session's local game cosmetics ("ball=/Game/...", one line per kind), loaded once the game is ready.
void LoadLocal() {
    gLocalLoaded = true;
    FILE* f = _wfopen(LocalFile().c_str(), L"rb");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos || eq + 1 >= s.size()) continue;
        for (int k = 0; k < 3; ++k) {
            if (s.compare(0, eq, kKindName[k]) != 0 || eq != std::strlen(kKindName[k])) continue;
            Obj asset = LoadAsset(eng::Widen(s.substr(eq + 1)));
            if (!asset || !eng::IsA(asset, eng::FindClass(kAssetClass[k]))) {
                hostlog::Warn("cosmetics: the local " + std::string(kKindName[k]) + " " + s.substr(eq + 1) + " did not load");
                continue;
            }
            KeepAlive(asset);
            gLocalGame[k] = eng::MakeWeak(asset);
            gLocalPath[k] = s.substr(eq + 1);
            hostlog::Info("cosmetics: local " + std::string(kKindName[k]) + " " + gLocalPath[k]);
        }
    }
    std::fclose(f);
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
    for (auto& section : gSections)
        for (eng::Weak* weak : {&section.header, &section.border})
            if (Obj widget = eng::Get(*weak)) eng::Call(widget, "RemoveFromParent");
    gSections.clear();
}

// The sections a tab shows: its custom cosmetics ("" in the list). Extras have tabs of their own (below).
std::vector<std::string> SectionsOf(int tab) {
    std::vector<std::string> out;
    if (tab >= 0 && tab < 3 && Count(static_cast<Kind>(tab)) > 0) out.push_back("");
    return out;
}

// A tile of the page's own for a kind of cosmetic, shown or not (an extra's tab is built while another tab shows).
Obj AnyTile(Obj page, Kind kind) {
    Obj found = nullptr, cls = eng::FindClass(kAssetClass[static_cast<int>(kind)]);
    ForEachGameTile(page, [&](Obj tile) {
        if (!found && eng::IsA(eng::ReadObj(tile, "CosmeticData"), cls)) found = tile;
    });
    return found;
}

// One section in a scroll box: a header like the collection's (its accent image and its text, with `label` as the
// text) and a Border like the collection's around a UniformGridPanel of tiles, one for each custom cosmetic `wanted`
// takes. The tiles are the game's own tile buttons, built like `refButton` and bound to the page's handlers as it is,
// so choosing one goes through the game's own handler.
template <class Wanted>
Section AddSection(Obj page, Obj scroll, const std::string& label, Obj refButton, Wanted wanted, std::vector<eng::Weak>* tiles,
                   int* made) {
    Obj tree = eng::ReadObj(page, "WidgetTree");
    Obj refHeader = eng::ReadObj(page, "HB_CollectionSubheader"), refText = eng::ReadObj(page, "CollectionCategory_Text");
    Obj refGrid = eng::ReadObj(page, "CollectionCosmetics_Grid");
    Obj refBorder = eng::Call(refGrid, "GetParent").ReturnObj();
    Obj header = w::Spawn("HorizontalBox", tree);
    for (Obj slot : eng::ReadObjArray(refHeader, "Slots")) {
        Obj content = eng::ReadObj(slot, "Content");
        Obj copy = nullptr;
        if (content == refText) {
            copy = w::Spawn("TextBlock", tree);
            w::CopyFont(refText, copy);
            w::SetText(copy, label);
        } else if (eng::IsA(content, eng::FindClass("Image"))) {
            copy = w::Spawn("Image", tree);
            CopyBrush(content, copy, "Brush");
            CopyProperty(content, copy, "ColorAndOpacity");
        }
        if (!copy) continue;
        CopySlot(slot, eng::Call(header, "AddChildToHorizontalBox", copy).ReturnObj());
    }
    Obj border = w::Spawn("Border", tree), grid = w::Spawn("UniformGridPanel", tree);
    CopyBrush(refBorder, border, "Background");
    for (const char* prop : {"BrushColor", "Padding", "HorizontalAlignment", "VerticalAlignment"}) CopyProperty(refBorder, border, prop);
    for (const char* prop : {"SlotPadding", "MinDesiredSlotWidth", "MinDesiredSlotHeight"}) CopyProperty(refGrid, grid, prop);
    w::AddChild(border, grid);
    CopySlot(eng::ReadObj(refHeader, "Slot"), eng::Call(scroll, "AddChild", header).ReturnObj());
    CopySlot(eng::ReadObj(refBorder, "Slot"), eng::Call(scroll, "AddChild", border).ReturnObj());
    Obj buttonClass = eng::FindClass("WBP_CustomizeButton_C");
    int32_t n = 0;
    for (const auto& c : gCustoms) {
        Obj asset = eng::Get(c.asset);
        if (!asset || !wanted(c)) continue;
        Obj button = eng::Call(Library("WidgetBlueprintLibrary"), "Create", page, buttonClass, game::PlayerController()).ReturnObj();
        if (!button) continue;
        CopyProperty(refButton, button, "Type");
        eng::Call(button, "BuildCustomizationButton", asset, n);
        CopyBindings(refButton, button, "ColorButtonClicked");
        CopyBindings(refButton, button, "CosmeticButtonUnhovered");
        tiles->push_back(eng::MakeWeak(button));
        CopySlot(eng::ReadObj(refButton, "Slot"), eng::Call(grid, "AddChildToUniformGrid", button, n / kColumns, n % kColumns).ReturnObj());
        ++n;
    }
    *made += n;
    return {eng::MakeWeak(header), eng::MakeWeak(border), eng::MakeWeak(grid)};
}

void BuildSection(Obj page, int tab) {
    RemoveSection();
    gPage = gSectionPage = eng::MakeWeak(page);
    gSectionTab = tab;
    gTiles.clear();
    const std::vector<std::string> wanted = SectionsOf(tab);
    if (wanted.empty()) return;
    Obj refHeader = eng::ReadObj(page, "HB_CollectionSubheader"), refText = eng::ReadObj(page, "CollectionCategory_Text");
    Obj refGrid = eng::ReadObj(page, "CollectionCosmetics_Grid");
    Obj scroll = eng::Call(refHeader, "GetParent").ReturnObj();
    if (!eng::ReadObj(page, "WidgetTree") || !refHeader || !refText || !refGrid || !eng::Call(refGrid, "GetParent").ReturnObj() || !scroll) {
        hostlog::Warn("cosmetics: the Customize page is not laid out as measured; no custom section");
        return;
    }
    // The tiles copy one the page shows for the same kind of cosmetic (tiles differ per tab: measured Type 1 for
    // collection balls, 3 for goal explosions). Until the page has shown its tiles there is none: tried again later.
    Obj refButton = ShownTile(page, static_cast<Kind>(tab));
    if (!refButton) return;
    gTileVisibility = eng::Call(refButton, "GetVisibility").ReturnAs<uint8_t>(w::kSelfHitTestInvisible);
    int total = 0;
    const Kind kind = static_cast<Kind>(tab);
    Section section = AddSection(page, scroll, "custom", refButton,
                                 [&](const Custom& c) { return c.kind == kind && c.extra.empty(); }, &gTiles, &total);
    gSections.push_back(section);
    if (gMode == kPublic)                           // custom cosmetics are only local choices
        for (eng::Weak* weak : {&section.header, &section.border})
            if (Obj widget = eng::Get(*weak)) w::SetVisibility(widget, w::kCollapsed);
    hostlog::Info("cosmetics: custom section with " + std::to_string(total) + " tile(s) on tab " + std::to_string(tab));
}

// --- which cosmetic the player chose ------------------------------------------------------------------------------
// The page's tile handler (ColorButtonClicked_Handler, a Blueprint function) is wrapped by swapping the native entry
// of that one UFunction. In public mode a game cosmetic is the game's own choice, as without the host. In local mode,
// and for every custom cosmetic, the game previews the pick as usual and the page's pending choice (what it saves, and
// what the profile and multiplayer get) is put back as it was, so the public choice stays and a custom asset never
// leaves this session; the host then wears the local one (below). Picking the public one again clears the local one.
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
    const Custom* c = ByAsset(data);
    if (!c && gMode == kPublic) return;             // the game's own choice, saved as usual
    // A local choice (or a custom one, which is only ever local): the public choice stays as it was.
    Obj taken = nullptr;
    eng::ReadBytes(context, kToSave[kind], &taken, sizeof taken);
    eng::WriteBytes(context, kToSave[kind], &pending, sizeof pending);
    if (c && !c->extra.empty()) {                   // an extra (or its slot's "none"): the hat stays as it is
        gEquippedExtra[c->extra] = c->none ? "" : c->id;
        gFixMenuHat = true;
    } else if (c) {
        gEquipped[kind] = c->id;
        SetLocalGame(kind, nullptr);
    } else if (taken != data) {
        return;                                     // the page takes only unlocked cosmetics (read in its handler)
    } else {
        gEquipped[kind].clear();
        SetLocalGame(kind, data == pending ? nullptr : data);   // the public one again: local matches public
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
    eng::Weak worn{};                               // hats: the mesh the host put on the slot (none for a model-only hat)
    eng::Weak dragged{};                            // the menu ball: what its drag turned before (see DragTurns)
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

// Some skins (measured: LBall's BP_LBall05) are an actor of their own on the racing ball (CustomSkinChild), with the
// ball's sphere hidden; a custom ball hides that actor and shows the sphere instead.
bool Hidden(Obj actor) {
    bool hidden = false;
    return eng::ReadBool(actor, "bHidden", &hidden) && hidden;
}

// --- the game's camera fade ------------------------------------------------------------------------------------------
// Since the game's 2026-10-06 update the racing ball fades out as the camera comes close (its CameraProximityFade, a
// BallestCameraFadeComponent). Measured: it keeps a snapshot of every mesh on the ball (and of actors given to
// RegisterCameraFadeCosmeticActor) and puts a fade copy of each material on in its place: a cooked variant under
// /Game/Art/Materials/CameraFade, or for a dynamic material a new dynamic one of its parent's variant (a custom image
// ball's, for one). BeginCameraFadeAppearanceChange puts the originals back and RefreshCameraFadeAppearance takes new
// snapshots. So the host tells it when the custom look changes, and takes the fade copy of its own material as worn.
Obj FadeOf(Obj ball) {
    return ball && eng::FindProp(eng::ClassOf(ball), "CameraProximityFade") ? eng::ReadObj(ball, "CameraProximityFade") : nullptr;
}

// BallestCameraFadeMaterialSnapshot (from the game's types): Mesh at 0, OriginalMaterials at 0x18, AppliedMaterials at
// 0x28, 0x48 bytes in all.
struct ObjArray {
    Obj* data;
    int32_t num, max;
};
constexpr size_t kSnapshotSize = 0x48, kSnapshotOriginals = 0x18, kSnapshotApplied = 0x28;

// The original material the fade replaced on a component with `copy`, or null if `copy` is not one of its copies.
Obj FadeOriginal(Obj ball, Obj component, Obj copy) {
    Obj fade = FadeOf(ball);
    struct {
        uint8_t* data;
        int32_t num, max;
    } snaps{};
    if (!fade || !copy || !eng::ReadBytes(fade, "MaterialSnapshots", &snaps, sizeof snaps) || !snaps.data || snaps.num > 1024)
        return nullptr;
    for (int i = 0; i < snaps.num; ++i) {
        const uint8_t* e = snaps.data + i * kSnapshotSize;
        Obj mesh;
        std::memcpy(&mesh, e, sizeof mesh);
        if (mesh != component) continue;
        ObjArray originals, applied;
        std::memcpy(&originals, e + kSnapshotOriginals, sizeof originals);
        std::memcpy(&applied, e + kSnapshotApplied, sizeof applied);
        for (int j = 0; j < originals.num && j < applied.num; ++j)
            if (applied.data[j] == copy && originals.data[j] != copy) return originals.data[j];
    }
    return nullptr;
}

// --- a game cosmetic worn locally ----------------------------------------------------------------------------------
// Put on as the game itself puts a skin on (read from the blueprints): the menu ball as the Customize page previews one
// (SetSpecialBall for a skin with an actor of its own; ReassignDMI + DetermineMIParams + AssignBasicBallParams for a
// basic ball, type 0; the skin's material and SetSpecialBall(false) otherwise), the racing ball as its
// LoadBallCustomization does, but without its SkinAsset, which is what its runs record.
struct SkinLook {
    Obj material = nullptr, special = nullptr;
    uint8_t type = 1;
};
struct CachedLook {
    eng::Weak asset;
    SkinLook look;
};
std::vector<CachedLook> gLooks;                     // game data assets do not change

SkinLook LookOf(Obj skin) {
    for (const auto& c : gLooks)
        if (skin && eng::Get(c.asset) == skin) return c.look;
    SkinLook l;
    if (!skin) return l;
    const Params m = eng::Call(skin, "GetSkinMaterials");
    l.material = m.GetObj("SkinMaterial");
    l.special = m.GetObj("?SpecialSkinClass");
    const Params t = eng::Call(skin, "GetCosmeticType");
    if (const uint8_t* v = t.Get("CustomizationType")) l.type = *v;
    if (m.Invoked() && t.Invoked()) gLooks.push_back({eng::MakeWeak(skin), l});
    return l;
}

// The menu ball shows a skin's own actor for any type; the racing ball only for type 1 (its LoadBallCustomization).
bool UsesSkinActor(const SkinLook& l, bool menu) { return l.special && (menu || l.type == 1); }

Obj SkinActor(Obj ball) {
    Obj actor = eng::ReadObj(ball, "CustomSkinChild");
    return actor && eng::IsLive(actor) ? actor : nullptr;
}

Obj ParentOf(Obj material) {
    return eng::IsA(material, eng::FindClass("MaterialInstance")) ? eng::ReadObj(material, "Parent") : nullptr;
}

bool WearsSkin(Obj ball, Obj sphere, Obj skin, bool menu) {
    const SkinLook l = LookOf(skin);
    Obj actor = SkinActor(ball);
    if (UsesSkinActor(l, menu)) return actor && eng::ClassOf(actor) == l.special && !Hidden(actor);
    if (actor && !Hidden(actor)) return false;
    Obj material = eng::Call(sphere, "GetMaterial", int32_t{0}).ReturnObj();
    if (l.type == 0) return ParentOf(material) == l.material;     // a dynamic copy of it
    return material == l.material;
}

Obj GameProfile() {
    Obj instance = GameInstance();
    return instance ? eng::Call(instance, "GetCleanBallerProfile").GetObj("CleanProfile") : nullptr;
}

void PutSkin(Obj ball, Obj sphere, Obj skin, bool menu) {
    const SkinLook l = LookOf(skin);
    if (!l.material && !l.special) return;
    struct Prefs {
        uint8_t bytes[16];
    } prefs{};
    if (menu) {
        if (UsesSkinActor(l, true)) {
            eng::Call(ball, "SetSpecialBall", uint8_t{1}, l.special);
        } else if (l.type == 0) {
            eng::Call(ball, "ReassignDMI", l.material);
            eng::ReadBytes(ball, "BallerSkinPrefsToSave", &prefs, sizeof prefs);   // what the menu ball last used
            eng::Call(ball, "DetermineMIParams", prefs);
            eng::Call(ball, "AssignBasicBallParams", Str(L"cosmetics: local skin"));
        } else {
            eng::Call(sphere, "SetMaterial", int32_t{0}, l.material);
            eng::Call(ball, "SetSpecialBall", uint8_t{0}, Obj{nullptr});
        }
        return;
    }
    if (Obj actor = SkinActor(ball)) eng::Call(actor, "K2_DestroyActor");
    SetObject(ball, "CustomSkinChild", nullptr);
    SetObject(ball, "?SpecialSkinClass", l.special);
    if (UsesSkinActor(l, false)) {
        const Params setup = eng::Call(ball, "SpecialSkinSetup");
        if (const uint8_t* ok = setup.Get("bSuccess"); ok && *ok) return;
    }
    eng::Call(sphere, "SetHiddenInGame", uint8_t{0}, uint8_t{0});
    if (l.type == 0) {
        Obj dmi = eng::Call(Library("KismetMaterialLibrary"), "CreateDynamicMaterialInstance", ball, l.material, Name{},
                            uint8_t{0})
                      .ReturnObj();
        if (!dmi) return;
        SetObject(ball, "BasicBallDMI", dmi);
        if (Obj profile = GameProfile()) eng::ReadBytes(profile, "BallerSkinPreferences", &prefs, sizeof prefs);
        eng::Call(ball, "DetermineMIParams", prefs);
        eng::Call(sphere, "SetMaterial", int32_t{0}, dmi);
    } else {
        eng::Call(sphere, "SetMaterial", int32_t{0}, l.material);
    }
}

Obj HatMesh(Obj accessory) { return accessory ? eng::Call(accessory, "GetAccessoryMesh").GetObj("AccessoryMesh") : nullptr; }

// The menu ball's drag turns its "Sphere Component" (read from BP_MenuBall: HandleRotation adds the drag to it). For a
// skin drawn by its own actor the game points it at that actor's sphere (GetSkinSphere), so with a custom ball over
// such a skin the drag turned the hidden skin (its shadow moved) and not the custom ball (reported). While a custom
// ball is worn the drag turns the ball's own sphere, which the custom look and its model are on.
bool HasDrag(Obj ball) { return eng::FindProp(eng::ClassOf(ball), "Sphere Component").size > 0; }

void DragTurns(Obj ball, Obj sphere, Replaced* r) {
    if (!r || !HasDrag(ball)) return;
    Obj now = eng::ReadObj(ball, "Sphere Component");
    if (now == sphere) return;
    r->dragged = eng::MakeWeak(now);
    SetObject(ball, "Sphere Component", sphere);
}

void WearBall(Obj ball, Obj sphere, const Custom* custom) {
    Obj skinActor = eng::ReadObj(ball, "CustomSkinChild");
    if (skinActor && !eng::IsLive(skinActor)) skinActor = nullptr;
    Obj material = eng::Call(sphere, "GetMaterial", int32_t{0}).ReturnObj();
    Obj mesh = eng::ReadObj(sphere, "StaticMesh");
    Obj wanted = custom ? eng::Get(custom->material) : nullptr;
    Replaced* r = RecordOf(sphere);
    if (Obj original = FadeOriginal(ball, sphere, material)) material = original;     // the camera fade's copy of it
    if (wanted) {
        const bool sphereShown = eng::Call(sphere, "IsVisible").ReturnBool();
        const bool skinShown = skinActor && !Hidden(skinActor);
        DragTurns(ball, sphere, r);                 // also after the game points it elsewhere again (a page preview)
        if (material == wanted && mesh == Sphere() && sphereShown && !skinShown) return;
        if (!r) {                                    // the game's own look, to put back later
            bool hiddenInGame = false, visible = true;
            eng::ReadBool(sphere, "bHiddenInGame", &hiddenInGame);
            eng::ReadBool(sphere, "bVisible", &visible);
            gReplaced.push_back({eng::MakeWeak(sphere), eng::MakeWeak(mesh == Sphere() ? nullptr : mesh),
                                 eng::MakeWeak(IsCustomMaterial(material) ? nullptr : material), eng::MakeWeak(skinActor),
                                 hiddenInGame, !visible});
            r = &gReplaced.back();
            DragTurns(ball, sphere, r);
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
        if (Obj before = eng::Get(r->dragged); before && HasDrag(ball) && eng::ReadObj(ball, "Sphere Component") == sphere)
            SetObject(ball, "Sphere Component", before);
        Forget(r);
    }
}

// Models built on a component (the ball's sphere, or its hat slot): one per component and slot ("" for the ball's or
// the hat's own, an extra slot's name for an extra, which is built on the sphere beside the ball's).
struct ModelOn {
    eng::Weak component;
    std::string id;
    models::Built built;
    std::string slot;
};
std::vector<ModelOn> gModels;

}  // namespace

std::vector<Obj> ModelActorsOn(Obj ball) {
    std::vector<Obj> out;
    for (const auto& m : gModels) {
        Obj component = eng::Get(m.component);
        if (!component || eng::OuterOf(component) != ball) continue;
        for (Obj actor : models::Actors(m.built)) out.push_back(actor);
    }
    return out;
}

namespace {
void RemoveModel(Obj component, const std::string& slot = "") {
    for (size_t i = 0; i < gModels.size(); ++i)
        if (eng::Get(gModels[i].component) == component && gModels[i].slot == slot) {
            models::Destroy(gModels[i].built);
            gModels.erase(gModels.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

void WearModel(Obj component, const Custom* c, Obj ball, const std::string& slot = "") {
    ModelOn* on = nullptr;
    for (auto& m : gModels)
        if (eng::Get(m.component) == component && m.slot == slot) on = &m;
    if (on && (on->id != c->id || !on->built.Alive())) {
        RemoveModel(component, slot);
        on = nullptr;
    }
    if (!on) {
        gModels.push_back({eng::MakeWeak(component), c->id, models::Build(c->model, component), slot});
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
        // Without a record the slot shows the game's own hat (a record only goes once that is back), even when a
        // custom hat uses the same mesh: a hat built on the player's own (reported: taking it off left no hat).
        if (!r) {
            gReplaced.push_back({eng::MakeWeak(slot), eng::MakeWeak(mesh), {}, {}, false, false, {scale.v[0], scale.v[1], scale.v[2]}});
            r = &gReplaced.back();
        }
        if (mesh != wanted) eng::Call(slot, "SetStaticMesh", wanted);
        r->worn = eng::MakeWeak(wanted);
        if (scale.v[0] != hat->scale) eng::Call(slot, "SetRelativeScale3D", Vector{{hat->scale, hat->scale, hat->scale}});
        if (hat->hasModel) WearModel(slot, hat, ball);
        else RemoveModel(slot);
    } else if (r) {
        if (mesh == eng::Get(r->worn))                  // otherwise the game has put its own on since
            eng::Call(slot, "SetStaticMesh", eng::Get(r->mesh));
        // The slot's size goes back whatever mesh is on it now: the host changed it, and a custom hat's size left on it
        // made every hat after it bigger (reported).
        eng::Call(slot, "SetRelativeScale3D", Vector{{r->scale[0], r->scale[1], r->scale[2]}});
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

// Balls wearing a local game cosmetic, to put the public one back on when it is taken off.
struct Forced {
    eng::Weak ball;
    bool skin = false, hat = false;
};
std::vector<Forced> gForced;
bool gBfxForced = false;

Forced& ForcedOf(Obj ball) {
    for (auto& f : gForced)
        if (eng::Get(f.ball) == ball) return f;
    gForced.push_back({eng::MakeWeak(ball)});
    return gForced.back();
}

// What each racing ball's camera fade was last set up with (see Wear).
struct FadeState {
    eng::Weak ball;
    std::string look;                               // the local look's ids; empty for the game's own
    std::vector<Obj> actors;                        // the models' actors registered with it
};
std::vector<FadeState> gFades;

FadeState& FadeStateOf(Obj ball) {
    for (auto& f : gFades)
        if (eng::Get(f.ball) == ball) return f;
    gFades.push_back({eng::MakeWeak(ball), "", {}});
    return gFades.back();
}

bool PageShown(Obj page) {
    bool active = false;
    return page && eng::ReadBool(page, "bActive", &active) && active;
}

// The public choice a ball shows by itself: the racing ball's own (from the profile when it was made), the menu ball's
// the page's pending choice while the page is shown, else the profile's.
Obj PublicChoice(Obj ball, bool menu, int kind) {
    if (!menu) return eng::ReadObj(ball, kind == 0 ? "SkinAsset" : "AccessoryAsset");
    Obj page = eng::Get(gPage);
    if (PageShown(page)) return eng::ReadObj(page, kToSave[kind]);
    Obj profile = GameProfile();
    return profile ? eng::ReadObj(profile, kind == 0 ? "DefaultBallSkin" : "DefaultAccessory") : nullptr;
}

void SetHatMesh(Obj slot, Obj mesh) {
    if (eng::ReadObj(slot, "StaticMesh") != mesh) eng::Call(slot, "SetStaticMesh", mesh);
}

// The checkpoints' goal explosions set up with this one, where they are not already. Its effect and scale tell them
// apart (InitParticle sets the component's Asset and scale from the data asset: measured).
void WearExplosion(Obj asset) {
    Obj effect = eng::ReadObj(asset, "NiagaraSystem");
    double wanted[3] = {1, 1, 1};
    eng::ReadBytes(asset, "RelativeScale", wanted, sizeof wanted);
    for (const auto& weak : gExplosions) {
        Obj explosion = eng::Get(weak);
        double scale[3] = {};
        if (explosion && eng::ReadBytes(explosion, "RelativeScale3D", scale, sizeof scale) &&
            (scale[0] != wanted[0] || eng::ReadObj(explosion, "Asset") != effect))
            eng::Call(explosion, "InitParticle", asset);
    }
}

void Wear() {
    const Custom* ball = Worn(Kind::Ball);
    const Custom* hat = Worn(Kind::Hat);
    Obj gameSkin = ball ? nullptr : eng::Get(gLocalGame[0]);
    Obj gameHat = hat ? nullptr : eng::Get(gLocalGame[1]);
    // While the Customize page is in public mode, the menu ball previews the public choice.
    const bool publicPreview = gMode == kPublic && PageShown(eng::Get(gPage));
    Obj menuClass = eng::FindClass("BP_MenuBall_C");
    for (const auto& weak : gBalls) {
        Obj actor = eng::Get(weak);
        if (!actor) continue;
        const bool menu = eng::ClassOf(actor) == menuClass;
        const bool local = !(menu && publicPreview);
        // The camera fade (racing balls): its originals go back before the look changes and it takes new snapshots
        // after, with the models' actors (see FadeOf). Nothing is asked of it while the game's own look is worn.
        Obj fade = menu ? nullptr : FadeOf(actor);
        std::string look;
        if (fade && local) {
            if (ball) look += "ball " + ball->id + ";";
            if (hat) look += "hat " + hat->id + ";";
            if (gameSkin) look += "skin " + eng::PathOf(gameSkin) + ";";
            if (gameHat) look += "accessory " + eng::PathOf(gameHat) + ";";
            for (const std::string& slotName : ExtraSlots())
                if (const Custom* extra = WornExtra(slotName)) look += slotName + " " + extra->id + ";";
        }
        FadeState* faded = fade ? &FadeStateOf(actor) : nullptr;
        const bool changing = faded && look != faded->look;
        if (changing) eng::Call(fade, "BeginCameraFadeAppearanceChange", actor);
        for (int pass = 0; pass < 2; ++pass) {
            Forced& forced = ForcedOf(actor);
            if (Obj sphere = eng::ReadObj(actor, "Sphere")) {
                if (Obj skin = local ? gameSkin : nullptr) {
                    if (!WearsSkin(actor, sphere, skin, menu)) PutSkin(actor, sphere, skin, menu);
                    forced.skin = true;
                } else if (forced.skin) {
                    forced.skin = false;
                    if (Obj own = PublicChoice(actor, menu, 0); own && !WearsSkin(actor, sphere, own, menu)) PutSkin(actor, sphere, own, menu);
                }
                const Custom* custom = local ? ball : nullptr;
                WearBall(actor, sphere, custom);
                if (custom && custom->hasModel) WearModel(sphere, custom, actor);
                else RemoveModel(sphere);
                // Extras (arms, ...): built on the sphere beside the ball's own model, local only like custom cosmetics.
                for (const std::string& slotName : ExtraSlots()) {
                    const Custom* extra = local ? WornExtra(slotName) : nullptr;
                    if (extra && extra->hasModel) WearModel(sphere, extra, actor, slotName);
                    else RemoveModel(sphere, slotName);
                }
            }
            if (Obj slot = eng::ReadObj(actor, "AccessorySlot")) {
                if (Obj accessory = local ? gameHat : nullptr) {
                    SetHatMesh(slot, HatMesh(accessory));
                    forced.hat = true;
                } else if (forced.hat) {
                    forced.hat = false;
                    if (Obj own = PublicChoice(actor, menu, 1)) SetHatMesh(slot, HatMesh(own));
                }
                WearHat(actor, slot, local ? hat : nullptr);
                // An extra's tile is an accessory, so the page previewed "no hat" on the menu ball when it was picked: the
                // hat the ball should show goes back on (a custom or local game hat is put back above, each frame).
                if (menu && gFixMenuHat && !(local && (hat || gameHat)))
                    if (Obj own = PublicChoice(actor, menu, 1)) SetHatMesh(slot, HatMesh(own));
            }
            if (!faded) break;
            const std::vector<Obj> actors = ModelActorsOn(actor);
            if (!changing && actors == faded->actors) break;
            if (pass == 0 && !changing) {                   // a model built again: originals back, and worn again
                eng::Call(fade, "BeginCameraFadeAppearanceChange", actor);
                continue;
            }
            for (Obj part : actors) eng::Call(fade, "RegisterCameraFadeCosmeticActor", actor, part);
            eng::Call(fade, "RefreshCameraFadeAppearance", actor);
            faded->look = look;
            faded->actors = actors;
            hostlog::Info("cosmetics: camera fade set up for " + (look.empty() ? std::string("the game's own look") : look) +
                          " (" + std::to_string(actors.size()) + " model actors)");
            break;
        }
    }
    gFixMenuHat = false;
    gFades.erase(std::remove_if(gFades.begin(), gFades.end(), [](const FadeState& f) { return !eng::Get(f.ball); }), gFades.end());
    gForced.erase(std::remove_if(gForced.begin(), gForced.end(), [](const Forced& f) { return !eng::Get(f.ball); }), gForced.end());
    for (size_t i = gModels.size(); i-- > 0;)          // balls gone with their map
        if (!eng::Get(gModels[i].component)) {
            models::Destroy(gModels[i].built);
            gModels.erase(gModels.begin() + static_cast<std::ptrdiff_t>(i));
        }
    // Checkpoints set up their explosion from the profile's choice; the local one is set up over it.
    if (Obj bfx = LocalAsset(2)) {
        WearExplosion(bfx);
        gBfxForced = true;
    } else if (gBfxForced) {
        gBfxForced = false;
        Obj profile = GameProfile();
        if (Obj own = profile ? eng::ReadObj(profile, "DefaultGoalExplo") : nullptr) WearExplosion(own);
    }
}

// The menu ball as the page's mode shows it, at once: the public choice, with any local one taken off (Wear puts the
// local one back on in local mode). A custom one left on it could otherwise stay: the game only changes the ball when a
// tile is picked.
void ShowModeOnMenuBall(Obj page) {
    Obj ball = eng::ReadObj(page, "As BP Menu Ball");
    if (!ball || !eng::IsLive(ball)) return;
    Obj sphere = eng::ReadObj(ball, "Sphere"), slot = eng::ReadObj(ball, "AccessorySlot");
    if (sphere) {
        WearBall(ball, sphere, nullptr);
        RemoveModel(sphere);
        if (Obj own = eng::ReadObj(page, kToSave[0])) PutSkin(ball, sphere, own, true);
    }
    if (slot) {
        WearHat(ball, slot, nullptr);
        if (Obj own = eng::ReadObj(page, kToSave[1])) SetHatMesh(slot, HatMesh(own));
    }
    ForcedOf(ball) = {eng::MakeWeak(ball)};
}

// The page's highlight: in local mode the local choice (the public one while there is none); in public mode the
// game highlights its own choice, and this only runs when the mode changes.
void Highlight(Obj page, int tab) {
    Obj wanted = gMode == kLocal ? LocalAsset(tab) : nullptr;
    if (!wanted) wanted = eng::ReadObj(page, kToSave[tab]);
    auto mark = [&](Obj tile) {
        bool active = false;
        eng::ReadBool(tile, "Active", &active);
        Obj data = eng::ReadObj(tile, "CosmeticData");
        const Custom* c = ByAsset(data);
        bool on = wanted && data == wanted;
        if (c && !c->extra.empty()) {               // an extra's section: its worn extra, or "none"
            const Custom* worn = WornExtra(c->extra);
            on = worn ? worn == c : c->none;
        }
        if (active != on) eng::Call(tile, "SetIsActive", static_cast<uint8_t>(on));
    };
    for (const auto& weak : gTiles)
        if (Obj tile = eng::Get(weak)) mark(tile);
    ForEachGameTile(page, mark);
}

// --- the public and local buttons ----------------------------------------------------------------------------------
// Two of the game's tab buttons (WBP_TabButton_C, coloured as the page's own tabs), right of the cosmetics panel at its
// top, each with the ball and hat it stands for. The panel is the page's MenuOverlay (the scroll box's grandparent,
// read from the page's widget tree); the buttons are in a box on its right edge, moved past it by their own width.
constexpr float kThumbSize = 36, kPanelGap = 24;
const char* const kModeLabel[2] = {"local", "public"};
struct ModeButton {
    eng::Weak tab, hitbox, text, ball, hat;
    eng::Weak shownBall, shownHat;
    bool wasPressed = false;
};
ModeButton gModeButtons[2];
eng::Weak gModeBox;
double gModeBoxWidth = -1;
bool gPageWasShown = false;

Obj Parent(Obj widget) { return widget ? eng::Call(widget, "GetParent").ReturnObj() : nullptr; }

Obj CosmeticsPanel(Obj page) {
    Obj overlay = Parent(Parent(Parent(eng::ReadObj(page, "HB_CollectionSubheader"))));
    return eng::IsA(overlay, eng::FindClass("Overlay")) ? overlay : nullptr;
}

void RemoveModeButtons() {
    if (Obj box = eng::Get(gModeBox)) eng::Call(box, "RemoveFromParent");
    gModeBox = {};
    gModeBoxWidth = -1;
    for (auto& b : gModeButtons) b = {};
}

Obj Thumbnail(Obj tab, Obj row) {
    Obj image = w::Spawn("Image", eng::ReadObj(tab, "WidgetTree"));
    if (!image || !w::AddToRow(row, image, 4)) return nullptr;
    eng::Call(image, "SetDesiredSizeOverride", w::Vec2{kThumbSize, kThumbSize});
    w::SetVisibility(image, w::kCollapsed);
    return image;
}

void BuildModeButtons(Obj page) {
    RemoveModeButtons();
    Obj panel = CosmeticsPanel(page), reference = eng::ReadObj(page, "Skins_Tab"), tree = eng::ReadObj(page, "WidgetTree");
    Obj tabClass = eng::FindClass("WBP_TabButton_C");
    if (!panel || !reference || !tree || !tabClass) {
        static bool logged = false;
        if (!logged) hostlog::Warn("cosmetics: the Customize page is not laid out as measured; no public/local buttons");
        logged = true;
        return;
    }
    Obj box = w::Spawn("VerticalBox", tree);
    if (!box || !w::AddToOverlay(panel, box, w::kAlignEnd, w::kAlignLeft, {0, 0, 0, 0})) return;
    gModeBox = eng::MakeWeak(box);
    for (int i = 0; i < 2; ++i) {
        Obj tab = eng::Call(Library("WidgetBlueprintLibrary"), "Create", page, tabClass, game::PlayerController()).ReturnObj();
        if (!tab) return;
        // The page's tabs' colours and side, before the button is built (its PreConstruct applies them).
        for (const char* p : {"BackgroundActiveColor", "BackgroundInactiveColor", "TextActiveColor", "TextInactiveColor",
                              "TextHoverColor", "BubHoveredColor", "BubUnhoveredColor", "bLeftSide"})
            CopyProperty(reference, tab, p);
        Obj slot = eng::Call(box, "AddChildToVerticalBox", tab).ReturnObj();
        if (slot) {
            eng::Call(slot, "SetHorizontalAlignment", w::kAlignLeft);
            eng::Call(slot, "SetPadding", w::Margin{0, i ? 14.0f : 0.0f, 0, 0});
        }
        ModeButton& b = gModeButtons[i];
        b.tab = eng::MakeWeak(tab);
        b.hitbox = eng::MakeWeak(eng::ReadObj(tab, "HitBox"));
        b.text = eng::MakeWeak(eng::ReadObj(tab, "ButtonText"));
        if (Obj row = Parent(eng::ReadObj(tab, "Icon"))) {
            b.ball = eng::MakeWeak(Thumbnail(tab, row));
            b.hat = eng::MakeWeak(Thumbnail(tab, row));
        }
        w::SetText(eng::Get(b.text), kModeLabel[i]);
        eng::Call(tab, "SetIsActive", static_cast<uint8_t>(i == gMode));
    }
    hostlog::Info("cosmetics: public/local buttons on the Customize page");
}

void ShowThumbnail(const eng::Weak& image, eng::Weak& shown, Obj asset) {
    Obj widget = eng::Get(image);
    Obj texture = asset ? eng::ReadObj(asset, "PreviewTexture") : nullptr;
    if (!widget || (eng::Get(shown) == texture && texture)) return;
    shown = eng::MakeWeak(texture);
    if (texture) eng::Call(widget, "SetBrushFromTexture", texture, uint8_t{0});
    eng::Call(widget, "SetDesiredSizeOverride", w::Vec2{kThumbSize, kThumbSize});
    w::SetVisibility(widget, texture ? w::kSelfHitTestInvisible : w::kCollapsed);
}

// The buttons' pictures and labels, and their place: past the panel's right edge by their width.
void UpdateModeButtons(Obj page) {
    for (int i = 0; i < 2; ++i) {
        ModeButton& b = gModeButtons[i];
        Obj ball = eng::ReadObj(page, kToSave[0]), hat = eng::ReadObj(page, kToSave[1]);
        if (i == kLocal) {
            if (Obj a = LocalAsset(0)) ball = a;
            if (Obj a = LocalAsset(1)) hat = a;
        }
        ShowThumbnail(b.ball, b.shownBall, ball);
        ShowThumbnail(b.hat, b.shownHat, HatMesh(hat) ? hat : nullptr);    // no picture for "no hat"
        if (Obj text = eng::Get(b.text); text && w::ReadText(text) != kModeLabel[i]) w::SetText(text, kModeLabel[i]);  // rebuilt
    }
    Obj box = eng::Get(gModeBox);
    w::Vec2 desired{-1, -1};
    if (box) desired = eng::Call(box, "GetDesiredSize").ReturnAs<w::Vec2>(desired);
    if (desired.x > 0 && desired.x != gModeBoxWidth) {
        gModeBoxWidth = desired.x;
        eng::Call(box, "SetRenderTranslation", w::Vec2{desired.x + kPanelGap, 0});
    }
}


// --- a tab for each extra slot -------------------------------------------------------------------------------------
// After the page's own tabs (balls, hats, bfx) come one for each extra slot ("arms"): the game's tab button
// (WBP_TabButton_C, coloured as the page's own, in the same stack, spaced as the hats tab is). Choosing one hides the
// page's own list (SizeBox_0, the scroll box's parent in MenuOverlay: read from the page's widget tree) and shows a
// list of the host's own in its place, with that slot's section: "none" first, then its extras. Extras are local
// choices, so the page switches to local mode. Choosing one of the page's own tabs (its handler then switches, read
// from SwitchActiveTab: it makes its tabs active and inactive itself) or public mode brings the page's list back.
struct ExtraTab {
    std::string slot;
    eng::Weak box, tab, hitbox, text;               // box: the tab with a spacer above it, in the page's tab stack
    bool wasPressed = false;
};
std::vector<ExtraTab> gExtraTabs;
eng::Weak gExtraBox, gExtraScroll;                  // the host's list, in the page's MenuOverlay
eng::Weak gGameList;                                // the page's list (SizeBox_0), hidden while an extra tab is shown
uint8_t gGameListVisibility = 0;
std::string gShownExtra;                            // the extra slot whose tab is shown, or ""
std::vector<eng::Weak> gExtraTiles;
bool gGameTabWasPressed[3] = {};
const char* const kGameTabs[3] = {"Skins_Tab", "Hats_Tab", "BFX_Tab"};

void RemoveExtraTabs() {
    for (auto& t : gExtraTabs)
        if (Obj box = eng::Get(t.box)) eng::Call(box, "RemoveFromParent");
    gExtraTabs.clear();
}

void BuildExtraTabs(Obj page) {
    RemoveExtraTabs();
    Obj stack = eng::ReadObj(page, "PLAYLISTTABS_Stack"), reference = eng::ReadObj(page, "BFX_Tab");
    Obj tree = eng::ReadObj(page, "WidgetTree");
    Obj tabClass = eng::FindClass("WBP_TabButton_C");
    // The hats tab's slot has 14 above and below it (read from the page) and the others none, so the tabs are 14
    // apart. A StackBoxSlot has no setters on this build (measured: SetPadding missing), so each extra tab is in a
    // VerticalBox under a 14-high spacer, aligned right as the page's tabs are.
    double gap = 14;
    struct Margin {
        float left, top, right, bottom;
    } hatsPadding{};
    if (eng::ReadBytes(eng::ReadObj(eng::ReadObj(page, "Hats_Tab"), "Slot"), "Padding", &hatsPadding, sizeof hatsPadding))
        gap = hatsPadding.top;
    if (!stack || !reference || !tabClass || !tree) {
        static bool logged = false;
        if (!logged) hostlog::Warn("cosmetics: the Customize page's tabs are not laid out as measured; no extra tabs");
        logged = true;
        return;
    }
    for (const std::string& slot : ExtraSlots()) {
        Obj tab = eng::Call(Library("WidgetBlueprintLibrary"), "Create", page, tabClass, game::PlayerController()).ReturnObj();
        if (!tab) return;
        // The page's tabs' colours and side, before the button is built (its PreConstruct applies them).
        for (const char* prop : {"BackgroundActiveColor", "BackgroundInactiveColor", "TextActiveColor", "TextInactiveColor",
                                 "TextHoverColor", "BubHoveredColor", "BubUnhoveredColor", "bLeftSide"})
            CopyProperty(reference, tab, prop);
        Obj box = w::Spawn("VerticalBox", tree), spacer = w::Spawn("Spacer", tree);
        if (!box || !spacer) return;
        eng::Call(spacer, "SetSize", w::Vec2{1, gap});
        eng::Call(box, "AddChildToVerticalBox", spacer);
        if (Obj tabSlot = eng::Call(box, "AddChildToVerticalBox", tab).ReturnObj()) eng::Call(tabSlot, "SetHorizontalAlignment", w::kAlignEnd);
        eng::Call(stack, "AddChildToStackBox", box);
        w::SetVisibility(box, gMode == kLocal ? w::kSelfHitTestInvisible : w::kCollapsed);
        ExtraTab t;
        t.slot = slot;
        t.box = eng::MakeWeak(box);
        t.tab = eng::MakeWeak(tab);
        t.hitbox = eng::MakeWeak(eng::ReadObj(tab, "HitBox"));
        t.text = eng::MakeWeak(eng::ReadObj(tab, "ButtonText"));
        if (Obj text = eng::Get(t.text)) {
            if (Obj refText = eng::ReadObj(reference, "ButtonText")) w::CopyFont(refText, text);
            w::SetText(text, slot);
        }
        eng::Call(tab, "SetIsActive", static_cast<uint8_t>(slot == gShownExtra));
        gExtraTabs.push_back(std::move(t));
    }
    if (!gExtraTabs.empty()) hostlog::Info("cosmetics: " + std::to_string(gExtraTabs.size()) + " extra tab(s) on the Customize page");
}

void ShowGameTabs(Obj page, bool active) {
    if (active) return;                             // the page's SwitchActiveTab makes them active itself
    for (const char* name : kGameTabs)
        if (Obj tab = eng::ReadObj(page, name)) eng::Call(tab, "SetIsActive", uint8_t{0});
}

void LeaveExtraTab(Obj page, bool switchBack) {
    if (gShownExtra.empty()) return;
    gShownExtra.clear();
    if (Obj box = eng::Get(gExtraBox)) w::SetVisibility(box, w::kCollapsed);
    if (Obj list = eng::Get(gGameList)) w::SetVisibility(list, gGameListVisibility);
    for (auto& t : gExtraTabs)
        if (Obj tab = eng::Get(t.tab)) eng::Call(tab, "SetIsActive", uint8_t{0});
    if (switchBack && page) {                       // the page's own tab back, as the page does it
        int32_t tab = 0;
        eng::ReadBytes(page, "ActiveTabIndex", &tab, sizeof tab);
        eng::Call(page, "SwitchActiveTab", tab);
    }
}

void SetMode(Obj page, int mode);

void ShowExtraTab(Obj page, const std::string& slot) {
    Obj gameList = Parent(Parent(eng::ReadObj(page, "HB_CollectionSubheader")));
    Obj panel = Parent(gameList), tree = eng::ReadObj(page, "WidgetTree");
    if (gMode == kPublic) SetMode(page, kLocal);    // extras are local choices
    // Extras' tiles are hat tiles (their assets are accessories): the page's tiles are a pool it fills per tab, so the
    // hats tab is built first (the page's own SwitchActiveTab) to have one to copy; leaving comes back to it.
    int32_t active = 0;
    eng::ReadBytes(page, "ActiveTabIndex", &active, sizeof active);
    if (active != 1 || !AnyTile(page, Kind::Hat)) eng::Call(page, "SwitchActiveTab", int32_t{1});
    Obj refButton = AnyTile(page, Kind::Hat);
    if (!gameList || !panel || !tree || !refButton) {
        hostlog::Warn("cosmetics: the Customize page is not laid out as measured; the " + slot + " tab can't be shown");
        return;
    }
    // The host's list: a SizeBox like the page's (its sizes copied) holding a scroll box, in the same place.
    Obj box = eng::Get(gExtraBox), scroll = eng::Get(gExtraScroll);
    if (!box || !scroll || !Parent(box)) {
        box = w::Spawn("SizeBox", tree);
        scroll = w::Spawn("ScrollBox", tree);
        if (!box || !scroll) return;
        for (const char* prop : {"WidthOverride", "HeightOverride", "MinDesiredWidth", "MinDesiredHeight", "MaxDesiredWidth",
                                 "MaxDesiredHeight", "bOverride_WidthOverride"})
            CopyProperty(gameList, box, prop);
        w::AddChild(box, scroll);
        Obj slotObj = eng::Call(panel, "AddChildToOverlay", box).ReturnObj();
        CopySlot(eng::ReadObj(gameList, "Slot"), slotObj);
        gExtraBox = eng::MakeWeak(box);
        gExtraScroll = eng::MakeWeak(scroll);
    }
    eng::Call(scroll, "ClearChildren");
    gExtraTiles.clear();
    const uint8_t visible = eng::Call(refButton, "GetVisibility").ReturnAs<uint8_t>(w::kCollapsed);
    gTileVisibility = visible == w::kCollapsed ? w::kSelfHitTestInvisible : visible;
    int made = 0;
    AddSection(page, scroll, slot, refButton, [&](const Custom& c) { return c.extra == slot; }, &gExtraTiles, &made);
    if (gShownExtra.empty()) gGameListVisibility = eng::Call(gameList, "GetVisibility").ReturnAs<uint8_t>(0);
    gGameList = eng::MakeWeak(gameList);
    w::SetVisibility(gameList, w::kCollapsed);
    w::SetVisibility(box, w::kSelfHitTestInvisible);
    gShownExtra = slot;
    ShowGameTabs(page, false);
    for (auto& t : gExtraTabs)
        if (Obj tab = eng::Get(t.tab)) eng::Call(tab, "SetIsActive", static_cast<uint8_t>(t.slot == slot));
    hostlog::Info("cosmetics: " + slot + " tab with " + std::to_string(made) + " tile(s)");
}

// The extra tabs' clicks, and the page's own tabs' while an extra tab is shown (the page switches itself then).
void WatchExtraTabs(Obj page) {
    for (auto& t : gExtraTabs) {
        Obj hitbox = eng::Get(t.hitbox);
        if (!hitbox) continue;
        const bool pressed = eng::Call(hitbox, "IsPressed").ReturnBool();
        if (t.wasPressed && !pressed && eng::Call(hitbox, "IsHovered").ReturnBool() && t.slot != gShownExtra) ShowExtraTab(page, t.slot);
        t.wasPressed = pressed;
    }
    for (int i = 0; i < 3; ++i) {
        Obj hitbox = eng::ReadObj(eng::ReadObj(page, kGameTabs[i]), "HitBox");
        const bool pressed = hitbox && eng::Call(hitbox, "IsPressed").ReturnBool();
        if (gGameTabWasPressed[i] && !pressed && !gShownExtra.empty()) LeaveExtraTab(page, false);
        gGameTabWasPressed[i] = pressed;
    }
}

// The extra tab's highlight: the extra worn in its slot, or "none".
void MarkExtraTiles() {
    for (const auto& weak : gExtraTiles)
        if (Obj tile = eng::Get(weak)) {
            const Custom* c = ByAsset(eng::ReadObj(tile, "CosmeticData"));
            if (!c) continue;
            const Custom* worn = WornExtra(c->extra);
            const bool on = worn ? worn == c : c->none;
            bool active = false;
            eng::ReadBool(tile, "Active", &active);
            if (active != on) eng::Call(tile, "SetIsActive", static_cast<uint8_t>(on));
            if (eng::Call(tile, "GetVisibility").ReturnAs<uint8_t>(gTileVisibility) != gTileVisibility)
                eng::Call(tile, "SetVisibility", gTileVisibility);
        }
}

void SetMode(Obj page, int mode) {
    if (mode != kPublic && mode != kLocal) return;
    if (mode == kPublic) LeaveExtraTab(page, true);  // extras are local choices
    gMode = mode;
    for (int i = 0; i < 2; ++i)
        if (Obj tab = eng::Get(gModeButtons[i].tab)) eng::Call(tab, "SetIsActive", static_cast<uint8_t>(i == mode));
    // The custom sections are only for local choices.
    for (auto& section : gSections)
        for (eng::Weak* weak : {&section.header, &section.border})
            if (Obj widget = eng::Get(*weak)) w::SetVisibility(widget, mode == kLocal ? uint8_t{0} : w::kCollapsed);
    if (!page) return;
    ShowModeOnMenuBall(page);
    int32_t tab = 0;
    eng::ReadBytes(page, "ActiveTabIndex", &tab, sizeof tab);
    if (tab >= 0 && tab < 3) Highlight(page, tab);
    hostlog::Info(std::string("cosmetics: ") + kModeLabel[mode] + " mode");
}

// A click is a press that ends while the pointer is still over the button (as the footer's buttons).
void WatchModeButtons(Obj page) {
    for (int i = 0; i < 2; ++i) {
        ModeButton& b = gModeButtons[i];
        Obj hitbox = eng::Get(b.hitbox);
        if (!hitbox) continue;
        const bool pressed = eng::Call(hitbox, "IsPressed").ReturnBool();
        if (b.wasPressed && !pressed && eng::Call(hitbox, "IsHovered").ReturnBool() && gMode != i) SetMode(page, i);
        b.wasPressed = pressed;
    }
}

}  // namespace

// Referenced from the game instance's ReferencedObjects (a UPROPERTY array the collector follows; the game instance
// lives for the whole session). The root-set flag alone is not enough on this build: measured, an object with it set
// was still collected by the next garbage collection.
Obj LoadTexture(const std::wstring& file) { return Texture(file); }

// The game's LBall material (MI_LBall05) showing an image: as image balls are drawn, and textured model parts.
Obj ImageMaterial(Obj texture, const std::string& name) {
    Obj parent = LoadAsset(kImageBallMaterial);
    if (!texture || !parent) return nullptr;
    Obj material = eng::Call(Library("KismetMaterialLibrary"), "CreateDynamicMaterialInstance", game::PlayerController(), parent,
                             MakeName(name), uint8_t{0})
                       .ReturnObj();
    if (!material) return nullptr;
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
    // No LBall glow: its master (M_LBall05_Master, read from the cooked package) adds EmissiveColor through
    // T_LBall05_EmissiveMask, strength from EmissiveStrengthLow to EmissiveStrengthHigh by MPC_BallProperties'
    // BallSpeed, which drew LBall's yellow panels over the image at speed (reported).
    SetVector(material, "EmissiveColor", 0, 0, 0, 0);
    SetScalar(material, "EmissiveStrengthLow", 0);
    SetScalar(material, "EmissiveStrengthHigh", 0);
    return material;
}

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
    if (image.empty()) {                            // a clear ball
        Obj glass = LoadAsset(kGlassMaterial);
        Obj material = glass ? eng::Call(Library("KismetMaterialLibrary"), "CreateDynamicMaterialInstance", game::PlayerController(),
                                         glass, MakeName("CustomBall_" + id), uint8_t{0})
                                   .ReturnObj()
                             : nullptr;
        if (!material) {
            hostlog::Warn("cosmetics: ball " + id + ": the glass material did not load");
            return false;
        }
        KeepAlive(material);
        SetVector(material, "ColorGlass", 1, 1, 1, 1);
        // A model that is the whole ball (a glass sphere around it, "ball hidden") would show the game's ball inside it.
        SetScalar(material, "Opacity", c.hasModel && c.model.hideBall ? 0 : kClearBallOpacity);
        Obj asset = Create(Kind::Ball, id, name, Texture(preview));
        if (!asset) return false;
        SetObject(asset, "SkinMaterial", material);
        SetObject(asset, "SkinGhostVariant", material);
        c.asset = eng::MakeWeak(asset);
        c.material = eng::MakeWeak(material);
        Added(std::move(c));
        return true;
    }
    Obj texture = Texture(image);
    Obj parent = LoadAsset(kImageBallMaterial);
    if (!texture || !parent) {
        hostlog::Warn("cosmetics: ball " + id + ": " + (texture ? "the ball material did not load" : "the image did not load"));
        return false;
    }
    Obj material = ImageMaterial(texture, "CustomBall_" + id);
    if (!material) return false;
    KeepAlive(material);
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
    for (const auto& c : gCustoms) n += c.kind == kind && c.extra.empty() && eng::Get(c.asset) ? 1 : 0;
    return n;
}

namespace {
// The game's own "no hat" picture (DA_Accessory_None's PreviewTexture), for an extra slot's "none" tile.
Obj NonePicture() {
    static eng::Weak picture;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (Obj none = eng::FindObjectByName("DA_Accessory_None")) picture = eng::MakeWeak(eng::ReadObj(none, "PreviewTexture"));
    }
    return eng::Get(picture);
}

bool ValidSlot(const std::string& slot) {
    if (slot.empty() || slot.size() > 20) return false;
    for (char ch : slot)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == ' ')) return false;
    return true;
}

// An extra slot's "none" tile, made before its first extra so it comes first.
void EnsureNone(const std::string& slot) {
    for (const auto& c : gCustoms)
        if (c.extra == slot && c.none) return;
    Obj asset = Create(Kind::Hat, slot + ".none", "none", NonePicture());
    if (!asset) return;
    Custom c;
    c.kind = Kind::Hat;
    c.extra = slot;
    c.none = true;
    c.asset = eng::MakeWeak(asset);
    gCustoms.push_back(std::move(c));
}
}  // namespace

bool AddExtra(const std::string& slot, const std::string& id, const std::string& name, const std::wstring& preview,
              const std::string& model) {
    if (!ValidSlot(slot)) {
        hostlog::Warn("cosmetics: extra slot '" + slot + "' is not a name of lowercase letters, digits, dashes and spaces");
        return false;
    }
    if (id.empty()) return false;                   // model "": a tile only (a plugin does what it stands for)
    if (FindExtra(slot, id)) return true;            // already added (its plugin was reloaded)
    for (const auto& r : gWaitingExtras)
        if (r.slot == slot && r.id == id) return true;
    if (!game::PlayerController()) {
        gWaitingExtras.push_back({slot, id, name, preview, model});
        return true;
    }
    Custom c;
    c.kind = Kind::Hat;
    c.extra = slot;
    c.id = id;
    if (!ParseModel(&c, model)) return false;
    EnsureNone(slot);
    Obj asset = Create(Kind::Hat, id, name, Texture(preview));
    if (!asset) return false;
    if (Obj template_ = LoadAsset(kGhostHatSource)) CopyProperty(template_, asset, "AccessoryGhostMaterial");
    c.asset = eng::MakeWeak(asset);
    gCustoms.push_back(std::move(c));
    gSectionTab = -1;                               // rebuild the sections with it
    hostlog::Info("cosmetics: added " + slot + " " + id);
    return true;
}

bool EquipExtra(const std::string& slot, const std::string& id) {
    if (!ValidSlot(slot)) return false;
    if (!id.empty() && !FindExtra(slot, id)) {
        bool waiting = false;                       // a waiting one is worn once it is made
        for (const auto& r : gWaitingExtras) waiting |= r.slot == slot && r.id == id;
        if (!waiting) return false;
    }
    gEquippedExtra[slot] = id;
    return true;
}

std::string EquippedExtra(const std::string& slot) {
    const auto it = gEquippedExtra.find(slot);
    return it == gEquippedExtra.end() ? "" : it->second;
}

bool PreviewBall(double* x, double* y, double* z, double* radius, double* facing) {
    if (!PageShown(eng::Get(gPage))) return false;
    Obj menuClass = eng::FindClass("BP_MenuBall_C");
    for (const auto& weak : gBalls) {
        Obj actor = eng::Get(weak);
        Obj sphere = actor && eng::ClassOf(actor) == menuClass ? eng::ReadObj(actor, "Sphere") : nullptr;
        if (!sphere) continue;
        struct V {
            double x, y, z;
        };
        const V at = eng::Call(sphere, "K2_GetComponentLocation").ReturnAs<V>();
        const V scale = eng::Call(sphere, "K2_GetComponentScale").ReturnAs<V>();
        *x = at.x;
        *y = at.y;
        *z = at.z;
        *radius = 50 * scale.x;                         // the engine's sphere mesh is 50 across its radius
        *facing = 0;
        Obj controller = game::PlayerController();
        if (Obj manager = controller ? eng::ReadObj(controller, "PlayerCameraManager") : nullptr) {
            struct R {
                double pitch, yaw, roll;
            };
            *facing = eng::Call(manager, "GetCameraRotation").ReturnAs<R>().yaw + 180;
        }
        return true;
    }
    return false;
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
    if (!gWaitingExtras.empty() && game::PlayerController()) {
        const std::vector<ExtraRequest> waiting = std::move(gWaitingExtras);
        gWaitingExtras.clear();
        for (const auto& r : waiting) AddExtra(r.slot, r.id, r.name, r.preview, r.model);
    }
    // Local game cosmetics work without any custom one, so this runs from the moment the game is ready.
    if (!game::PlayerController()) return;
    if (!gLocalLoaded) LoadLocal();
    if (gLocalChanged) {                            // chosen in the page's handler
        gLocalChanged = false;
        for (int k = 0; k < 3; ++k)
            if (Obj asset = eng::Get(gLocalGame[k])) KeepAlive(asset);
        SaveLocal();
    }
    WatchChoices();
    static double lastSearch = -100;
    const bool search = game::Seconds() - lastSearch > 1;
    if (search) {                                   // the balls and checkpoints come and go with maps
        lastSearch = game::Seconds();
        FindTargets();
    }
    Obj page = eng::Get(gPage);
    if (!page && search && (page = LivePage())) gPage = eng::MakeWeak(page);
    if (page) {
        // The page opens in local mode each time it is shown.
        const bool shown = PageShown(page);
        if (shown && !gPageWasShown) SetMode(page, kLocal);
        gPageWasShown = shown;
        if (!eng::Get(gModeBox) || !Parent(eng::Get(gModeBox))) BuildModeButtons(page);
        WatchModeButtons(page);
        WatchExtraTabs(page);
    }
    Wear();
    static double lastLook = -100;
    if (game::Seconds() - lastLook < 0.2) return;
    lastLook = game::Seconds();
    if (!page) return;
    UpdateModeButtons(page);
    // One tab per extra slot, rebuilt when the slots change or the page rebuilt its tabs (ours then lost their parent).
    const size_t slots = ExtraSlots().size();
    if (gExtraTabs.size() != slots || (slots > 0 && !Parent(eng::Get(gExtraTabs.front().box)))) BuildExtraTabs(page);
    for (auto& t : gExtraTabs) {
        if (Obj text = eng::Get(t.text); text && w::ReadText(text) != t.slot) w::SetText(text, t.slot);   // (PreConstruct)
        // extras are local choices: their tabs show in local mode only
        const uint8_t wanted = gMode == kLocal ? w::kSelfHitTestInvisible : w::kCollapsed;
        if (Obj box = eng::Get(t.box); box && eng::Call(box, "GetVisibility").ReturnAs<uint8_t>(wanted) != wanted) w::SetVisibility(box, wanted);
    }
    if (!gShownExtra.empty()) MarkExtraTiles();
    int32_t tab = 0;
    eng::ReadBytes(page, "ActiveTabIndex", &tab, sizeof tab);
    // Rebuilt when the page is new, the tab changed, its sections changed (or could not be built yet), or the game
    // rebuilt its lists (our sections then lost their parent).
    Obj firstBorder = gSections.empty() ? nullptr : eng::Get(gSections.front().border);
    const char* why = page != eng::Get(gSectionPage) ? "new page"
                      : tab != gSectionTab     ? "tab"
                      : gSections.size() != SectionsOf(tab).size() ? "sections"
                      : firstBorder && !eng::Call(firstBorder, "GetParent").ReturnObj() ? "section removed"
                                                                                       : nullptr;
    if (why) BuildSection(page, tab);
    // A tile collapses itself when it is constructed; the page shows only the tiles it placed, so ours are shown here.
    for (const auto& weak : gTiles)
        if (Obj tile = eng::Get(weak))
            if (eng::Call(tile, "GetVisibility").ReturnAs<uint8_t>(gTileVisibility) != gTileVisibility)
                eng::Call(tile, "SetVisibility", gTileVisibility);
    if (tab >= 0 && tab < 3 && gMode == kLocal) Highlight(page, tab);
}

bool Equip(Kind kind, const std::string& id) {
    if (!id.empty() && !Exists(kind, id)) return false;     // a waiting one is worn once it is made
    gEquipped[static_cast<int>(kind)] = id;
    if (!id.empty() && !gLocalPath[static_cast<int>(kind)].empty()) SetLocalGame(static_cast<int>(kind), nullptr);
    return true;
}

bool ShowExtraTab(const std::string& slot) {
    Obj page = eng::Get(gPage);
    if (!page) return false;
    if (slot.empty()) LeaveExtraTab(page, true);
    else ShowExtraTab(page, slot);
    return slot.empty() || gShownExtra == slot;
}

bool ClickExtraTile(int index) {
    Obj tile = index >= 0 && index < static_cast<int>(gExtraTiles.size()) ? eng::Get(gExtraTiles[static_cast<size_t>(index)]) : nullptr;
    return tile && eng::Call(tile, "BndEvt__WBP_BasicBallSelect_Hitbox_K2Node_ComponentBoundEvent_5_OnButtonClickedEvent__DelegateSignature").Invoked();
}

bool SetLocalMode(bool local) {
    Obj page = eng::Get(gPage);
    if (!page) return false;
    SetMode(page, local ? kLocal : kPublic);
    return true;
}

std::string Equipped(Kind kind) { return gEquipped[static_cast<int>(kind)]; }

bool ClickTile(int index) {
    Obj tile = index >= 0 && index < static_cast<int>(gTiles.size()) ? eng::Get(gTiles[static_cast<size_t>(index)]) : nullptr;
    Obj page = eng::Get(gPage), cls = gSectionTab >= 0 ? eng::FindClass(kAssetClass[gSectionTab]) : nullptr;
    if (index < 0 && page && cls) {             // -n: the game's own n-th tile shown on this tab
        int n = -index;
        ForEachGameTile(page, [&](Obj t) {
            if (n > 0 && eng::Call(t, "GetVisibility").ReturnAs<uint8_t>(w::kCollapsed) != w::kCollapsed &&
                eng::IsA(eng::ReadObj(t, "CosmeticData"), cls) && --n == 0)
                tile = t;
        });
    }
    return tile && eng::Call(tile, "BndEvt__WBP_BasicBallSelect_Hitbox_K2Node_ComponentBoundEvent_5_OnButtonClickedEvent__DelegateSignature").Invoked();
}

namespace {
// Test: the extra slots, how many each has, and the one worn.
std::string ExtrasStatus() {
    std::string out = "; extra tabs " + std::to_string(gExtraTabs.size()) + ", shown '" + gShownExtra + "' (" +
                      std::to_string(gExtraTiles.size()) + " tile(s))";
    for (const auto& slot : ExtraSlots()) {
        int n = 0;
        for (const auto& c : gCustoms) n += c.extra == slot && !c.none ? 1 : 0;
        out += "; " + slot + ": " + std::to_string(n) + " (worn '" + EquippedExtra(slot) + "')";
    }
    return out;
}

// Test: the page's pending (public) choices and what the menu ball shows.
std::string PageStatus() {
    std::string out = "; profile:";
    Obj profile = GameProfile();
    for (const char* p : {"DefaultBallSkin", "DefaultAccessory", "DefaultGoalExplo"}) out += std::string(" ") + eng::ObjName(profile ? eng::ReadObj(profile, p) : nullptr);
    out += "; explosions " + std::to_string(gExplosions.size());
    for (const auto& weak : gExplosions)
        if (Obj explosion = eng::Get(weak)) {
            double scale[3] = {};
            eng::ReadBytes(explosion, "RelativeScale3D", scale, sizeof scale);
            out += " (first: " + eng::ObjName(eng::ReadObj(explosion, "Asset")) + " x" + std::to_string(scale[0]) + ")";
            break;
        }
    Obj page = eng::Get(gPage);
    if (!page) return out + "; no page";
    out += std::string("; page ") + (PageShown(page) ? "shown" : "hidden") + ", pending:";
    for (int k = 0; k < 3; ++k) out += std::string(" ") + kKindName[k] + " " + eng::ObjName(eng::ReadObj(page, kToSave[k]));
    Obj ball = eng::ReadObj(page, "As BP Menu Ball");
    if (!ball || !eng::IsLive(ball)) return out;
    Obj sphere = eng::ReadObj(ball, "Sphere"), slot = eng::ReadObj(ball, "AccessorySlot"), actor = SkinActor(ball);
    Obj material = sphere ? eng::Call(sphere, "GetMaterial", int32_t{0}).ReturnObj() : nullptr;
    out += "; menu ball: material " + eng::ObjName(material) + " (parent " + eng::ObjName(ParentOf(material)) +
           "), skin actor " + (actor ? eng::ObjName(eng::ClassOf(actor)) + (Hidden(actor) ? " hidden" : "") : "none") + ", hat " +
           eng::ObjName(slot ? eng::ReadObj(slot, "StaticMesh") : nullptr);
    return out;
}
}  // namespace

std::string Status() {
    return "cosmetics: " + std::to_string(Count(Kind::Ball)) + " ball(s), " + std::to_string(Count(Kind::Hat)) + " hat(s), " +
           std::to_string(Count(Kind::Bfx)) + " bfx; worn: ball '" + gEquipped[0] + "', hat '" + gEquipped[1] + "', bfx '" +
           gEquipped[2] + "'; section on tab " + std::to_string(gSectionTab) + "; " + std::to_string(gReplaced.size()) +
           " part(s) replaced; mode " + kModeLabel[gMode] + "; local game: ball '" + gLocalPath[0] + "', hat '" + gLocalPath[1] +
           "', bfx '" + gLocalPath[2] + "'" + ExtrasStatus() + PageStatus();
}

}  // namespace cosmetics
