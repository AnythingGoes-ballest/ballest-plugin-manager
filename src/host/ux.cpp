#include "ux.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#include "cosmetics.hpp"
#include "game.hpp"
#include "log.hpp"

namespace w = ui::widgets;

namespace ux {
namespace {

const wchar_t* kFont = L"/Game/UI/Fonts/CocogoosePro.CocogoosePro";
const wchar_t* kBubble = L"/Game/UI/UIMaterialLab/Widgets/ComponentMaterials/M_UI_BubbleButton.M_UI_BubbleButton";
constexpr double kPointsPerPixel = 0.75;    // a UMG font size is in points: 18 draws text 24 units tall (measured, tab buttons)

Obj Lib(const char* name) { return eng::FindCdo(name); }

void WriteName(Obj o, std::vector<const char*> path, const char* name) {
    const std::wstring wide = eng::Widen(name);
    const eng::Params made = eng::Call(Lib("KismetStringLibrary"), "Conv_StringToName",
                                       eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
    size_t size = 0;
    const uint8_t* r = made.Return(&size);
    eng::Prop last;
    const int off = o ? eng::NestedOffset(eng::ClassOf(o), path, &last) : -1;
    if (r && size == 8 && off >= 0 && last.size == 8) std::memcpy(o + off, r, 8);
}

template <class T>
void WriteAt(Obj o, std::vector<const char*> path, const T& value) {
    eng::Prop last;
    const int off = o ? eng::NestedOffset(eng::ClassOf(o), path, &last) : -1;
    if (off >= 0 && last.size == static_cast<int32_t>(sizeof value)) std::memcpy(o + off, &value, sizeof value);
}

// A text's face and size, written before it is on screen (its Slate widget reads the font when it is built).
void SetFace(Obj text, double px, const char* face) {
    if (Obj font = Font()) WriteAt(text, {"Font", "FontObject"}, font);
    WriteName(text, {"Font", "TypefaceFontName"}, face);
    w::SetFontSize(text, static_cast<float>(px * kPointsPerPixel));
}

Obj OnCanvas(Obj canvas, Obj child, int z, bool autoSize, bool centered, double alignX = 0.5) {
    Obj slot = child && canvas ? eng::Call(canvas, "AddChildToCanvas", child).ReturnObj() : nullptr;
    if (!slot) return nullptr;
    const double anchors[4] = {0, 0, 0, 0};
    eng::Params a(eng::FunctionOn(slot, "SetAnchors"));
    a.SetArg(0, anchors, sizeof anchors);
    eng::Invoke(slot, a);
    eng::Call(slot, "SetAlignment", w::Vec2{centered ? alignX : 0.0, centered ? 0.5 : 0.0});
    eng::Call(slot, "SetAutoSize", static_cast<uint8_t>(autoSize));
    eng::Call(slot, "SetZOrder", static_cast<int32_t>(z));
    eng::Call(slot, "SetPosition", w::Vec2{-4000, -4000});
    return slot;
}

}  // namespace

// The block takes a font only through SetFont (written into it alone, the text on screen kept its old size: a closed
// card's name stayed big): its own, outline and all, handed back to it.
void ResendFont(Obj text) {
    eng::Prop fp;
    const int fontAt = eng::NestedOffset(eng::ClassOf(text), {"Font"}, &fp);
    if (fontAt < 0 || fp.size <= 0) return;
    if (Obj fn = eng::FunctionOn(text, "SetFont")) {
        eng::Params p(fn);
        p.SetArg(0, text + fontAt, static_cast<size_t>(fp.size));
        eng::Invoke(text, p);
    }
}

void TextFace(Obj text, double px, const char* face) {
    if (!text) return;
    SetFace(text, px, face);
    ResendFont(text);
}

void TextOutline(Obj text, int size, const Color& c) {
    if (!text) return;
    Obj cls = eng::ClassOf(text);
    eng::Prop sp, cp;
    const int sizeAt = eng::NestedOffset(cls, {"Font", "OutlineSettings", "OutlineSize"}, &sp);
    const int colourAt = eng::NestedOffset(cls, {"Font", "OutlineSettings", "OutlineColor"}, &cp);
    if (sizeAt < 0 || sp.size != sizeof(int32_t)) return;
    const int32_t v = size;
    std::memcpy(text + sizeAt, &v, sizeof v);
    if (colourAt >= 0 && cp.size == sizeof(float) * 4) {
        const float f[4] = {c.r, c.g, c.b, c.a};
        std::memcpy(text + colourAt, f, sizeof f);
    }
    ResendFont(text);
}

void TextShadow(Obj text, double dx, double dy, const Color& c) {
    if (!text) return;
    eng::Call(text, "SetShadowOffset", w::Vec2{dx, dy});
    eng::Call(text, "SetShadowColorAndOpacity", c);
}

Color Hex(const char* hex, float alpha) {
    if (!hex) return {0, 0, 0, alpha};
    if (*hex == '#') ++hex;
    const unsigned long v = std::strtoul(hex, nullptr, 16);
    auto lin = [](unsigned long c) { return static_cast<float>(std::pow(static_cast<double>(c) / 255.0, 2.2)); };
    return {lin((v >> 16) & 255), lin((v >> 8) & 255), lin(v & 255), alpha};
}

Color Mix(const Color& a, const Color& b, double t) {
    const float f = static_cast<float>(std::clamp(t, 0.0, 1.0));
    return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f};
}

double Smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

double Approach(double value, double target, double dt, double tau, double finish) {
    if (tau <= 0 || dt <= 0) return target;
    const double k = 1 - std::exp(-dt / tau);
    double step = (target - value) * k;
    const double least = finish * dt;               // the tail ends at this speed instead of slowing for ever
    if (std::fabs(step) < least) step = target > value ? least : -least;
    const double next = value + step;
    return (target - value) * (target - next) <= 0 || std::fabs(next - target) < 0.001 ? target : next;
}

// The approved preview's wiggle (Slot tabs v4), 30 ms apart: like the game's Anim_TextBounce on its tab buttons.
double Wiggle(double seconds) {
    static const double kAngles[] = {0, 5.19, 6.67, 4.24, 0, -3.30, -4.00, -2.36, 0, 1.41, 1.33, 0.47, 0};
    constexpr int n = sizeof kAngles / sizeof kAngles[0];
    const double at = seconds / 0.030;
    if (at <= 0 || at >= n - 1) return 0;
    const int i = static_cast<int>(at);
    return kAngles[i] + (kAngles[i + 1] - kAngles[i]) * (at - i);
}

Rect Bounds(Obj widget) {
    Rect r;
    Obj controller = game::PlayerController(), lib = Lib("SlateBlueprintLibrary");
    if (!widget || !controller || !lib) return r;
    const eng::Params geometry = eng::Call(widget, "GetCachedGeometry");
    size_t size = 0;
    const uint8_t* g = geometry.Return(&size);
    if (!g || !size) return r;
    const std::vector<uint8_t> bytes(g, g + size);
    eng::Params local(eng::FunctionOn(lib, "GetLocalSize"));
    local.SetArg(0, bytes.data(), bytes.size());
    eng::Invoke(lib, local);
    const w::Vec2 extent = local.ReturnAs<w::Vec2>(w::Vec2{0, 0});
    auto toViewport = [&](w::Vec2 p, bool* ok) {
        eng::Params q(eng::FunctionOn(lib, "LocalToViewport"));
        q.Set("WorldContextObject", controller);
        q.Set("Geometry", bytes.data(), bytes.size());
        q.Set("LocalCoordinate", p);
        w::Vec2 out{0, 0};
        *ok = eng::Invoke(lib, q);
        if (const uint8_t* v = q.Get("ViewportPosition")) std::memcpy(&out, v, sizeof out);
        return out;
    };
    bool okA = false, okB = false;
    const w::Vec2 a = toViewport({0, 0}, &okA), b = toViewport(extent, &okB);
    r.x = a.x;
    r.y = a.y;
    r.w = b.x - a.x;
    r.h = b.y - a.y;
    r.ok = okA && okB && extent.x > 0 && extent.y > 0;
    return r;
}

bool ToLocal(Obj widget, double* x, double* y) {
    Obj controller = game::PlayerController(), lib = Lib("SlateBlueprintLibrary");
    if (!widget || !controller || !lib) return false;
    const eng::Params geometry = eng::Call(widget, "GetCachedGeometry");
    size_t size = 0;
    const uint8_t* g = geometry.Return(&size);
    if (!g || !size) return false;
    const std::vector<uint8_t> bytes(g, g + size);
    eng::Params local(eng::FunctionOn(lib, "GetLocalSize"));
    local.SetArg(0, bytes.data(), bytes.size());
    eng::Invoke(lib, local);
    const w::Vec2 extent = local.ReturnAs<w::Vec2>(w::Vec2{0, 0});
    if (extent.x <= 0 || extent.y <= 0) return false;
    auto toViewport = [&](w::Vec2 p, bool* ok) {
        eng::Params q(eng::FunctionOn(lib, "LocalToViewport"));
        q.Set("WorldContextObject", controller);
        q.Set("Geometry", bytes.data(), bytes.size());
        q.Set("LocalCoordinate", p);
        w::Vec2 out{0, 0};
        *ok = eng::Invoke(lib, q);
        if (const uint8_t* v = q.Get("ViewportPosition")) std::memcpy(&out, v, sizeof out);
        return out;
    };
    bool okA = false, okB = false;
    const w::Vec2 a = toViewport({0, 0}, &okA), b = toViewport(extent, &okB);
    if (!okA || !okB || std::fabs(b.x - a.x) < 1 || std::fabs(b.y - a.y) < 1) return false;
    *x = (*x - a.x) * extent.x / (b.x - a.x);
    *y = (*y - a.y) * extent.y / (b.y - a.y);
    return true;
}

bool Mouse(double* x, double* y) { return game::MousePosition(x, y); }
bool ScreenSize(double* w, double* h) { return game::ScreenSize(w, h); }

Obj Font() {
    static eng::Weak font;
    static double triedAt = -100;
    Obj f = eng::Get(font);
    if (!f && game::Seconds() - triedAt > 5) {
        triedAt = game::Seconds();
        if ((f = cosmetics::LoadAsset(kFont))) {
            cosmetics::KeepAlive(f);
            font = eng::MakeWeak(f);
        } else {
            hostlog::Warn("ux: the game's font did not load; texts use the default font");
        }
    }
    return f;
}

Obj BubbleMaterial() {
    static eng::Weak material;
    static double triedAt = -100;
    Obj m = eng::Get(material);
    if (!m && game::Seconds() - triedAt > 5) {
        triedAt = game::Seconds();
        if ((m = cosmetics::LoadAsset(kBubble))) {
            cosmetics::KeepAlive(m);
            material = eng::MakeWeak(m);
        } else {
            hostlog::Warn("ux: M_UI_BubbleButton did not load; hovers have no bubbles");
        }
    }
    return m;
}

namespace {
struct Name8 {
    uint8_t bytes[8];
};
Name8 NameOf(const char* s) {
    const std::wstring wide = eng::Widen(s);
    Name8 n{};
    const eng::Params made = eng::Call(Lib("KismetStringLibrary"), "Conv_StringToName",
                                       eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
    size_t size = 0;
    if (const uint8_t* r = made.Return(&size); r && size == 8) std::memcpy(n.bytes, r, 8);
    return n;
}
void SetColorParam(Obj material, const char* parameter, const Color& c) {
    eng::Params p(eng::FunctionOn(material, "SetVectorParameterValue"));
    const Name8 name = NameOf(parameter);
    const float value[4] = {c.r, c.g, c.b, c.a};
    p.Set("ParameterName", name);
    p.Set("Value", value);
    eng::Invoke(material, p);
}
}  // namespace

// M_UI_BubbleButton's colours (FModel): Liquid Color 1 ACE800, Liquid Color 2 BFFF00, BG Color BFFF00, Bubbles additive
// color F9F9F9, HoveredColor FAF9F6. Quiet: the liquid grey, the bubbles as they are.
namespace {
// A copy of the bubble in other colours (liquid 1 / 2, background, the bubbles), its bubbles at a quarter speed.
Obj RecolouredBubble(eng::Weak& material, double& triedAt, const char* what, const char* liquid1, const char* liquid2, const char* bg,
                     const char* bubbles, const Color* bubblesColour = nullptr) {
    if (Obj m = eng::Get(material)) return m;
    if (game::Seconds() - triedAt < 5) return BubbleMaterial();
    triedAt = game::Seconds();
    Obj parent = BubbleMaterial();
    if (!parent) return nullptr;
    Obj mid = eng::Call(Lib("KismetMaterialLibrary"), "CreateDynamicMaterialInstance", game::PlayerController(), parent, Name8{}, uint8_t{0})
                  .ReturnObj();
    if (!mid) {
        hostlog::Warn(std::string("ux: the ") + what + " bubble could not be made; hovers use the lime one");
        return parent;
    }
    SetColorParam(mid, "Liquid Color 1", Hex(liquid1));
    SetColorParam(mid, "Liquid Color 2", Hex(liquid2));
    SetColorParam(mid, "BG Color", Hex(bg));
    SetColorParam(mid, "Bubbles additive color", bubblesColour ? *bubblesColour : Hex(bubbles));
    // slower: a quarter of the bubbles' speed (Lvcid: their swirl was too fast)
    for (const auto& [parameter, value] : {std::pair<const char*, float>{"Bubble min speed", 0.0025f}, {"Bubble max speed", 0.025f}}) {
        eng::Params p(eng::FunctionOn(mid, "SetScalarParameterValue"));
        const Name8 name = NameOf(parameter);
        p.Set("ParameterName", name);
        p.Set("Value", value);
        eng::Invoke(mid, p);
    }
    cosmetics::KeepAlive(mid);
    material = eng::MakeWeak(mid);
    return mid;
}
}  // namespace

Obj QuietBubbleMaterial() {
    static eng::Weak material;
    static double triedAt = -100;
    // (bubbles darker than they were, #B4B7BC: they merged with the white words over them; Lvcid)
    return RecolouredBubble(material, triedAt, "quiet", "#1E2024", "#24262B", "#24262B", "#6E7177");
}

// Ballest Night's top bar hovers (Lvcid, 2026-10-11): a black liquid, its bubbles light enough to see.
Obj BlackBubbleMaterial() {
    static eng::Weak material;
    static double triedAt = -100;
    return RecolouredBubble(material, triedAt, "black", "#000000", "#08090A", "#08090A", "#8A8D93");
}

// Ballest Light's (Lvcid): a white liquid, its bubbles a soft grey (additive: brighter than the liquid's slightly darker
// shade, so they show).
Obj WhiteBubbleMaterial() {
    static eng::Weak material;
    static double triedAt = -100;
    return RecolouredBubble(material, triedAt, "white", "#D9D9DB", "#E6E6E8", "#E6E6E8", "#3A3A3C");
}

// White with dark bubbles (Ballest Light's settings menu, Lvcid): the bubbles are added to the liquid, so a negative
// colour darkens it where they are.
Obj InkBubbleMaterial() {
    static eng::Weak material;
    static double triedAt = -100;
    static const Color dark{-0.55f, -0.55f, -0.55f, 1};
    return RecolouredBubble(material, triedAt, "ink", "#F2F2F3", "#FAFAFB", "#FAFAFB", "#000000", &dark);
}

Obj LoadMaterial(const wchar_t* path) {
    static std::map<std::wstring, eng::Weak> kept;
    static std::map<std::wstring, double> triedAt;
    eng::Weak& w = kept[path];
    if (Obj m = eng::Get(w)) return m;
    if (game::Seconds() - triedAt[path] < 5) return nullptr;
    triedAt[path] = game::Seconds();
    Obj m = cosmetics::LoadAsset(path);
    if (!m) {
        hostlog::Warn("ux: a material did not load: " + eng::Narrow(path, static_cast<int>(std::wcslen(path))));
        return nullptr;
    }
    cosmetics::KeepAlive(m);
    w = eng::MakeWeak(m);
    return m;
}

Obj HalftoneMaterial(float boxScale, bool dark) {
    // (one per variant and box scale)
    struct Made {
        eng::Weak material;
        double triedAt = -100;
    };
    static std::map<std::pair<bool, int>, Made> made;
    Made& one = made[{dark, static_cast<int>(std::lround(boxScale * 1000))}];
    eng::Weak& material = one.material;
    if (Obj m = eng::Get(material)) return m;
    if (game::Seconds() - one.triedAt < 5) return nullptr;
    one.triedAt = game::Seconds();
    Obj parent = LoadMaterial(dark ? L"/Game/UI/UIMaterialLab/Materials/6_Patterns/4_Halftone/MI_Halftone.MI_Halftone"
                                   : L"/Game/UI/UIMaterialLab/Materials/6_Patterns/4_Halftone/MI_Halftone_White.MI_Halftone_White");
    if (!parent) return nullptr;
    Obj mid = eng::Call(Lib("KismetMaterialLibrary"), "CreateDynamicMaterialInstance", game::PlayerController(), parent, Name8{}, uint8_t{0})
                  .ReturnObj();
    if (!mid) {
        material = eng::MakeWeak(parent);     // (the plain one kept: returning it once, then nothing for 5 s, flickered)
        return parent;
    }
    eng::Params p(eng::FunctionOn(mid, "SetScalarParameterValue"));
    const Name8 name = NameOf("Bg box scale");
    p.Set("ParameterName", name);
    p.Set("Value", boxScale);
    eng::Invoke(mid, p);
    if (dark) {
        // (its box the plugin manager's own window colour, not the game's lighter grey: only the dots show, its edges
        // melt into the window: Lvcid)
        const Color c = Hex("#0D0E11");
        eng::Params v(eng::FunctionOn(mid, "SetVectorParameterValue"));
        const Name8 colour = NameOf("Bg box color");
        const float value[4] = {c.r, c.g, c.b, 1};
        v.Set("ParameterName", colour);
        v.Set("Value", value);
        eng::Invoke(mid, v);
    }
    cosmetics::KeepAlive(mid);
    material = eng::MakeWeak(mid);
    return mid;
}

// --- a theme's colour map ------------------------------------------------------------------------------------------------
namespace {
ThemeMap gThemeMap = nullptr;
int gRawDepth = 0;
}  // namespace
void SetThemeMap(ThemeMap map) { gThemeMap = map; }
namespace {
double gOutlineFade = 1;
}
void SetOutlineFade(double fade) { gOutlineFade = std::clamp(fade, 0.0, 1.0); }
bool ThemeMapped() { return gThemeMap && gRawDepth == 0; }
Color Themed(const Color& c, int kind) { return ThemeMapped() ? gThemeMap(c, kind) : c; }
RawColors::RawColors() { ++gRawDepth; }
RawColors::~RawColors() { --gRawDepth; }

namespace {
struct Embedded {
    const char* name;
    const unsigned char* data;
    size_t size;
};
#include "pictures.inc"
}  // namespace

Obj Picture(const char* name) {
    for (const Embedded& e : kPictures) {
        if (std::strcmp(e.name, name) != 0) continue;
        const std::wstring dir = hostlog::DataDir() + L"\\cache\\ui";
        const std::wstring file = dir + L"\\" + eng::Widen(name);
        static std::map<std::string, bool> written;     // checked once a session
        if (written[name]) return LoadPicture(file);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        const bool same = GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeHigh == 0 &&
                          info.nFileSizeLow == e.size;
        if (!same) {
            CreateDirectoryW((hostlog::DataDir() + L"\\cache").c_str(), nullptr);
            CreateDirectoryW(dir.c_str(), nullptr);
            HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                hostlog::Warn(std::string("ux: could not write the picture ") + name);
                return nullptr;
            }
            DWORD bytes = 0;
            WriteFile(h, e.data, static_cast<DWORD>(e.size), &bytes, nullptr);
            CloseHandle(h);
        }
        written[name] = true;
        return LoadPicture(file);
    }
    hostlog::Warn(std::string("ux: no built-in picture ") + name);
    return nullptr;
}

Obj LoadPicture(const std::wstring& file) {
    static std::map<std::wstring, eng::Weak> cache;
    // (a file that failed: tried again once it's there or has changed (a download finishing: Lvcid, an icon showing
    // only after its card was clicked), else after a while; a file that exists but won't load waits longer)
    struct Failed {
        double at = 0;
        bool missing = false;
        uint64_t stamp = 0;                 // its size and write time when it failed
        double checked = -100;              // when its file was last looked at
    };
    static std::map<std::wstring, Failed> failed;
    if (file.empty()) return nullptr;
    if (Obj t = eng::Get(cache[file])) return t;
    auto stampOf = [&](bool* exists) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        *exists = GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a) != 0;
        return *exists ? (static_cast<uint64_t>(a.nFileSizeLow) ^ (static_cast<uint64_t>(a.ftLastWriteTime.dwLowDateTime) << 20)) : 0;
    };
    if (const auto it = failed.find(file); it != failed.end()) {
        // (its file looked at four times a second at most, not on every draw: a disk call per broken icon per frame)
        if (game::Seconds() - it->second.checked < 0.25) return nullptr;
        it->second.checked = game::Seconds();
        bool exists = false;
        const uint64_t stamp = stampOf(&exists);
        const bool changed = exists && (it->second.missing || stamp != it->second.stamp);
        // (checked at most four times a second: a missing file isn't asked for every frame)
        if (!changed && game::Seconds() - it->second.at < (it->second.missing ? 0.25 : 30)) return nullptr;
        if (!changed && it->second.missing) {
            it->second.at = game::Seconds();
            return nullptr;
        }
    }
    Obj t = cosmetics::LoadTexture(file);
    if (!t) {
        bool exists = false;
        const uint64_t stamp = stampOf(&exists);
        if (!failed.count(file))
            hostlog::Warn("ux: picture " + eng::Narrow(file.c_str(), static_cast<int>(file.size())) + " did not load" +
                          (!exists ? " (no such file yet: it's tried again once it's there)" : ""));
        failed[file] = {game::Seconds(), !exists, stamp};
        return nullptr;
    }
    failed.erase(file);
    cache[file] = eng::MakeWeak(t);
    return t;
}

// --- screens ---------------------------------------------------------------------------------------------------------
bool Screen::Ready() const { return eng::Get(host) && eng::Get(canvas) && generation == game::Generation(); }

bool Screen::Make() {
    Remove();
    Obj controller = game::PlayerController();
    Obj h = nullptr, t = nullptr, c = nullptr;
    if (!controller || !w::NewScreen(controller, &h, &t, &c)) return false;
    w::SetVisibility(h, w::kSelfHitTestInvisible);
    w::SetVisibility(c, w::kSelfHitTestInvisible);
    eng::Call(h, "AddToViewport", static_cast<int32_t>(zOrder));
    host = eng::MakeWeak(h);
    tree = eng::MakeWeak(t);
    canvas = eng::MakeWeak(c);
    generation = game::Generation();
    shown = true;
    return true;
}

void Screen::Show(bool on) {
    Obj h = eng::Get(host);
    if (!h || on == shown) return;
    shown = on;
    w::SetVisibility(h, on ? w::kSelfHitTestInvisible : w::kCollapsed);
}

void Screen::Scale(double k) {
    Obj c = eng::Get(canvas);
    if (!c || std::fabs(k - scale) < 0.001) return;
    scale = k;
    eng::Call(c, "SetRenderTransformPivot", w::Vec2{0.5, 0.5});
    eng::Call(c, "SetRenderScale", w::Vec2{k, k});
}

void Screen::Remove() {
    if (Obj h = eng::Get(host)) eng::Call(h, "RemoveFromParent");
    host = tree = canvas = {};
}

// --- nodes -----------------------------------------------------------------------------------------------------------
// Widget units onto whole pixels of the screen (its DPI scale: 4/3 at 2560x1440), so edges and texts land on the
// pixel grid and don't shimmer as they move by fractions of a pixel.
double Snap(double v) {
    static double scale = 1, readAt = -100;
    if (game::Seconds() - readAt > 1) {
        readAt = game::Seconds();
        if (Obj controller = game::PlayerController()) {
            const float s = eng::Call(eng::FindCdo("WidgetLayoutLibrary"), "GetViewportScale", controller).ReturnAs<float>(0.0f);
            if (s > 0.1f) scale = s;
        }
    }
    return std::round(v * scale) / scale;
}

void Node::Box(double nx, double ny, double nw, double nh) {
    Obj s = eng::Get(slot);
    if (!s) return;
    const double right = Snap(nx + nw), bottom = Snap(ny + nh);
    nx = Snap(nx);
    ny = Snap(ny);
    nw = right - nx;
    nh = bottom - ny;
    if (std::fabs(nx - x) > 0.01 || std::fabs(ny - y) > 0.01) {
        x = nx;
        y = ny;
        eng::Call(s, "SetPosition", w::Vec2{nx, ny});
    }
    if (std::fabs(nw - w) > 0.01 || std::fabs(nh - h) > 0.01) {
        w = nw;
        h = nh;
        eng::Call(s, "SetSize", w::Vec2{std::max(0.0, nw), std::max(0.0, nh)});
    }
}

void Node::Center(double cx, double cy) {
    cx = Snap(cx);
    cy = Snap(cy);
    Obj s = eng::Get(slot);
    if (!s || (std::fabs(cx - x) <= 0.01 && std::fabs(cy - y) <= 0.01)) return;
    x = cx;
    y = cy;
    eng::Call(s, "SetPosition", w::Vec2{cx, cy});
}

void Node::Transform(double s, double a) {
    Obj o = Get();
    if (!o) return;
    if (std::fabs(s - scale) > 0.001) {
        scale = s;
        eng::Call(o, "SetRenderScale", w::Vec2{s, s});
    }
    if (std::fabs(a - angle) > 0.01) {
        angle = a;
        eng::Call(o, "SetRenderTransformAngle", static_cast<float>(a));
    }
}

void Node::Opacity(float o) {
    Obj widget_ = Get();
    if (!widget_ || std::fabs(o - opacity) < 0.003f) return;
    opacity = o;
    eng::Call(widget_, "SetRenderOpacity", o);
}

void Node::Tint(const Color& c) {
    Obj o = Get();
    if (!o || (std::fabs(c.r - color.r) < 0.002f && std::fabs(c.g - color.g) < 0.002f && std::fabs(c.b - color.b) < 0.002f &&
               std::fabs(c.a - color.a) < 0.003f))
        return;
    color = c;
    if (type == kBorder) eng::Call(o, "SetBrushColor", c);
    else if (type == kImage) eng::Call(o, "SetColorAndOpacity", c);
    else if (type == kText) w::SetTextColor(o, c);
}

void Node::Outline(float width, const Color& given) {
    Color c = Themed(given, 3);
    c.a = static_cast<float>(c.a * gOutlineFade);
    Obj o = Get();
    if (!o || (type != kBorder && type != kImage)) return;
    if (std::fabs(width - outline) < 0.02f && std::fabs(c.r - outlineColor.r) < 0.003f && std::fabs(c.g - outlineColor.g) < 0.003f &&
        std::fabs(c.b - outlineColor.b) < 0.003f && std::fabs(c.a - outlineColor.a) < 0.005f)
        return;
    outline = width;
    outlineColor = c;
    OutlineBrush(o, type == kBorder ? "Background" : "Brush", width, c);
    // The brush is read when the widget paints; setting its colour again makes it paint now.
    const Color now = color.r < 0 ? Color{1, 1, 1, 1} : color;
    if (type == kBorder) eng::Call(o, "SetBrushColor", now);
    else eng::Call(o, "SetColorAndOpacity", now);
}

// (corners or order changed after the node was made (a row's view switched, a key growing, another theme's z): set in
// place; before, the shape and order it was first drawn with stayed for good)
void Node::Round(double tl, double tr, double br, double bl) {
    Obj o = Get();
    if (!o || (type != kBorder && type != kImage)) return;
    const double want[4] = {tl, tr, br, bl};
    bool same = true;
    for (int i = 0; i < 4; ++i) same = same && std::fabs(corners[i] - want[i]) < 0.25;
    if (same) return;
    const bool first = corners[0] < 0;
    for (int i = 0; i < 4; ++i) corners[i] = want[i];
    if (first) return;                                       // (made with these: nothing to change)
    RoundBrush(o, type == kBorder ? "Background" : "Brush", tl, tr, br, bl);
    const Color now = color.r < 0 ? Color{1, 1, 1, 1} : color;      // (as Outline: setting the colour repaints it)
    if (type == kBorder) eng::Call(o, "SetBrushColor", now);
    else eng::Call(o, "SetColorAndOpacity", now);
}

void Node::Z(int order) {
    if (order == z) return;
    const bool first = z == -1000000;
    z = order;
    if (first) return;                                       // (made at it)
    if (Obj s = eng::Get(slot)) eng::Call(s, "SetZOrder", static_cast<int32_t>(order));
}

void Node::Show(bool on) {
    Obj o = Get();
    if (!o || on == visible) return;
    visible = on;
    w::SetVisibility(o, on ? (type == kButton ? uint8_t{0} : w::kHitTestInvisible) : w::kCollapsed);
}

void Node::Picture(Obj texture) {
    Obj o = Get();
    if (!o || type != kImage) return;
    if (texture && eng::Get(shownPicture) == texture) return;
    if (!texture && hasPicture == 0) return;
    shownPicture = eng::MakeWeak(texture);
    hasPicture = texture ? 1 : 0;
    // No picture draws nothing (an Image without one is a white box).
    if (texture) eng::Call(o, "SetBrushResourceObject", texture);
    eng::Call(o, "SetRenderOpacity", texture ? opacity : 0.0f);
}

void Node::Text(const std::string& s) {
    Obj o = Get();
    if (!o || s == shownText) return;
    shownText = s;
    w::SetText(o, s);
}

double Node::TextWidth() const {
    Obj o = Get();
    return o ? eng::Call(o, "GetDesiredSize").ReturnAs<w::Vec2>(w::Vec2{0, 0}).x : 0;
}

// --- making nodes ----------------------------------------------------------------------------------------------------
void RoundBrush(Obj widget, const char* brush, double tl, double tr, double br, double bl) {
    if (!widget) return;
    Obj cls = eng::ClassOf(widget);
    eng::Prop drawAs, radii, rounding;
    const int drawAt = eng::NestedOffset(cls, {brush, "DrawAs"}, &drawAs);
    const int radiiAt = eng::NestedOffset(cls, {brush, "OutlineSettings", "CornerRadii"}, &radii);
    const int roundingAt = eng::NestedOffset(cls, {brush, "OutlineSettings", "RoundingType"}, &rounding);
    if (drawAt < 0 || radiiAt < 0 || drawAs.size != 1 || (radii.size != 32 && radii.size != 16)) {
        static bool logged = false;
        if (!logged) hostlog::Warn("ux: a brush is not laid out as measured; boxes stay square");
        logged = true;
        return;
    }
    widget[drawAt] = 4;                             // ESlateBrushDrawType::RoundedBox
    // FVector4 CornerRadii: top left, top right, bottom right, bottom left (doubles on this engine, floats on older)
    if (radii.size == 32) {
        const double r[4] = {tl, tr, br, bl};
        std::memcpy(widget + radiiAt, r, sizeof r);
    } else {
        const float r[4] = {float(tl), float(tr), float(br), float(bl)};
        std::memcpy(widget + radiiAt, r, sizeof r);
    }
    if (roundingAt >= 0 && rounding.size == 1) widget[roundingAt] = 0;     // FixedRadius
}

void OutlineBrush(Obj widget, const char* brush, float width, const Color& c) {
    if (!widget) return;
    Obj cls = eng::ClassOf(widget);
    eng::Prop wp;
    const int widthAt = eng::NestedOffset(cls, {brush, "OutlineSettings", "Width"}, &wp);
    if (widthAt >= 0 && wp.size == sizeof width) std::memcpy(widget + widthAt, &width, sizeof width);
    w::WriteSlateColor(widget, {brush, "OutlineSettings", "Color"}, c);
}

Node MakeBox(Obj tree, Obj canvas, const Color& fill, double tl, double tr, double br, double bl, int z) {
    Node n;
    n.type = Node::kBorder;
    Obj border = w::Spawn("Border", tree);
    if (!border) return n;
    RoundBrush(border, "Background", tl, tr, br, bl);
    eng::Call(border, "SetPadding", w::Margin{0, 0, 0, 0});
    eng::Call(border, "SetBrushColor", fill);
    n.color = fill;
    w::SetVisibility(border, w::kHitTestInvisible);
    n.slot = eng::MakeWeak(OnCanvas(canvas, border, z, false, false));
    n.widget = eng::MakeWeak(border);
    return n;
}

Node MakePicture(Obj tree, Obj canvas, double radius, int z) {
    Node n;
    n.type = Node::kImage;
    Obj image = w::Spawn("Image", tree);
    if (!image) return n;
    RoundBrush(image, "Brush", radius, radius, radius, radius);
    w::SetVisibility(image, w::kHitTestInvisible);
    n.slot = eng::MakeWeak(OnCanvas(canvas, image, z, false, false));
    n.widget = eng::MakeWeak(image);
    n.color = {1, 1, 1, 1};
    return n;
}

Node MakeBubble(Obj tree, Obj canvas, double tl, double tr, double br, double bl, int z, int look) {
    Node n;
    n.type = Node::kImage;
    Obj image = w::Spawn("Image", tree);
    if (!image) return n;
    if (Obj material = look == 4 ? InkBubbleMaterial() : look == 3 ? WhiteBubbleMaterial() : look == 2 ? BlackBubbleMaterial()
                       : look == 1 ? QuietBubbleMaterial() : BubbleMaterial())
        eng::Call(image, "SetBrushFromMaterial", material);
    else eng::Call(image, "SetColorAndOpacity", Hex("#BFFF00"));
    RoundBrush(image, "Brush", tl, tr, br, bl);
    eng::Call(image, "SetColorAndOpacity", Color{1, 1, 1, 0});
    n.color = {1, 1, 1, 0};
    w::SetVisibility(image, w::kHitTestInvisible);
    n.slot = eng::MakeWeak(OnCanvas(canvas, image, z, false, false));
    n.widget = eng::MakeWeak(image);
    return n;
}

Node MakeText(Obj tree, Obj canvas, const std::string& s, double px, const Color& c, int z, const char* face, double alignX) {
    Node n;
    n.type = Node::kText;
    n.centered = true;
    Obj text = w::Spawn("TextBlock", tree);
    if (!text) return n;
    SetFace(text, px, face);
    w::SetTextColor(text, c);
    n.color = c;
    w::SetText(text, s);
    n.shownText = s;
    eng::Call(text, "SetJustification", static_cast<uint8_t>(alignX < 0.25 ? 0 : alignX > 0.75 ? 2 : 1));
    eng::Call(text, "SetRenderTransformPivot", w::Vec2{0.5, 0.5});
    w::SetVisibility(text, w::kHitTestInvisible);
    n.slot = eng::MakeWeak(OnCanvas(canvas, text, z, true, true, alignX));
    n.widget = eng::MakeWeak(text);
    return n;
}

Node MakeHit(Obj tree, Obj canvas, int z) {
    Node n;
    n.type = Node::kButton;
    Obj button = w::Spawn("Button", tree);
    if (!button) return n;
    w::Transparent(button);
    w::Unfocusable(button);
    n.slot = eng::MakeWeak(OnCanvas(canvas, button, z, false, false));
    n.widget = eng::MakeWeak(button);
    return n;
}

Node MakeMaterialImage(Obj tree, Obj canvas, Obj material, double radius, int z) {
    Node n;
    n.type = Node::kImage;
    Obj image = w::Spawn("Image", tree);
    if (!image) return n;
    if (material) eng::Call(image, "SetBrushFromMaterial", material);
    RoundBrush(image, "Brush", radius, radius, radius, radius);
    w::SetVisibility(image, w::kHitTestInvisible);
    n.slot = eng::MakeWeak(OnCanvas(canvas, image, z, false, false));
    n.widget = eng::MakeWeak(image);
    n.color = {1, 1, 1, 1};
    return n;
}

Node Place(Obj canvas, Obj widget, int z, Node::Type type) {
    Node n;
    n.type = type;
    if (!widget) return n;
    n.slot = eng::MakeWeak(OnCanvas(canvas, widget, z, false, false));
    n.widget = eng::MakeWeak(widget);
    return n;
}

bool Click::Update(const Node& hit) {
    Obj button = hit.Get();
    if (!button || !hit.visible) {
        wasPressed = hovered = false;
        return false;
    }
    const bool pressed = eng::Call(button, "IsPressed").ReturnBool();
    hovered = eng::Call(button, "IsHovered").ReturnBool();
    const bool clicked = wasPressed && !pressed && hovered;
    wasPressed = pressed;
    return clicked;
}

// --- plain pieces ----------------------------------------------------------------------------------------------------
Obj Spawn(const char* cls, Obj tree) { return w::Spawn(cls, tree); }

Obj TextBlock(Obj tree, const std::string& s, double px, const Color& c, const char* face) {
    Obj text = w::Spawn("TextBlock", tree);
    if (!text) return nullptr;
    SetFace(text, px, face);
    w::SetTextColor(text, c);
    w::SetText(text, s);
    return text;
}

Obj Rounded(Obj tree, const Color& fill, double radius, w::Margin padding) {
    Obj border = w::Spawn("Border", tree);
    if (!border) return nullptr;
    if (radius > 0) RoundBrush(border, "Background", radius, radius, radius, radius);
    eng::Call(border, "SetBrushColor", fill);
    eng::Call(border, "SetPadding", padding);
    return border;
}

Obj Sized(Obj tree, Obj child, double width, double height) {
    Obj box = w::Spawn("SizeBox", tree);
    if (!box || !child) return nullptr;
    if (width > 0) eng::Call(box, "SetWidthOverride", static_cast<float>(width));
    if (height > 0) eng::Call(box, "SetHeightOverride", static_cast<float>(height));
    w::AddChild(box, child);
    return box;
}

void InBox(Obj vbox, Obj child, w::Margin padding, uint8_t alignX) {
    Obj slot = child && vbox ? eng::Call(vbox, "AddChildToVerticalBox", child).ReturnObj() : nullptr;
    if (!slot) return;
    eng::Call(slot, "SetPadding", padding);
    eng::Call(slot, "SetHorizontalAlignment", alignX);
}

void InRow(Obj row, Obj child, double left, bool fill, uint8_t alignY) {
    Obj slot = child && row ? eng::Call(row, "AddChildToHorizontalBox", child).ReturnObj() : nullptr;
    if (!slot) return;
    eng::Call(slot, "SetPadding", w::Margin{static_cast<float>(left), 0, 0, 0});
    eng::Call(slot, "SetVerticalAlignment", alignY);
    if (fill) w::FillSlot(slot);
}

}  // namespace ux
