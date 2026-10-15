#include "manager.hpp"
#include "race.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <angelscript.h>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "browser.hpp"
#include "cosmetics.hpp"
#include "draw.hpp"
#include "game.hpp"
#include "hoverfx.hpp"
#include "input.hpp"
#include "log.hpp"
#include "packages.hpp"
#include "plugins.hpp"
#include "pmui.hpp"
#include "registry.hpp"
#include "settings.hpp"
#include "storage.hpp"
#include "testchannel.hpp"
#include "ui.hpp"
#include "ux.hpp"
#include "widgets.hpp"

using eng::Obj;
using pmui::Layer;
using pmui::Rect;
using ui::Color;
namespace w = ui::widgets;

namespace manager {
namespace {

// --- the look (the concept's colours, sRGB; ux::Hex makes them linear) -----------------------------------------------
const Color kBlack = ux::Hex("#000000"), kWhite = ux::Hex("#FFFFFF");
// The limes follow the theme (ThemeLimes, each frame): Lucid Lime's own below; Ballest Night uses the game's lime
// line (sampled off its UI, #B3F000) for all three (Lvcid, 2026-10-11).
const Color kLucidLime = ux::Hex("#BEFF00"), kLucidNuLime = ux::Hex("#96E200"), kLucidTopLime = ux::Hex("#97E300");
const Color kNightLime = ux::Hex("#B3F000");
Color kLime = kLucidLime;
// Lvcid's names (2026-10-10): "nulime" = the logo's lit square (#96E200): sliders, the console key, the scroll bars,
// the disk bars, the logo's hover ring; "lime" / "off-lime" = the bubble button's (and the resize edges, a box's
// inner scroll bar). kTopLime: the top row's open cog and page.
Color kNuLime = kLucidNuLime, kTopLime = kLucidTopLime;
const Color kPink = ux::Hex("#F82884");      // ballest's neon pink: the NEW tags (Lvcid)
// A plugin with an error / a warning: Ballest Night uses ballest's lipstick red and bright amber for its row's lit
// edge, glow and smoke, its toggle and its pill (the red dots and the messages' words keep theirs).
// (Lucid Lime keeps its own red and gold for now: kRed / kErr and kGold, Lvcid 2026-10-11)
const Color kErrAccent = ux::Hex("#F80042"), kWarnAccent = ux::Hex("#E9C51D");
bool BallestNight();                        // Ballest Night, or Ballest Light (the same, light: BallestLight)
bool BallestLight();
bool gNightBand = false;                    // (Ballest Light's dark band under the top bar: drawn as Night)
const Color& ErrAccent();                   // the error's colour in this theme (lit edge, toggle)
const Color& WarnAccent();                  // the warning's
// The game's colours (Lvcid's names: ballest blue #005EC7, ballest cyan #09CEEB, the lighter one; gold, red, pink).
const Color kBallestCyan = ux::Hex("#09CEEB"), kBallestGold = ux::Hex("#E9C51D"), kBallestBlue = ux::Hex("#005EC7");
// "Ballest Night" (Lvcid, 2026-10-11): Lucid Lime with the game's colours in a few places: the open tab of the strip
// pink with white words over a black drop shadow (as the game's leaderboard), the version gold, "what's new" cyan.
bool BallestNight();
int CurrentTheme();                         // 0 Ballest Night, 1 Ballest Light, 2 Lucid Lime, 3 Classic
// Over everything, unclipped (the screen's own units): hover effects that hang out past their button and the window's
// edge (the settings nav's glow and smoke). Pictures only: nothing on it takes the pointer.
Layer* gFx = nullptr;
// The black hover bubble against its thin white edge (Lvcid, 2026-10-11, a try): a little bigger than the edge, under
// it, so its moving lines run on out past the edge too, was tried: Lvcid liked it inside better (-3).
constexpr double kBubbleOut = -3;
const Color kKeyLit = ux::Hex("#16171A");     // the top bar's round keys' inside, hovered: a shade over black (Lvcid)
const Color kWindow = ux::Hex("#0D0E11", 0.95f), kTabOff = ux::Hex("#4A4D52"), kTabHover = ux::Hex("#B9BCC1");
const Color kRing = ux::Hex("#1E1E1E"), kField = ux::Hex("#1A1B1F"), kFieldEdge = ux::Hex("#232428"), kHint = ux::Hex("#5E6166");
const Color kRule = ux::Hex("#23252A"), kStrip = ux::Hex("#1E1E1E"), kSegText = ux::Hex("#5C5F63"), kChip = ux::Hex("#1E1F23");
const Color kSub = ux::Hex("#8D9096"), kVer = ux::Hex("#6E7176"), kCard = ux::Hex("#18191C"), kCardEdge = ux::Hex("#1F2024");
const Color kOffRow = ux::Hex("#141517"), kOpenEdge = ux::Hex("#2E3036"), kTile = ux::Hex("#16171A"), kTileRing = ux::Hex("#2B2D31");
const Color kAb = ux::Hex("#101114"), kAbEdge = ux::Hex("#26272C"), kText = ux::Hex("#E8E9EB"), kOffName = ux::Hex("#7C7F84");
const Color kDescOpen = ux::Hex("#B5B8BD"), kRed = ux::Hex("#FF2E4D"), kErr = ux::Hex("#FF736B"), kWarn = ux::Hex("#FFC24A");
const Color kBlue = ux::Hex("#7FC8FF"), kOverlay = ux::Hex("#0B0C0E", 0.985f), kSideEdge = ux::Hex("#1C1D21"), kHead = ux::Hex("#55585D");
const Color kNightNavBg = ux::Hex("#1D1E21");     // Ballest Night's settings menu (shows about #16171A)
const Color kRowLabel = ux::Hex("#D9DADD"), kSetRow = ux::Hex("#1E1F23"), kPopup = ux::Hex("#15161A"), kPopupRing = ux::Hex("#2A2C31");
const Color kDescLine = ux::Hex("#6A6D72"), kDescLineHover = ux::Hex("#7E8187"), kHoverEdge = ux::Hex("#4A4D54");
// A plugin's error: its caution triangle, its glow and its message, a golden orange (Lvcid).
const Color kGold = ux::Hex("#FFA51F");
// An update that failed: its pill a moment.
const Color kRemoveRedGlow = ux::Hex("#E8344E");
// A row's card: its trim a polished light grey; its version muted; the right tab's panel.
// A clickable button's fill in the button panel: shows as #111215 on screen (Lvcid). (Near black the game draws a colour
// about 7 levels darker than its value: the card's #18191C shows as #111215, so that's the value to give.)
const Color kButtonFill = ux::Hex("#18191C");
const Color kOffFill = ux::Hex("#090A0D");   // what can't be clicked: blacked out (Lvcid: the dimmed trash's look)
const Color kCogNone = ux::Hex("#232529");   // a cog with nothing to set: muted more (Lvcid), no hover
const Color kCogOff = ux::Hex("#2E3035");    // a cog with nothing to set, "built in" on compact rows, "up to date": darker (Lvcid)
const Color kTrim = ux::Hex("#2A2C32"), kTrimHover = ux::Hex("#3C3F46"), kVerMuted = ux::Hex("#55585E"), kLinkPanel = ux::Hex("#111215");
const Color kPopupText = ux::Hex("#A9ACB1"), kConsole = ux::Hex("#08090B"), kOffKnob = ux::Hex("#6E7176"), kBannerInk = ux::Hex("#1C2600");

// Sizes in design units (the concept's pixels at 1920x1080, scale 1).
// (the window's first size, until it's dragged: Lvcid's at 1080p, 1184 x 780 (screen units: the same share of any
// screen; a bigger scale's minimum can make it larger); dragged, the player's own size is kept)
constexpr double kWinW = 1184, kWinH = 780, kTopH = 78, kSubH = 74, kGap = 10;
constexpr double kThickH = 98, kCompactH = 58;
constexpr double kEase = 0.06;
constexpr double kRowEase = 0.26;           // a row's or card's hover: slow and soft (Lvcid: a quarter of the speed or less)
constexpr double kGlowMargin = 40.0 / 96, kGlowUnits = 96, kGlowRoom = 24;     // pm-glow.png: a radius-16 box inset 24 of 96
constexpr int kCursorOwner = -77;
const char* const kSelf = "plugin-manager";
const char* const kRepo = "https://github.com/AnythingGoes-ballest/ballest-plugin-manager";
const char* const kReleases = "https://github.com/AnythingGoes-ballest/ballest-plugin-manager/releases";
// A version's own release page (its notes), as the releases are tagged: v0.23.8.
std::string ReleasePage(const std::string& version) { return std::string(kReleases) + "/tag/v" + version; }

// Levels (pmui): the page, then the top bar and overlays over it, then dropdowns, then popups.
constexpr int kPageLevel = 1, kBarLevel = 2, kDropLevel = 3, kPopupLevel = 4;

Color A(const Color& c, double a) { return {c.r, c.g, c.b, static_cast<float>(c.a * std::clamp(a, 0.0, 1.0))}; }
Color Mix(const Color& a, const Color& b, double t) { return ux::Mix(a, b, t); }
double Smooth(double t) { return ux::Smooth(t); }
// A colour lifted a touch towards white (a hovered row's texts and icons: only slightly).
Color Lift(const Color& c, double hover) { return ux::Mix(c, {1, 1, 1, c.a}, 0.09 * hover); }
// Eased out: quick at first, settling slowly.
double EaseOut(double t, double power = 2) { return 1 - std::pow(1 - std::clamp(t, 0.0, 1.0), power); }

// --- the window's own settings (kept in the plugin manager's storage) --------------------------------------------------
// The defaults (Lvcid, 2026-10-11): screen dim 67%, window scale 110% (of the 0.85 that's "100%"; more on small screens:
// ScreenDefaultScale), hover sounds at 69%,
// the compact (small) list on every page, the theme Ballest Night (the Plugin Manager plugin's setting), DEBUG all off.
constexpr double kDefaultDim = 0.67, kDefaultScale = 0.85 * 1.1, kDefaultHoverVol = 0.69;
struct Prefs {
    double scale = kDefaultScale, dim = kDefaultDim;
    bool scaleChosen = false;               // the window scale moved by the player (else the screen's default)
    double winW = 0, winH = 0;              // the window's size on screen (UI units), 0: the concept's
    bool marquee = true;
    std::string view = "compact";           // compact or thick (compact by default: Lvcid, cleaner)
    std::string cosView = "compact";        // the cosmetics page's own: compact or thick (compact by default: Lvcid)
    double diskH = 0;                       // advanced: the disk box's height (dragged by its bottom edge); 0: as tall as what it shows
    // general's hover sounds (Lvcid): the plugin manager's own hover sounds (on by default) and their volume, 0..1 of
    // the slider (69% by default, kDefaultHoverVol; the footer's buttons follow it too: HoverSoundLevel)
    bool hoverSound = true;
    double hoverVol = kDefaultHoverVol;
    bool dimOn = true;                      // the screen dim on (its toggle, general: Lvcid)
};
Prefs gPrefs;
bool gPrefsRead = false;
double gPrefsDirtyAt = -1;

// The window scale for this screen, until the player picks one (Lvcid, 2026-10-15): 110% at 1080p and up, more on
// smaller screens, where the small words got too small to read (AnythingGoes' review): 150% at 720p, in between
// by the screen's pixels per unit, in steps of 5.
double ScreenDefaultScale() {
    const double s = game::ViewportScale();
    if (s <= 0) return kDefaultScale;
    const double small = std::clamp((1.0 - s) / (1.0 - 2.0 / 3.0), 0.0, 1.0);
    const double pct = std::round((110 + 40 * small) / 5) * 5;
    return pct / 100 * 0.85;
}

void ReadPrefs() {
    auto get = [](const char* key, const char* fallback) { return storage::Get(kSelf, std::string("manager.") + key, fallback); };
    // (a saved number that isn't one (nan, inf: clamp keeps a nan) reads as the default)
    auto num = [&](const char* key, const char* fallback) {
        const double v = std::atof(get(key, fallback).c_str());
        return std::isfinite(v) ? v : std::atof(fallback);
    };
    gPrefs.scale = std::clamp(num("scale", "0.935"), 0.68, 1.275);      // (80..150% of 0.85)
    gPrefs.scaleChosen = get("scaleset", "0") == "1";
    if (!gPrefs.scaleChosen) gPrefs.scale = ScreenDefaultScale();
    gPrefs.dim = std::clamp(num("dim", "0.67"), 0.0, 0.99);
    // (Lvcid, twice: darker. Once, whatever was saved goes up to the new default; the slider can lower it again)
    // (Lvcid, after several tries: 60%. Once, whatever was saved goes to it, saved at once; the slider can change it)
    // (a fresh install: the default, 67%)
    if (get("dim5", "") != "1") {
        gPrefs.dim = kDefaultDim;
        storage::Set(kSelf, "manager.dim", "0.670");
        storage::Set(kSelf, "manager.dim5", "1");
    }
    gPrefs.winW = std::max(0.0, std::atof(get("width", "0").c_str()));
    gPrefs.winH = std::max(0.0, std::atof(get("height", "0").c_str()));
    gPrefs.marquee = get("marquee", "1") != "0";
    gPrefs.view = get("view", "compact");
    if (gPrefs.view != "thick") gPrefs.view = "compact";      // (the grid view is gone: Lvcid, clunky)
    if (get("view2", "") != "1") {
        gPrefs.view = "compact";
        storage::Set(kSelf, "manager.view", "compact");
        storage::Set(kSelf, "manager.view2", "1");
    }
    // (0, or never dragged: it fits what it shows (Lvcid); "diskh2": once, an older saved height goes to that)
    gPrefs.diskH = std::atof(get("diskh", "0").c_str());
    if (get("diskh2", "") != "1") {
        gPrefs.diskH = 0;
        storage::Set(kSelf, "manager.diskh", "0");
        storage::Set(kSelf, "manager.diskh2", "1");
    }
    if (gPrefs.diskH > 0) gPrefs.diskH = std::clamp(gPrefs.diskH, 120.0, 900.0);
    gPrefs.hoverSound = get("hoversound", "1") == "1";
    gPrefs.dimOn = get("dimon", "1") == "1";
    gPrefs.hoverVol = std::clamp(num("hovervol2", "0.69"), 0.0, 1.0);     // ("2": the new scale; 69% by default)
    gPrefs.cosView = get("cosview", "compact");
    if (gPrefs.cosView != "thick") gPrefs.cosView = "compact";     // (anything else, an older page's grid or list too: compact)
    if (get("cosview2", "") != "1") {       // (once: a saved view goes to the default, compact; the views can change it)
        gPrefs.cosView = "compact";
        storage::Set(kSelf, "manager.cosview", "compact");
        storage::Set(kSelf, "manager.cosview2", "1");
    }
    gPrefsRead = true;
}

// Saved once a change settles (sliders change every frame while dragged; not while a slider is test-dragged).
void SavePrefs() {
    auto set = [](const char* key, const std::string& value) { storage::Set(kSelf, std::string("manager.") + key, value); };
    char buffer[32];
    const Prefs& out = gPrefs;
    std::snprintf(buffer, sizeof buffer, "%.4f", out.scale);
    set("scale", buffer);
    set("scaleset", out.scaleChosen ? "1" : "0");
    std::snprintf(buffer, sizeof buffer, "%.3f", out.dim);
    set("dim", buffer);
    set("marquee", out.marquee ? "1" : "0");
    std::snprintf(buffer, sizeof buffer, "%.0f", out.winW);
    set("width", buffer);
    std::snprintf(buffer, sizeof buffer, "%.0f", out.winH);
    set("height", buffer);
    set("view", out.view);
    set("cosview", out.cosView);
    std::snprintf(buffer, sizeof buffer, "%.0f", out.diskH);
    set("diskh", buffer);
    set("hoversound", out.hoverSound ? "1" : "0");
    set("dimon", out.dimOn ? "1" : "0");
    std::snprintf(buffer, sizeof buffer, "%.2f", out.hoverVol);
    set("hovervol2", buffer);
    gPrefsDirtyAt = -1;
}
void PrefsChanged() { gPrefsDirtyAt = pmui::Now(); }


// --- what the pages show ---------------------------------------------------------------------------------------------------
struct Plug {
    std::string id, name, version, latest, author, description, icon, status, pending, page, minHost, home, category;
    int priority = 0, order = 0;            // within its category (higher first); where it was listed (load or registry order)
    std::vector<std::string> needs;
    std::vector<std::string> net;           // the servers it may connect to ([net] domains; none listed for the maintainer's own)
    bool installed = false, enabled = true, essential = false, update = false, settings = false, inRegistry = false;
    bool fresh = false;                     // new to the browser (its NEW tag)
    bool warned = false;                    // it logged a warning this session (on, but not quite right)
};
std::vector<Plug> gInstalled, gBrowse;
std::set<std::string> gTestUpdateIds;      // (DEBUG's pretend updates: nothing to download)
int gUpdates = 0;
double gModelAt = -100;

bool HasSettings(const std::string& id) {
    if (ui::HasMovable(id)) return true;
    for (const settings::Setting& s : settings::List())
        if (s.pluginId == id && !s.hidden) return true;
    return false;
}

// Days since 1970 for a "YYYY-MM-DD" (or today for ""); -1 if it isn't one.
long DayOf(const std::string& date) {
    int y = 0, m = 0, d = 0;
    if (date.empty()) {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        y = t.wYear, m = t.wMonth, d = t.wDay;
    } else if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
        return -1;
    }
    // days from civil (Howard Hinnant's)
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

// New (its NEW tag, on browse / explore only: Lvcid, 2026-10-10): less than 7 days old and its card not opened yet. Its
// age: from the registry's "added" when it has one, else from the day this plugin manager first saw it. Both are kept
// by the plugin's id in the plugin manager's storage, set once: an update (of it, or of the plugin manager) never makes
// it new again, so no one can bump theirs back up by updating. The first time the registry is read, everything in it
// counts as seen long ago (nothing is new on a first run).
bool Fresh(const registry::Entry& e) {
    if (storage::Get(kSelf, "manager.opened." + e.id, "") == "1") return false;     // (its card opened once: seen)
    const long today = DayOf("");
    if (const long added = DayOf(e.added); !e.added.empty() && added >= 0) return today - added < 7;
    // (the first look at the whole registry, the cosmetic packs included ("manager.seenall"; the packs joined the list
    // later, 2026-10-10): everything in it is old, even one noted earlier as first seen today)
    const bool first = storage::Get(kSelf, "manager.seenall", "") != "1";
    const std::string key = "manager.seen." + e.id;
    std::string seen = storage::Get(kSelf, key, "");
    if (seen.empty() || first) {
        seen = std::to_string(first ? 0 : today);
        storage::Set(kSelf, key, seen);
    }
    return today - std::atol(seen.c_str()) < 7;
}

// The plugins that logged a warning this session (read from the log as it grows).
std::set<std::string> gWarned;
std::map<std::string, std::string> gWarnText;  // the last warning each logged
size_t gWarnedRead = 0;
void ReadWarnings() {
    const size_t count = hostlog::LineCount();
    for (size_t i = gWarnedRead; i < count; ++i) {
        const std::string line = hostlog::Line(i);
        // "[time] [warn] [source] message"
        const size_t a = line.find("] [warn] [");
        if (a == std::string::npos) continue;
        const size_t b = line.find(']', a + 10);
        if (b == std::string::npos) continue;
        const std::string source = line.substr(a + 10, b - a - 10);
        gWarned.insert(source);
        gWarnText[source] = b + 2 <= line.size() ? line.substr(b + 2) : "";
    }
    gWarnedRead = count;
}

// Lvcid's rule for every red dot: it shows until its item has been hovered, and goes when the pointer leaves it (this
// session; a new dot, e.g. another update, has another key).
std::set<std::string> gDotsSeen, gDotsHovered;
bool DotShown(const std::string& key, bool hovered) {
    if (gDotsSeen.count(key)) return false;
    if (hovered) gDotsHovered.insert(key);
    else if (gDotsHovered.erase(key)) gDotsSeen.insert(key);
    return !gDotsSeen.count(key);
}

bool gTestToggles = false;                  // debug: the first two plugins that can be switched show a warning and an error
int gTestHost = 0;                          // debug: the plugin manager's update pretended (1 out .. 4 failed; HostStage)
double gTestHostAt = -100;                  // (a pretend "update now": when it started)
int gTestNeedsHost = 0;                     // debug: some gets / updates need a newer plugin manager (1 within it, 2 not out)
bool gTestUpdates = false;                  // debug: the first three plugins that can be updated show a pretend update
bool gRestartNoticed = false;               // the restart notice was shown this session
std::string gTestWarnId, gTestErrId;

void BuildModel() {
    gModelAt = pmui::Now();
    ReadWarnings();
    gInstalled.clear();
    gBrowse.clear();
    gUpdates = 0;
    const std::vector<registry::Entry>& entries = registry::Entries();
    auto entryOf = [&](const std::string& id) -> const registry::Entry* {
        for (const registry::Entry& e : entries)
            if (e.id == id) return &e;
        return nullptr;
    };
    for (const plugins::Info& info : plugins::List()) {
        Plug p;
        p.id = info.id;
        p.name = info.name.empty() ? info.id : info.name;
        p.version = p.latest = info.version;
        p.author = info.author;
        p.description = info.description;
        p.icon = info.icon;
        p.status = info.status;
        p.essential = info.essential;
        p.installed = true;
        p.net = info.netDomains;
        p.enabled = !plugins::IsOff(info.id);
        p.pending = registry::Pending(info.id);
        if (const registry::Entry* e = entryOf(info.id)) {
            p.inRegistry = true;
            p.latest = e->version;
            p.page = e->Page();
            p.home = e->repo.empty() ? "" : "https://github.com/" + e->repo;
            p.needs = e->dependencies;
            p.minHost = e->minHost;
            if (!e->description.empty()) p.description = e->description;
            if (p.author.empty()) p.author = e->author;
            if (p.icon.empty()) p.icon = e->icon;
            p.update = plugins::CompareVersions(e->version, info.version) > 0;
        }
        // (the maintainer's own plugins don't list their servers, as the Classic menu: OWN_REPOS in its main.as)
        if (p.home.rfind("https://github.com/AnythingGoes-ballest/", 0) == 0) p.net.clear();
        if (info.id == kSelf && p.home.empty()) {
            p.home = "https://github.com/AnythingGoes-ballest/ballest-plugin-manager";
            p.page = p.home + "/tree/main/plugins/plugin-manager";
        }
        p.settings = HasSettings(info.id);
        p.warned = gWarned.count(info.id) > 0;
        registry::Placing(info.id, &p.category, &p.priority);
        // (one the registry doesn't place, kept in plugins\cosmetics (a cosmetic plugin of the player's own): a cosmetic)
        if (p.category.empty()) {
            const std::wstring dir = plugins::FolderOf(info.id), cos = plugins::CosmeticsDir();
            if (!cos.empty() && dir.size() > cos.size() && dir.compare(0, cos.size(), cos) == 0) p.category = "cosmetics";
        }
        p.order = static_cast<int>(gInstalled.size());
        if (p.update) ++gUpdates;
        gInstalled.push_back(std::move(p));
    }
    for (const registry::Entry& e : entries) {
        // (the cosmetic packs too, unlisted on the plugins page's browse: the cosmetics page's explore shows them)
        if (!e.listed && e.category != "cosmetics") continue;
        Plug p;
        p.id = e.id;
        p.name = e.name.empty() ? e.id : e.name;
        p.version = p.latest = e.version;
        p.author = e.author;
        p.description = e.description;
        p.icon = e.icon;
        p.page = e.Page();
        p.home = e.repo.empty() ? "" : "https://github.com/" + e.repo;
        p.needs = e.dependencies;
        p.minHost = e.minHost;
        p.inRegistry = true;
        p.pending = registry::Pending(e.id);
        plugins::Info info;
        p.installed = plugins::Find(e.id, &info);
        p.fresh = Fresh(e);
        p.category = e.category;
        p.priority = e.priority;
        p.order = static_cast<int>(gBrowse.size());
        gBrowse.push_back(std::move(p));
    }
    // Both lists in their categories' order (the plugin manager, enhancements, utilities, fun, the rest), the higher
    // priority first in each, else as they were listed.
    auto placed = [](const Plug& a, const Plug& b) {
        const int ra = registry::CategoryRank(a.category), rb = registry::CategoryRank(b.category);
        if (ra != rb) return ra < rb;
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.order < b.order;
    };
    std::stable_sort(gInstalled.begin(), gInstalled.end(), placed);
    gTestUpdateIds.clear();
    if (gTestUpdates) {
        int n = 0;
        for (Plug& p : gInstalled) {
            if (n >= 3 || p.essential || p.update || p.id == kSelf) continue;
            p.update = true;
            p.latest = p.version + "-test";
            gTestUpdateIds.insert(p.id);
            ++gUpdates;
            ++n;
        }
    }
    gTestWarnId.clear();
    gTestErrId.clear();
    for (const Plug& p : gInstalled) {
        if (p.essential || !p.enabled) continue;
        if (gTestWarnId.empty()) gTestWarnId = p.id;
        else if (gTestErrId.empty()) gTestErrId = p.id;
    }
    std::stable_sort(gBrowse.begin(), gBrowse.end(), placed);
    if (registry::State() == "ready") {     // (the registry has been seen once, all of it)
        storage::Set(kSelf, "manager.seen", "1");
        storage::Set(kSelf, "manager.seenall", "1");
    }
}

const Plug* FindPlug(const std::string& id) {
    for (const Plug& p : gInstalled)
        if (p.id == id) return &p;
    for (const Plug& p : gBrowse)
        if (p.id == id) return &p;
    return nullptr;
}

std::string HostUpdate() {
    const std::string latest = registry::HostVersion();
    return !latest.empty() && plugins::CompareVersions(latest, plugins::kHostVersion) > 0 ? latest : "";
}

// The plugin manager plugin's Theme setting ("Ballest Night", "Ballest Light", "Lucid Lime" or "Classic"): Classic, its classic menu is used instead
// of this window (which then only shows its settings).
int NewLookIndex() {
    const std::vector<settings::Setting>& list = settings::List();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].pluginId == kSelf && list[i].variable == "Theme") return static_cast<int>(i);
    return -1;
}

// --- state ---------------------------------------------------------------------------------------------------------------
struct Scroll {
    double pos = 0, target = 0, max = 0;
};
struct State {
    bool open = false;
    std::string page = "plugins", tab = "installed";
    std::string overlay, overlayShown, section = "general";
    double overlayT = 0, sectionAt = -100;
    std::string expanded, scrollTo;
    std::map<std::string, Scroll> scrolls;
    std::string popup, popupId;             // "remove" or "get", and the plugin
    bool forget = false;
    double bannerT = 0;
    bool bannerLater = false;
    std::string flash;
    double flashAt = -100;
    std::string choiceOpen;                 // the settings dropdown that is open
    // A jump into a plugin's settings (a row's cog): where it came from, for the pane's back button,
    // shown while that section is still the one open (any other way of moving on forgets it)
    std::string backFor, backOverlay, backSection;
    std::string installedSearch;            // the page's search (both its tabs)
    double installedReadAt = -100;
    bool installedFocused = false, sortOpen = false;
    std::string swapFrom;                   // installed / browse: the tab being switched away from, while its fog drifts
    double swapAt = -100;
    std::string filter = "everything";
    size_t clearedAt = 0;
    bool stick = true, focusPrompt = false;
};
State gS;

// --- the plugin manager's own update, and what waits for it (Lvcid, 2026-10-09) -----------------------------------------
// Where its update is: none, out (available), downloading, installed (the game not restarted yet: the new one runs from
// the next start), failed. DEBUG can pretend each (gTestHost; version 9.9.9: nothing is downloaded).
enum class HostStage { None, Available, Downloading, Installed, Failed };
struct HostNow {
    HostStage stage = HostStage::None;
    std::string version;                    // the newer one
    std::string error;
};
HostNow Host() {
    HostNow h;
    if (gTestHost > 0) {
        // (a pretend "update now": downloading for a moment, then installed)
        if (gTestHost == 2 && gTestHostAt > 0 && game::Seconds() - gTestHostAt > 2.5) gTestHost = 3;
        h.stage = static_cast<HostStage>(gTestHost);
        h.version = "9.9.9";
        if (h.stage == HostStage::Failed) h.error = "error: version.dll: a pretend failure (DEBUG)";
        return h;
    }
    h.version = HostUpdate();
    if (h.version.empty()) return h;
    const std::string state = registry::HostUpdateState();
    h.stage = state == "downloading" ? HostStage::Downloading
              : state == "restart"   ? HostStage::Installed
              : state.rfind("error", 0) == 0 ? HostStage::Failed
                                              : HostStage::Available;
    if (h.stage == HostStage::Failed) h.error = state;
    return h;
}
bool HostShown(const HostNow& h) {          // counted as an update (to do, or doing)
    return h.stage == HostStage::Available || h.stage == HostStage::Failed || h.stage == HostStage::Downloading;
}

// DEBUG: some gets and updates need a newer plugin manager (1: 9.9.9, within the pretend update; 2: 99.0.0, not out
// yet): the first browse plugin, the first installed plugin with an update, the first cosmetic pack not got, and the
// first missing dependency of a browse plugin (so a plugin is locked by what it would fetch).
std::string TestNeedsHostOf(const std::string& id) {
    if (gTestNeedsHost == 0) return "";
    const std::string version = gTestNeedsHost == 1 ? "9.9.9" : "99.0.0";
    for (const Plug& p : gBrowse)
        if (!p.installed) {
            if (p.id == id) return version;
            break;
        }
    for (const Plug& p : gInstalled)
        if (p.update) {
            if (p.id == id) return version;
            break;
        }
    for (const registry::Entry& e : registry::Entries())
        if (registry::IsCosmetic(e.id, e.dependencies) && !plugins::Find(e.id, nullptr)) {
            if (e.id == id) return version;
            break;
        }
    for (const Plug& p : gBrowse) {
        if (p.installed) continue;
        for (const std::string& d : p.needs)
            if (!plugins::Find(d, nullptr)) return d == id ? version : "";
    }
    return "";
}

// The plugin manager version a get or update of `id` needs that this one isn't: its own min_host, or that of a missing
// dependency it would fetch with it, down the chain; "" when the installer would take it. (Plugins needing plugins are
// fetched as they are: only the host's version locks. registry::Install refusing the rest is the backstop, logged.)
std::string NeedsHost(const std::string& id, int depth = 0) {
    std::string need = TestNeedsHostOf(id);
    auto keep = [&need](const std::string& v) {
        if (!v.empty() && (need.empty() || plugins::CompareVersions(v, need) > 0)) need = v;
    };
    const registry::Entry* e = nullptr;
    for (const registry::Entry& x : registry::Entries())
        if (x.id == id) e = &x;
    if (!e) return need;
    if (!e->minHost.empty() && plugins::CompareVersions(e->minHost, plugins::kHostVersion) > 0) keep(e->minHost);
    if (depth < 8)
        for (const std::string& d : e->dependencies)
            if (!plugins::Find(d, nullptr)) keep(NeedsHost(d, depth + 1));
    return need;
}

void Tip(Layer& pane, const std::string& key, const Rect& hit, const std::string& text, const Color* ink = nullptr, double width = 0,
         bool alignRight = false);     // (settings' tooltips: below)

// Why something needing plugin manager `need` can't be got yet, and what to do (the lock's tooltip).
std::string LockReason(const std::string& need) {
    const HostNow h = Host();
    const std::string pm = "needs plugin manager " + need;
    if (h.stage == HostStage::None || plugins::CompareVersions(h.version, need) < 0) return pm + " (not out yet)";
    switch (h.stage) {
        case HostStage::Available: return pm + ": update the plugin manager first";
        case HostStage::Downloading: return pm + ": the plugin manager is updating";
        case HostStage::Installed: return "restart the game to use it (the plugin manager is updated)";
        case HostStage::Failed: return pm + ": its update didn't finish (try again on the banner)";
        default: return pm;
    }
}
bool gClosedByX = false;                    // closed by the top bar's x: it stays white as the window fades out
ux::Screen gScreen;
// What takes clicks away from the game while the window is open: the screen, but for a hole round the footer's plugins
// button (it stays live: its hover, and a click closes the window; Lvcid), in four pieces round the hole.
ux::Node gBlocker[4];
eng::Weak gBlockerCanvas;
// Where that button is (the viewport's units), measured only when it may have moved (Lvcid: not every frame): when the
// window opens, and when the footer's buttons or the screen's size change (a plugin installed or removed, a new map's
// footer, the game's window resized); each time for half a second, while the footer lays itself out again.
struct FooterHole {
    ux::Rect at;
    std::string key;                        // what the footer was when last measured
    double until = -1;                      // measured until then
} gHole;
std::vector<std::string> gForgetAfterRemove;
Obj gTypingField = nullptr;

// A text box kept from frame to frame: held weakly (as the rest of the host holds widgets), so one freed with its screen
// (a map change) reads as null instead of a dangling pointer (AnythingGoes' review: a pad's Y could reach a freed one).
struct FieldRef {
    eng::Weak weak;
    FieldRef() = default;
    FieldRef(Obj o) : weak(o ? eng::MakeWeak(o) : eng::Weak{}) {}
    FieldRef& operator=(Obj o) {
        weak = o ? eng::MakeWeak(o) : eng::Weak{};
        return *this;
    }
    operator Obj() const { return eng::Get(weak); }
};
bool gTypedLastFrame = false;               // a text box had the keyboard last frame (Esc then leaves the box first)

struct Anim {
    double value = 0, open = 0, knob = -1, since = 0;
    bool was = false;
};
std::map<std::string, Anim> gAnim;
std::map<std::string, double> gOpenExtra;   // a row's details' height, from the frame before
std::map<std::string, double> gContentH;    // a scrolling area's content height, from the frame before

// Eased 0..1 toward `on`.
double Ease(const std::string& key, bool on, double tau = kEase) {
    Anim& a = gAnim[key];
    a.value = ux::Approach(a.value, on ? 1.0 : 0.0, pmui::Dt(), tau);
    return a.value;
}

// Anything coming into the window comes in sharply (Lvcid): popups fade in over kPopupIn, and the window under them dims
// a little (kFocusDim: its own, not the screen's dim setting; the corner logo isn't in the window, so it stays).
constexpr double kPopupIn = 0.1, kFocusDim = 0.22;
std::string gPopupShown;                    // the popup PopupIn last faded in ("" when closed: the next one fades in)
double PopupIn() {
    std::string& shown = gPopupShown;
    static double at = -100;
    if (gS.popup != shown) {
        shown = gS.popup;
        at = pmui::Now();
    }
    const double u = std::clamp((pmui::Now() - at) / kPopupIn, 0.0, 1.0);
    return 1 - (1 - u) * (1 - u);
}

// A text's width as the key draws it (its face), measured or estimated.
// Eased toward `on`, rising and falling at their own speeds.
// Toward `target` (0..1): a button whose thing is open rests part lit, hovered all the way (Lvcid: open things stay lit,
// dimmer than the hover, so the hover still shows).
constexpr double kOpenLit = 0.55;
double EaseTo(const std::string& key, double target, double tau = kEase) {
    Anim& a = gAnim[key];
    a.value = ux::Approach(a.value, target, pmui::Dt(), tau);
    return a.value;
}

double EaseUpDown(const std::string& key, bool on, double up, double down) {
    Anim& a = gAnim[key];
    a.value = ux::Approach(a.value, on ? 1.0 : 0.0, pmui::Dt(), on ? up : down);
    return a.value;
}

// A hover's 0..1 (Lvcid: liquid smooth and responsive, no lag either way): both ways it moves at once and settles into
// its end (no ease in, an ease out): in over `up` seconds, out over `down` (resting a little slower than the quick
// hover). Turning back midway it sets off from where it is, over the share of the way left, so it never jumps.
constexpr double kHoverIn = 0.16, kHoverOut = 0.3;   // the top bar's (cog, pages, keys)
struct HoverWay {
    double v = 0, from = 0, t = 0;
    bool on = false;
};
std::map<std::string, HoverWay> gHoverWays;
// (the top bar's hovers start from rest when the window opens: the x clicked to close it stayed lit on the next open)
void HoverReset(const std::string& prefix) {
    for (auto& [k, w] : gHoverWays)
        if (k.rfind(prefix, 0) == 0) w = HoverWay{};
}
double HoverCurve(const std::string& key, bool on, double up, double down) {
    HoverWay& w = gHoverWays[key];
    if (on != w.on) {
        w.on = on;
        w.from = w.v;
        w.t = 0;
    }
    w.t += pmui::Dt();
    const double length = on ? up * (1 - w.from) : down * w.from;
    const double u = length <= 1e-6 ? 1.0 : std::clamp(w.t / length, 0.0, 1.0);
    const double e = 1 - std::pow(1 - u, 2.5);
    w.v = on ? w.from + (1 - w.from) * e : w.from * (1 - e);
    return w.v;
}

// The game's own hover sound (its main menu buttons', WBP_ButtonBase: the engine's Flashlight_Switch; Lvcid, 2026-10-10)
// as the pointer comes onto anything that can be clicked (every button, card, tab and key, the logo; pmui tells us:
// OnHoverStart), never more than one every 45 ms (sweeping down the list). Scroll bars, sliders, the resize edges and the
// window's own background are quiet.
constexpr const char* kHoverSound = "/Engine/VREditor/Sounds/UI/Flashlight_Switch.Flashlight_Switch";
void HoverSound(const std::string& /*id*/) {
    if (!gPrefs.hoverSound || gPrefs.hoverVol <= 0.001) return;      // (general's hover sounds: on by default, 69%)
    static double lastAt = -1;
    if (game::Seconds() - lastAt <= 0.045) return;
    lastAt = game::Seconds();
    // (as the game's discord button plays it: double pitch, from 0.11 s in; its volume the slider's, 69% by default)
    draw::UiSound(kHoverSound, gPrefs.hoverVol, 2.0, 0.11);
}

double Measured(Layer& l, const std::string& key, const std::string& s, double px) {
    const pmui::Entry* e = pmui::Find(l, key);
    if (e && e->measured == s && e->width > 0) return e->width;
    return pmui::Width(s, px, e ? e->face : "Regular");
}

void Unfocus() { eng::Call(eng::FindCdo("WidgetBlueprintLibrary"), "SetFocusToGameViewport"); }

bool Focused(Obj field) {
    if (!field || !eng::Call(field, "HasKeyboardFocus").ReturnBool()) return false;
    gTypingField = field;
    return true;
}

void OpenUrl(const std::string& url) {
    if (url.rfind("https://github.com/", 0) != 0 || url.find_first_of(" \"<>") != std::string::npos) return;
    const std::wstring wide = eng::Widen(url);
    eng::Call(eng::FindCdo("KismetSystemLibrary"), "LaunchURL",
              eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
    hostlog::Info("manager: opened " + url);
}

// Explorer can take a while to start: from a thread of its own (as plugins::OpenFolder).
void OpenDir(const std::wstring& dir) {
    std::thread([dir] {
        wchar_t windows[MAX_PATH];
        const UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(windows, n) + L"\\explorer.exe\" \"" + dir + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }).detach();
}

void CopyText(const std::string& text) {
    const std::wstring wide = eng::Widen(text);
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))) {
        std::memcpy(GlobalLock(h), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

void OpenOverlay(const std::string& overlay, const std::string& section = "") {
    gS.backFor.clear();
    gS.overlay = overlay;
    if (!overlay.empty()) gS.overlayShown = overlay;
    if (!section.empty() && section != gS.section) {
        gS.section = section;
        gS.sectionAt = game::Seconds();          // (may be opening the window: pmui::Now() is behind until it's drawn)
        gS.scrolls["pane"] = {};
    }
    if (overlay == "console") {
        gS.focusPrompt = true;
        gS.stick = true;
    }
    gS.choiceOpen.clear();
}

void OpenSettingsOf(const std::string& id) {
    // (the plugin manager's own: its general page, where its theme is too (Lvcid, 2026-10-11))
    const std::string fromOverlay = gS.overlay, fromSection = gS.section,
                      to = id == kSelf ? "general" : "plugin:" + id;     // (the theme is on general too)
    OpenOverlay("settings", to);
    if (fromOverlay != "settings" || fromSection != to) {
        gS.backFor = to;
        gS.backOverlay = fromOverlay;
        gS.backSection = fromSection;
    }
    gS.flash = "header";
    gS.flashAt = pmui::Now();
}

void Get(const Plug& p) {
    std::vector<std::string> missing;
    for (const std::string& need : p.needs) {
        plugins::Info info;
        if (!plugins::Find(need, &info)) missing.push_back(need);
    }
    if (!missing.empty()) {
        gS.popup = "get";
        gS.popupId = p.id;
        return;
    }
    hostlog::Info("manager: get " + p.id);
    registry::Install(p.id);
    gModelAt = -100;
}

// --- small pieces ------------------------------------------------------------------------------------------------------
// The Customize page's hover shine round r: its halo, and its rays cross-faded round the shape (glow-<shape>-*.png:
// "globe" round, drawn for a 52-unit globe; "slot" a rounded square, for an 80-unit slot), 12 units out at the size it
// was drawn for: scaled with r, so it hugs a button of any size (and grows with it).
void Shine(Layer& l, const std::string& key, int z, const Rect& r, const char* shape, double lit) {
    if (lit < 0.01) return;
    const std::string s = shape;
    // drawn for: a 52-unit globe, an 80-unit slot (by their width), a 30-unit tall pill (by its height: any length)
    const bool pill = s == "pill";
    const double drawnFor = s == "globe" ? 52 : pill ? 30 : 80, size = pill ? r.h : r.w;
    // its shape pulled in a little under the button's edge, so the halo starts flush with it (no dark ring between)
    const Rect inner = r.Grow(-0.035 * size);
    const Rect out = inner.Grow(12 * (pill ? inner.h : inner.w) / drawnFor);
    pmui::Pic(l, key + ".halo", z, out, ("glow-" + s + "-halo.png").c_str(), kWhite).Opacity(static_cast<float>(lit));
    const double phase = std::fmod(pmui::Now() / 1.2, 1.0) * 4;
    for (int i = 0; i < 4; ++i) {
        const double apart = std::fabs(phase - i), weight = std::max(0.0, 1 - std::min(apart, 4 - apart));
        pmui::Pic(l, key + ".rays" + std::to_string(i), z, out, ("glow-" + s + "-rays" + std::to_string(i) + ".png").c_str(), kWhite)
            .Opacity(static_cast<float>(lit * weight));
    }
}

// A heading's lime tick in r. Ballest Light (Lvcid): a wider black tick behind it and the lime one thinner on top
// (its trim, drawn inside a tick this thin, covered the lime).
void Tick(Layer& l, const std::string& key, int z, const Rect& r) {
    if (BallestLight()) {
        ux::RawColors raw;
        pmui::Box(l, key + ".black", z, r.Grow(1.2), kBlack, 2.4);
        // (one above: at the same z the black could land over it, the tick's lime gone dark: Lvcid's "blips")
        pmui::Box(l, key, z + 1, {r.x + 0.5, r.y + 1, r.w - 1, r.h - 2}, kLime, 1);
        return;
    }
    pmui::Box(l, key, z, r, kLime, 1.5);
}

void Glow(Layer& l, const std::string& key, int z, const Rect& r, const Color& c, double alpha) {
    Color colour = c;
    if (BallestLight()) {
        // (softer on the light panel; a white glow, there invisible, a soft dark shadow instead: hovered things lift,
        // Lvcid's "flat" hovers)
        alpha *= 0.55;
        const float lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b, sat = std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b});
        if (lum > 0.6f && sat < 0.15f) {
            colour = kBlack;
            alpha *= 0.8;
        }
    }
    if (alpha < 0.01) return;
    pmui::Sliced(l, key, z, r.Grow(kGlowRoom), "pm-glow.png", kGlowMargin, kGlowUnits, A(colour, alpha));
}


// A rounded box's outline (`radius`, `width`) in `c`, on a group of its own (the hover edges round the bubble buttons).
void EdgeOutline(Layer& l, const std::string& key, int z, const Rect& r, double radius, float width, const Color& c, double opacity = 1) {
    Layer& g = pmui::Group(l, key + ".m", z, r.Grow(width), false, opacity);
    pmui::Box(g, "o", 0, {width, width, r.w, r.h}, A(kWhite, 0), radius).Outline(width, c);
}

// The same, a touch metallic (the icon tiles' frame): as chrome reflects, its top a little brighter, a darker band just
// under its middle (the horizon), its bottom a soft silver; `strength` 0..1 how much.
const Color kSteel = ux::Hex("#6E737B"), kSilver = ux::Hex("#B9BDC4");
void MetalOutline(Layer& l, const std::string& key, int z, const Rect& r, double radius, float width, const Color& c, double strength,
                  double opacity = 1) {
    if (strength <= 0) return EdgeOutline(l, key, z, r, radius, width, c, opacity);
    const double s = strength;
    struct Part {
        double from, to;
        Color colour;
    };
    const Part parts[] = {{0.00, 0.40, Mix(c, A(kWhite, c.a), 0.35 * s)},          // the sky
                          {0.40, 0.50, Mix(c, A(kSilver, c.a), 0.30 * s)},
                          {0.50, 0.62, Mix(c, A(kSteel, c.a), 0.45 * s)},          // the horizon
                          {0.62, 1.00, Mix(c, A(kSilver, c.a), 0.28 * s)}};        // the ground
    const double pad = width + 1;
    for (size_t i = 0; i < 4; ++i) {
        const double y0 = -pad + (r.h + 2 * pad) * parts[i].from, y1 = -pad + (r.h + 2 * pad) * parts[i].to;
        Layer& g = pmui::Group(l, key + ".m" + std::to_string(i), z, {r.x - pad, r.y + y0, r.w + 2 * pad, y1 - y0}, true, opacity);
        pmui::Box(g, "o", 0, {pad, -y0, r.w, r.h}, A(kWhite, 0), radius).Outline(width, parts[i].colour);
    }
}

// A rule (a thin bar) in `c`, its ends round.
void Rule(Layer& l, const std::string& key, int z, const Rect& r, const Color& c) { pmui::Box(l, key, z, r, c, std::min(r.w, r.h) / 2); }

// A square icon button (a row's settings, source, remove): dark at rest, white when hovered; black in a hovered row.
// `hoverFill`: another colour than white when hovered (remove: red, its icon white), and `grow`: bigger by that, eased.
bool IconButton(Layer& l, const std::string& id, int z, const Rect& r, const char* icon, double rowHover, double radius = 12,
                const Color* hoverFill = nullptr, double grow = 0, const Color* restFill = nullptr, const Color* restEdge = nullptr,
                const Color* restIcon = nullptr) {
    const double h = Ease(id, pmui::Over(l, r));
    const double k = 1 + grow * Smooth(h);
    const Rect g{r.Cx() - r.w * k / 2, r.Cy() - r.h * k / 2, r.w * k, r.h * k};
    const Color fill = hoverFill ? *hoverFill : kWhite;
    Glow(l, id + ".glow", z, g, fill, (hoverFill ? 0.55 : 0.4) * h);
    pmui::Box(l, id + ".b", z + 1, g, Mix(Mix(restFill ? *restFill : kAb, A(kBlack, 0.85), rowHover), fill, h), radius * k)
        .Outline(2, Mix(Mix(restEdge ? *restEdge : kAbEdge, kBlack, rowHover), fill, h));
    const double s = std::min(24.0, r.w * 0.44) * k;
    const Color rest = restIcon ? *restIcon : Mix(kSub, kLime, rowHover);
    if (hoverFill && ux::ThemeMapped()) {
        // (Ballest Light: on its red, the icon goes to a true white, Lvcid; its rest as the theme draws it)
        const Color from = ux::Themed(rest, 2);
        ux::RawColors raw;
        pmui::Pic(l, id + ".i", z + 2, {r.Cx() - s / 2, r.Cy() - s / 2, s, s}, icon, Mix(from, kWhite, h));
    } else {
        pmui::Pic(l, id + ".i", z + 2, {r.Cx() - s / 2, r.Cy() - s / 2, s, s}, icon, Mix(rest, hoverFill ? kWhite : kBlack, h));
    }
    return pmui::Click(l, r, id);
}

// A round icon button (folder, refresh, close).
bool RoundButton(Layer& l, const std::string& id, int z, const Rect& r, const char* icon, double iconSize = 20) {
    const double h = Ease(id, pmui::Over(l, r));
    Glow(l, id + ".glow", z, r, kWhite, 0.35 * h);
    pmui::Box(l, id + ".b", z + 1, r, Mix(kField, kWhite, h), r.h / 2).Outline(2, Mix(kAbEdge, kWhite, h));
    pmui::Pic(l, id + ".i", z + 2, {r.Cx() - iconSize / 2, r.Cy() - iconSize / 2, iconSize, iconSize}, icon, Mix(kSub, kBlack, h));
    return pmui::Click(l, r, id);
}

// An icon button that can't be clicked (nothing to open, or can't be done): black, a faint edge, the icon faint.
void OffButton(Layer& l, const std::string& id, int z, const Rect& r, const char* icon) {
    pmui::Box(l, id + ".off", z, r, kOffFill, 13).Outline(2, A(kAbEdge, 0.5));
    pmui::Pic(l, id + ".offi", z + 1, {r.Cx() - 10, r.Cy() - 10, 20, 20}, icon, A(kSub, 0.25));
}

enum class PillKind { Lime, Ghost, White, Dark };
double PillWidth(Layer& l, const std::string& id, const std::string& label, double px, const char* icon, double padX = 18) {
    return padX * 2 + Measured(l, id + ".t", label, px) + (icon ? px + 8 : 0);
}

// A text button. Lime: rests as the bubble, turns white when hovered (or `white`: a hovered row's get). Ghost: rests
// grey, turns to the bubble. White: the banner's main button. Dark: on the banner, ink on the bubble.
bool Pill(Layer& l, const std::string& id, int z, const Rect& r, const std::string& label, double px, const char* icon, PillKind kind,
          bool white = false, double radius = 13, const Color* ghostBack = nullptr, const Color* ghostInk = nullptr) {
    const double h = Ease(id, pmui::Over(l, r));
    Color ink = kBlack;
    const char* face = kind == PillKind::Ghost || kind == PillKind::Dark ? "Regular" : "Bold";
    if (kind == PillKind::Lime) {
        const double wt = std::max(h, white ? 1.0 : 0.0);
        Glow(l, id + ".glow", z, r, wt > h ? kLime : kWhite, white ? 0.3 : 0.4 * h);
        pmui::Bubble(l, id + ".bub", z + 1, r, radius, 1);
        pmui::Box(l, id + ".w", z + 2, r, A(kWhite, wt), radius);
    } else if (kind == PillKind::Ghost) {
        pmui::Box(l, id + ".b", z + 1, r, ghostBack ? *ghostBack : kChip, radius);
        pmui::Bubble(l, id + ".bub", z + 2, r, radius, h, true);
        ink = ghostInk ? Mix(*ghostInk, kWhite, 0.25 * h) : Mix(kTabHover, kWhite, h);      // (ghostInk: its own words' colour)
    } else if (kind == PillKind::White) {
        pmui::Box(l, id + ".b", z + 1, r, Mix(kWhite, kBlack, h), radius);
        ink = Mix(kBlack, kLime, h);
    } else {
        pmui::Box(l, id + ".b", z + 1, r, A(kBlack, 0.12 * h), radius);
        ink = kBannerInk;
    }
    const double tw = Measured(l, id + ".t", label, px), iw = icon ? px + 8 : 0;
    const double x = r.Cx() - (tw + iw) / 2;
    if (icon) pmui::Pic(l, id + ".i", z + 3, {x, r.Cy() - px / 2, px, px}, icon, ink);
    pmui::Text(l, id + ".t", z + 3, label, px, face, ink, x + iw, r.Cy());
    return pmui::Click(l, r, id);
}

enum class ChipKind { Run, Off, Update, Built, Error, Warn, Plain };
// A small status label; returns its width.
double Chip(Layer& l, const std::string& key, int z, double x, double cy, const std::string& text, ChipKind kind, double rowHover, double fade = 1) {
    Color ink = kSub, back = kChip;
    if (kind == ChipKind::Run) ink = kLime, back = A(kLime, 0.1);
    if (kind == ChipKind::Off) ink = kVer;
    if (kind == ChipKind::Update) ink = kBlack, back = kLime;
    const bool night = BallestNight();
    if (kind == ChipKind::Error) ink = night ? kErrAccent : kErr, back = A(ink, night ? 0.14 : 0.12);
    if (kind == ChipKind::Warn) ink = night ? kWarnAccent : kGold, back = A(ink, 0.13);
    ink = Lift(ink, rowHover);
    back = Lift(back, rowHover);
    if (fade < 1) ink = Mix(kBlack, ink, fade), back = A(back, back.a * fade);
    const bool dot = kind == ChipKind::Run;
    const double tw = Measured(l, key + ".t", text, 13), dw = dot ? 11 : 0, wd = tw + dw + 18;
    const Rect box{x, cy - 11, wd, 22};
    if (BallestLight()) {
        // (Ballest Light: a dark chip, its words in their own colour, a faint glow of it round it (Lvcid: on the light
        // card they couldn't be read))
        ux::RawColors raw;
        if (kind == ChipKind::Off || kind == ChipKind::Plain) ink = Mix(ink, kWhite, 0.35);
        if (kind == ChipKind::Update) ink = Lift(kLime, rowHover);      // (its black words were for a lime chip)
        if (kind != ChipKind::Off && kind != ChipKind::Plain) Glow(l, key + ".glow", z, box, ink, 0.45 * fade);
        pmui::Box(l, key + ".b", z, box, A(ux::Hex("#1A1B1F"), fade), 8).Outline(1, A(ink, 0.35 * fade));
        if (dot) pmui::Pic(l, key + ".d", z + 1, {x + 8, cy - 3, 6, 6}, "pm-dot.png", ink);
        pmui::Text(l, key + ".t", z + 1, text, 13, "Bold", ink, x + 9 + dw, cy);
        return wd;
    }
    pmui::Box(l, key + ".b", z, box, back, 8).Outline(0, A(kBlack, 0));      // (Ballest Light's edge off again after a theme switch)
    if (dot) pmui::Pic(l, key + ".d", z + 1, {x + 8, cy - 3, 6, 6}, "pm-dot.png", ink);
    pmui::Text(l, key + ".t", z + 1, text, 13, "Bold", ink, x + 9 + dw, cy);
    return wd;
}

// The on/off switch: lime with the metallic knob on the right when on; grey with a grey knob on the left when off.
// `invert`: the knob light with a dark ring, the other way round (the pink toggle: Lvcid). `trim`: Ballest Light's
// settings look, its edge a thin black trim instead of the white one (Lvcid: the white looked out of place there).
bool Toggle(Layer& l, const std::string& id, int z, double x, double y, bool on, bool locked, double k = 1, const Color* onColor = nullptr,
            bool invert = false, bool trim = false) {
    const Rect r{x, y, 54 * k, 30 * k};
    Anim& a = gAnim[id];
    if (a.knob < 0) a.knob = on ? 1 : 0;
    a.knob = ux::Approach(a.knob, on ? 1.0 : 0.0, pmui::Dt(), 0.05);
    const double t = a.knob;
    double fade = 1.0;
    if (locked) {
        // can't be changed (the plugin manager's own): blacked out, as an unclickable button, its knob still showing where it is
        pmui::Box(l, id + ".b", z + 1, r, kOffFill, r.h / 2).Outline(2, A(kAbEdge, 0.62));     // (its edge a touch lighter than the blacked-out buttons' half: Lvcid)
        const double d = 22 * k, kx = x + 4 * k + (r.w - 8 * k - d) * Smooth(t);
        pmui::Box(l, id + ".off", z + 2, {kx, y + 4 * k, d, d}, ux::Hex("#25272C"), d / 2);
        return false;
    }
    const double h = Ease(id + ".h", pmui::Over(l, r.Grow(4)));
    // (Ballest Light: off and not hovered, dimmed into the dark column; hovered, up to its full look: Lvcid)
    if (BallestLight()) fade = 1 - 0.42 * (1 - t) * (1 - h);
    const Color lit = onColor ? *onColor : kLime;
    Glow(l, id + ".glow", z, r, lit, (0.35 + 0.2 * h) * t * fade);
    ux::Node& track = pmui::Box(l, id + ".b", z + 1, r, Mix(Mix(kTileRing, ux::Hex("#3A3D42"), h), lit, t), r.h / 2);
    if (BallestLight()) {
        // (Ballest Light: a white edge round it, as the icon tiles' frame (Lvcid); or, `trim`, a thin black one)
        ux::RawColors raw;
        if (trim) track.Outline(1.3f, A(kBlack, 0.85));
        else track.Outline(static_cast<float>(1.6 + 1.0 * h), Mix(kWhite, lit, t));      // (hovered, a touch thicker; on, its edge its own colour: Lvcid)
    } else {
        // (no edge: one set in Ballest Light stayed on the widget after switching theme, white rings in Night: Lvcid)
        track.Outline(0, A(kBlack, 0));
    }
    track.Opacity(static_cast<float>(fade));
    const double d = 22 * k, kx = x + 4 * k + (r.w - 8 * k - d) * Smooth(t);
    // (Ballest Light: the knob on as off, white with its dark edge: Lvcid)
    const double kt = BallestLight() ? 0 : t;
    pmui::Box(l, id + ".off", z + 2, {kx, y + 4 * k, d, d}, kOffKnob, d / 2).Opacity(static_cast<float>((1 - kt) * fade));
    pmui::Pic(l, id + ".on", z + 3, {kx, y + 4 * k, d, d}, invert ? "pm-knob-light.png" : "pm-knob.png", kWhite).Opacity(static_cast<float>(kt * fade));
    // its edge chrome (Lvcid, 2026-10-11: as the get button's ring), rolling as the knob travels; dark as gunmetal while
    // off. A part turn of the ring for its way along: as a ball rolls.
    const double roll = (r.w - 8 * k - d) * Smooth(t) / (3.14159265 * d) * 360;
    ux::Node& chrome = pmui::Pic(l, id + ".chrome", z + 4, Rect{kx, y + 4 * k, d, d}.Grow(0.6 * k), "pm-ring-chrome-knob.png",      // (a thicker ring: the knob is small)
              Mix(Color{0.15f, 0.155f, 0.165f, 1}, invert || CurrentTheme() == 0 ? Color{0.02f, 0.02f, 0.025f, 1} : kWhite, kt));      // (Ballest Night: its white line gone, a dark ring: cleaner, Lvcid)
    chrome.Transform(1, roll);
    chrome.Opacity(static_cast<float>(fade));
    return !locked && pmui::Click(l, r.Grow(4), id);
}

// A plugin's icon on its tile. `bubble` (0..1, its card hovered: Lvcid, 2026-10-11): its border becomes the black
// bubble button's swirl (as the top bar's pages hovered), the icon's tile laid over the bubble so only its edge shows,
// a thin white edge round it.
void Tile(Layer& l, const std::string& key, int z, const Rect& r, const std::string& icon, double radius, double ring, double rowHover, bool off,
          double bubble = 0) {
    // its border a touch brighter than the other rings (Lvcid)
    const bool light = BallestLight();
    if (light) {
        // (Ballest Light: the tile dark all the way through, at rest and hovered (Lvcid: one look for the icon); its
        // frame at rest the card's lines' grey, giving way to the white splash band as it's hovered). The frame a touch
        // metallic (Lvcid: it looked flat), its grey between the tile's old frame and the card's top and bottom edges so
        // it sits with them.
        const Color frame = Mix(ux::Themed(kTrim, 3), ux::Themed(kCardEdge, 3), 0.5);
        ux::RawColors raw;
        pmui::Box(l, key + ".b", z, r, ux::Hex("#202125"), radius);
        if (bubble < 0.99) MetalOutline(l, key + ".metal", z, r, radius, static_cast<float>(ring), A(frame, 1 - bubble), 0.85, 1);
    } else {
        // (the dark themes: a touch metallic frame, a grey that lightens as its card is hovered, in place of the black
        // bubble band and its white edge (Lvcid: the black and white didn't fit))
        pmui::Box(l, key + ".b", z, r, kTile, radius);
        MetalOutline(l, key + ".metal", z, r, radius, static_cast<float>(ring), Mix(Mix(ux::Hex("#2C2F34"), ux::Hex("#393C42"), rowHover), ux::Hex("#585C63"), bubble), 0.4, 1);      // (darker, less shine: Lvcid)
    }
    if (light && bubble > 0.01) {
        const double band = ring + 2;                 // (a little wider than the ring: the swirl shows in it)
        pmui::Bubble(l, key + ".bb", z, r, radius, bubble, 2);
        {
            ux::RawColors raw;
            pmui::Box(l, key + ".in", z, r.Grow(-band), ux::Hex("#202125"), std::max(2.0, radius - band));
        }
        pmui::Box(l, key + ".edge", z, r, A(kWhite, 0), radius).Outline(1.5f, A(kWhite, 0.3 * bubble));      // (dimmed: quiet beside the lime edge, Lvcid)
    }
    Obj texture = icon.empty() ? nullptr : ux::LoadPicture(eng::Widen(icon));
    if (!texture) texture = ux::LoadPicture(eng::Widen(registry::DefaultIcon()));
    const double inset = r.w * 0.16;
    pmui::Texture(l, key + ".i", z + 1, r.Grow(-inset), texture, off ? Color{0.35f, 0.35f, 0.35f, 1} : kWhite, radius * 0.4);
}

// Mixed as the eye sees it (in sRGB, not the widgets' linear values): t 0.5 looks halfway.
Color SeenMix(const Color& a, const Color& b, double t) {
    auto ch = [t](float x, float y) {
        const double u = std::pow(std::max(0.0f, x), 1 / 2.2), v = std::pow(std::max(0.0f, y), 1 / 2.2);
        return static_cast<float>(std::pow(u + (v - u) * t, 2.2));
    };
    return {ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), a.a + (b.a - a.a) * static_cast<float>(t)};
}

// How strongly the description shows along its line, 0..1 of its width (Lvcid's curve, measured from their drawing;
// 2026-10-10: more of it revealed, its line longer): full at its start, easing down to about half, a long gentle
// slope, then gone just before its end.
double DescStrength(double x) {
    static const double kCurve[][2] = {{0, 1}, {0.04, 0.92}, {0.08, 0.82}, {0.12, 0.73}, {0.17, 0.65}, {0.23, 0.59},
                                       {0.3, 0.55}, {0.4, 0.52}, {0.52, 0.5}, {0.62, 0.47}, {0.71, 0.43}, {0.78, 0.38},
                                       {0.84, 0.3}, {0.89, 0.2}, {0.93, 0.11}, {0.965, 0.03}, {0.985, 0}, {1, 0}};
    x = std::clamp(x, 0.0, 1.0);
    for (size_t i = 1; i < std::size(kCurve); ++i)
        if (x <= kCurve[i][0]) {
            const double t = (x - kCurve[i - 1][0]) / (kCurve[i][0] - kCurve[i - 1][0]);
            return kCurve[i - 1][1] + (kCurve[i][1] - kCurve[i - 1][1]) * t;
        }
    return 0;
}

// One line of description, clipped to r; on the hovered row (with the setting on) a long one scrolls to its end, waits,
// fades out and starts again. At rest it fades along its line by Lvcid's curve (DescStrength), into `mask` (the row's
// colour), as the eye sees it; hovered, it lights to full. Drawn in narrow slices, each in its own colour.
void Description(Layer& l, const std::string& key, int z, const std::string& s, double px, const Color& c, const Rect& r, double hover,
                 double since, const Color& mask) {
    constexpr int kSlices = 36;
    const std::string first = key + ".s0";
    const double tw = Measured(l.groups.count(first) ? *l.groups[first] : l, "t", s, px);
    const bool overflow = tw > r.w;
    double offset = 0, alpha = 1;
    if (overflow && hover > 0.5 && gPrefs.marquee) {
        // a short wait, then eased: it gets going quicker than a smoothstep (a part of it straight), and slows into the end
        constexpr double kWait = 0.5;
        const double dist = tw - r.w + 18, run = dist / 50;
        const double cycle = kWait + run + 1.4 + 0.35 + 0.35;
        const double t = std::fmod(std::max(0.0, pmui::Now() - since), cycle);
        if (t < kWait) offset = 0;
        else if (t < kWait + run) {
            const double u = (t - kWait) / run;
            offset = dist * (0.4 * u + 0.6 * Smooth(u));
        } else offset = dist;
        if (t > cycle - 0.7 && t <= cycle - 0.35) alpha = 1 - (t - (cycle - 0.7)) / 0.35;
        if (t > cycle - 0.35) offset = 0, alpha = (t - (cycle - 0.35)) / 0.35;

    }
    for (int i = 0; i < kSlices; ++i) {
        const double x0 = std::round(r.w * i / kSlices), x1 = std::round(r.w * (i + 1) / kSlices);
        const double seen = DescStrength((x0 + x1) / 2 / r.w);
        Layer& g = pmui::Group(l, key + ".s" + std::to_string(i), z, {r.x + x0, r.y, x1 - x0, r.h}, true, alpha);
        pmui::Text(g, "t", 0, s, px, "Semilight", SeenMix(mask, c, seen + (1 - seen) * hover), -offset - x0, r.h / 2);
    }
}

// The scroll of an area this frame: the wheel while the pointer is on it, eased; clamped to its content.
double Scrolled(const std::string& key, const Layer& l, const Rect& view, double content) {
    Scroll& s = gS.scrolls[key];
    s.max = std::max(0.0, content - view.h);
    s.target = std::clamp(s.target - pmui::Wheel(l, view) * 110, 0.0, s.max);
    s.pos = std::clamp(ux::Approach(s.pos, s.target, pmui::Dt(), 0.06, 300), 0.0, s.max);
    return s.pos;
}

// The thin scroll bar, dragged by its thumb or clicked along its track: nulime; a box's own inside another (the disk's)
// lime (Lvcid).
void ScrollBar(Layer& l, const std::string& key, int z, const Rect& track, double view, const Color& tint = kNuLime) {
    Scroll& s = gS.scrolls[key];
    if (s.max <= 0.5) return;
    pmui::Box(l, key + ".track", z, track, A(kWhite, 0.04), track.w / 2);
    const double th = std::max(40.0, track.h * view / (view + s.max));
    const double ty = track.y + (track.h - th) * s.pos / s.max;
    const Rect thumb{track.x, ty, track.w, th};
    const double h = Ease(key + ".h", pmui::Over(l, track.Grow(6)) || pmui::Held(key));
    // (hovered or held, white, in every theme: Lvcid; Ballest Light: no black trim round it, it reads without one)
    const Color c = Mix(tint, kWhite, h);
    Glow(l, key + ".glow", z + 1, thumb, c, 0.3 + 0.2 * h);
    if (BallestLight()) {
        ux::RawColors raw;
        pmui::Box(l, key + ".thumb", z + 2, thumb, c, track.w / 2);
    } else {
        pmui::Box(l, key + ".thumb", z + 2, thumb, c, track.w / 2);
    }
    pmui::Click(l, track.Grow(6), key, false);
    if (pmui::Held(key)) {
        const double my = pmui::Mouse().y - l.oy;
        s.target = s.pos = std::clamp((my - track.y - th / 2) / std::max(1.0, track.h - th), 0.0, 1.0) * s.max;
    }
}

// --- search ------------------------------------------------------------------------------------------------------------
std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}
std::string Bare(const std::string& s) {
    std::string out;
    for (char c : s)
        if (c != ' ' && c != '-' && c != '_') out.push_back(c);
    return out;
}

// As the classic menu: every word typed somewhere in the name (the middle of a word counts, spaces left out too);
// a word that starts one of the name's words scores more. -1: not at all.
int SearchScore(const std::string& name, const std::string& query) {
    const std::string n = Lower(name), bare = Bare(n);
    int score = 0, used = 0;
    std::string word;
    for (char c : Lower(query) + " ") {
        if (c != ' ') {
            word.push_back(c);
            continue;
        }
        if (word.empty()) continue;
        ++used;
        const size_t at = n.find(word);
        if (at == std::string::npos) {
            if (bare.find(Bare(word)) == std::string::npos) return -1;
            score += 1;
        } else {
            const bool starts = at == 0 || n[at - 1] == ' ' || n[at - 1] == '-';
            score += starts ? (at == 0 ? 4 : 3) : 2;
        }
        word.clear();
    }
    return used ? score : -1;
}

// A search box's look (the field sits over it): a dark pill, a bigger magnifier on its left, and a very faint hint of
// our own while it's empty (the text box's own hint can't be dimmed that far).
const Color kHintFaint = ux::Hex("#3A3D42");
// The search box's look (Lvcid, 2026-10-10): no lime edge or glow. Resting, dark; hovered, a touch brighter than in use
// (a faint glow), its magnifier grey; in use (`t`: typing, or holding a search), the field's own grey, its magnifier
// lime; in use and hovered, the hover's brightness with the lime magnifier.
void SearchLook(Layer& l, const std::string& key, int z, const Rect& r, const std::string& hint, double px, double t, bool showHint) {
    const double hv = Ease(key + ".hv", pmui::Over(l, r), 0.08);
    if (CurrentTheme() == 1) {
        // (Ballest Light: a light search on the dark band (Lvcid: nice there); hovered a touch lighter, in use white)
        ux::RawColors raw;
        const Color fill = Mix(Mix(ux::Hex("#DCDDE0"), ux::Hex("#E9EAEC"), hv), ux::Hex("#F7F7F8"), t);
        pmui::Box(l, key + ".b", z + 1, r, fill, r.h / 2).Outline(2, Mix(Mix(ux::Hex("#9EA1A7"), ux::Hex("#C2C4C8"), hv), kWhite, t));
        const double icon = std::round(r.h * 0.5);
        pmui::Pic(l, key + ".i", z + 2, {r.x + 14, r.Cy() - icon / 2, icon, icon}, "pm-search.png", Mix(ux::Hex("#5A5D63"), ux::Hex("#1E1F23"), std::max(hv, t)));
        if (showHint) {
            const double room = r.w - (14 + icon + 12) - 14;
            if (room > 24)
                pmui::Text(l, key + ".hint", z + 2, pmui::Fit(hint, pmui::Width(hint, px, "Regular"), room), px, "Regular", ux::Hex("#6E7176"),
                           r.x + 14 + icon + 12, r.Cy());
        }
        return;
    }
    static const Color kRest = ux::Hex("#131417"), kRestEdge = ux::Hex("#1C1D21"), kHov = ux::Hex("#202227"), kHovEdge = ux::Hex("#2E3036");
    Glow(l, key + ".glow", z, r, kWhite, 0.05 * hv);
    pmui::Box(l, key + ".b", z + 1, r, Mix(Mix(kRest, kField, t), kHov, hv), r.h / 2).Outline(2, Mix(Mix(kRestEdge, kFieldEdge, t), kHovEdge, hv));
    const double icon = std::round(r.h * 0.5);
    pmui::Pic(l, key + ".i", z + 2, {r.x + 14, r.Cy() - icon / 2, icon, icon}, "pm-search.png", Mix(Mix(kHintFaint, kSub, 0.5 * hv), kLime, t));
    if (showHint) {
        const double room = r.w - (14 + icon + 12) - 14;       // (a narrow box shortens its hint, never spills it)
        if (room > 24) pmui::Text(l, key + ".hint", z + 2, pmui::Fit(hint, pmui::Width(hint, px, "Regular"), room), px, "Regular", kHintFaint, r.x + 14 + icon + 12, r.Cy());
    }
}

// --- the top bar ---------------------------------------------------------------------------------------------------------
void SetPage(const std::string& page) {
    gS.page = page;
    gS.sortOpen = false;                    // (the cosmetics sort's list: only on its page)
    OpenOverlay("");
}

// The top bar (Lvcid's layout B): the logo (drawn by Frame, over the corner), the cog (settings), then the pages; on the
// right update all and its note, the console and folder as a pair of round keys, and the close x a little apart.
constexpr double kKey = 48, kKeyGap = 12, kCloseGap = 20;

void UpdateAll(Layer& wl, double right, double cy);

// A top bar round key in Ballest Night (Lvcid, 2026-10-11): at rest black with its grey ring; hovered (`h`), as the
// pages: the ring gives way to a thin white edge round a black bubble (kBubbleOut inside it); its icon goes white.
void NightKey(Layer& wl, const std::string& key, const Rect& g, double h) {
    if (BallestLight()) {
        // (Ballest Light: a dark key at rest, its edge a mid grey, on the light bar (Lvcid); hovered, the white bubble)
        ux::RawColors raw;
        pmui::Box(wl, key + ".b", 301, g, A(ux::Hex("#1E1F23"), 1 - h), g.w / 2).Outline(3, A(ux::Hex("#6B6E74"), 1 - h));
    } else {
        pmui::Box(wl, key + ".b", 301, g, kBlack, g.w / 2).Outline(5, A(kRing, 1 - h));
    }
    if (h > 0.01) {
        pmui::Bubble(wl, key + ".nb", 301, g.Grow(kBubbleOut), g.w / 2 + kBubbleOut, h, 2);
        EdgeOutline(wl, key + ".ring", 301, g, g.w / 2, 2, A(kWhite, 0.85 * h));
    }
}

void TopBar(Layer& wl, double W) {
    pmui::Level(kBarLevel);
    const double cy = 39;
    const double folderX = W - 22 - 44 - kCloseGap - kKey, termX = folderX - kKeyGap - kKey;     // (Lvcid: console, folder, close)
    // settings: a round black key with a grey ring; white while its overlay is open
    {
        // the plugins folder: a round key, white when hovered
        const Rect r{folderX, cy - 24, kKey, kKey};
        // (growing and shrinking as the cog and the pages: HoverCurve, Lvcid)
        const double h = HoverCurve("top.folder.hov", pmui::Over(wl, r), kHoverIn, kHoverOut), grow = 1 + 0.15 * h, sz = r.w * grow;
        const Rect g{r.Cx() - sz / 2, r.Cy() - sz / 2, sz, sz};
        if (BallestNight()) NightKey(wl, "top.folder", g, h);
        else {
            Shine(wl, "top.folder", 300, g, "globe", h);
            // hovered (Lvcid): its ring and icon white, the inside only a shade lighter
            pmui::Box(wl, "top.folder.b", 301, g, Mix(kBlack, kKeyLit, h), sz / 2).Outline(5, Mix(kRing, kWhite, h));
        }
        const double icon = 24 * grow;
        if (BallestLight()) {
            // (Ballest Light: light on the dark key, dark on the white hover)
            ux::RawColors raw;
            pmui::Pic(wl, "top.folder.i", 302, {r.Cx() - icon / 2, r.Cy() - icon / 2, icon, icon}, "pm-folder.png",
                      Mix(ux::Hex("#C9CBCF"), ux::Hex("#2B2B2B"), h));
        } else {
            pmui::Pic(wl, "top.folder.i", 302, {r.Cx() - icon / 2, r.Cy() - icon / 2, icon, icon}, "pm-folder.png", Mix(kSub, kWhite, h));
        }
        if (pmui::Click(wl, r, "top.folder")) plugins::OpenFolder();
    }
    {
        const Rect r{termX, cy - 24, kKey, kKey};
        const double h = HoverCurve("top.term.hov", pmui::Over(wl, r), kHoverIn, kHoverOut), on = Ease("top.term.on", gS.overlay == "console", 0.08);
        const double grow = 1 + 0.15 * h, sz = r.w * grow;
        const Rect g{r.Cx() - sz / 2, r.Cy() - sz / 2, sz, sz};
        static bool closedHere = false;          // (as the cog: closed by a click, it fades back while still hovered)
        if (!pmui::Over(wl, r)) closedHere = false;
        const double white = std::max(HoverCurve("top.term.lit", pmui::Over(wl, r) && !closedHere, kHoverIn, kHoverOut), kOpenLit * on);
        if (BallestNight()) NightKey(wl, "top.term", g, white);
        else {
            Shine(wl, "top.term", 300, g, "globe", white);
            Glow(wl, "top.term.glow", 300, g, kWhite, 0.35 * on);
            pmui::Box(wl, "top.term.b", 301, g, Mix(kBlack, kKeyLit, white), sz / 2).Outline(5, Mix(kRing, kWhite, white));
        }
        const double icon = 24 * grow;
        pmui::Pic(wl, "top.term.i", 302, {r.Cx() - icon / 2, r.Cy() - icon / 2, icon, icon}, "pm-term.png", Mix(kNuLime, kWhite, white));
        if (pmui::Click(wl, r, "top.term")) {
            if (gS.overlay == "console") closedHere = true;
            OpenOverlay(gS.overlay == "console" ? "" : "console");
        }
    }
    // the cog and the pages, right after the logo, all three alike (Lvcid): grey at rest; hovered, white and glowing,
    // growing a little at once, shrinking back a bit slower (HoverCurve); open, #97E300, glowing (no line under them)
    constexpr double kTopUp = kHoverIn, kTopDown = kHoverOut;
    const Color topOn = kTopLime;
    double x = 106;
    {
        constexpr double sz = 52;               // (bigger: Lvcid's sketch, 2026-10-10)
        const Rect r{x - 4, cy - sz / 2 - 4, sz + 8, sz + 8};
        static bool closedHere = false;          // (closed by a click: it fades back while still hovered)
        if (!pmui::Over(wl, r)) closedHere = false;
        const double h = HoverCurve("top.cog", pmui::Over(wl, r) && !closedHere, kTopUp, kTopDown), on = Ease("top.cog.on", gS.overlay == "settings", 0.08);
        const Color lit = BallestNight() ? kWhite : topOn;     // (Ballest Night: settings open, white; Lvcid)
        const double s2 = sz * (1 + 0.08 * h);
        // (Ballest Night, hovered: a round black bubble behind it, as the pages' pill; Lvcid)
        if (BallestNight() && h > 0.01) {
            EdgeOutline(wl, "top.cog.ring", 301, r, r.w / 2, 2, A(kWhite, 0.85 * h));
            pmui::Bubble(wl, "top.cog.bub", 299, r.Grow(kBubbleOut), r.w / 2 + kBubbleOut, h, 2);
        }
        Glow(wl, "top.cog.glow", 300, {r.Cx() - s2 * 0.6, r.Cy() - s2 * 0.6, s2 * 1.2, s2 * 1.2}, Mix(Mix(kWhite, lit, on), kWhite, h), 0.3 * std::max(h, on));
        pmui::Pic(wl, "top.cog.i", 302, {r.Cx() - s2 / 2, r.Cy() - s2 / 2, s2, s2}, "pm-cog.png", Mix(Mix(kTabOff, lit, on), kWhite, h));
        if (pmui::Click(wl, r, "top.cog")) {
            closedHere = gS.overlay == "settings";
            OpenOverlay(closedHere ? "" : "settings");
        }
        x += sz + 22;
    }
    std::vector<std::pair<std::string, std::string>> pages = {{"plugins", "plugins"}};
    if (packages::KitOn()) pages.push_back({"cosmetics", "cosmetics"});
    if (gS.page == "cosmetics" && !packages::KitOn()) gS.page = "plugins";
    for (const auto& [page, label] : pages) {
        const std::string key = "top.tab." + page;
        const double tw = Measured(wl, key, label, 31);
        const Rect hit{x - 8, 12, tw + 16, 56};
        const bool on = gS.page == page;
        const double h = HoverCurve(key + ".hov", pmui::Over(wl, hit), kTopUp, kTopDown);
        // (under the console, the page you were on stays lime, dimmed; under settings (the cog lit) it's as the others:
        // Lvcid)
        const double under = Ease(key + ".under", on && gS.overlay == "console", 0.08);
        const double lit = Ease(key + ".on", on && gS.overlay != "settings", 0.08);
        if (BallestNight()) {
            // Ballest Night (Lvcid): resting, the game's charcoal; open, white (the white the hover had); hovered, a
            // black bubble button behind it, pill shaped, the word white on it
            static const Color kCharcoal = ux::Hex("#5C5F63");
            const Color rest = Mix(kCharcoal, Mix(kWhite, kCharcoal, 0.5 * under), lit);
            if (h > 0.01) {
                const Rect bub{x - 18, cy - 23, tw + 36, 46};
                // (a thin white edge round it: the bubble alone had no edge on the dark window; the bubble just
                // inside the edge: kBubbleOut; Lvcid)
                if (BallestLight()) {
                    // (Ballest Light: as Night's, not turned white: the black bubble, its white swirls, a black edge;
                    // the word white on it (Lvcid))
                    ux::RawColors raw;
                    EdgeOutline(wl, key + ".ring", 301, bub, 23, 2, A(kBlack, 0.85 * h));
                    pmui::Bubble(wl, key + ".bub", 300, bub.Grow(kBubbleOut), 23 + kBubbleOut, h, 2);
                } else {
                    EdgeOutline(wl, key + ".ring", 301, bub, 23, 2, A(kWhite, 0.85 * h));
                    pmui::Bubble(wl, key + ".bub", 300, bub.Grow(kBubbleOut), 23 + kBubbleOut, h, 2);
                }
            }
            if (BallestLight()) {
                const Color restShown = ux::Themed(rest, 1);
                ux::RawColors raw;
                pmui::Text(wl, key, 302, label, 31, "Regular", Mix(restShown, kWhite, h), x, cy);
            } else {
                pmui::Text(wl, key, 302, label, 31, "Regular", Mix(rest, kWhite, h), x, cy);
            }
        } else {
            const Color plain = Mix(kTabOff, kWhite, h);
            const Color ink = Mix(Mix(plain, Mix(topOn, ux::Hex("#3E5300"), under), lit), kWhite, h);     // (hovered: white, open or not)
            Glow(wl, key + ".glow", 301, {x - 4, cy - 17, tw + 8, 34}, Mix(Mix(kWhite, topOn, lit), kWhite, h), 0.3 * std::max(h, lit * (1 - under)));
            pmui::Text(wl, key, 302, label, 31, "Regular", ink, x, cy);
        }
        if (pmui::Entry* e = pmui::Find(wl, key)) e->node.Transform(1 + 0.08 * h, 0);      // (as the cog grows)
        const int badge = page == "plugins" ? gUpdates + (HostShown(Host()) ? 1 : 0) : 0;
        if (badge > 0 && DotShown("tab." + page + "@" + std::to_string(badge), pmui::Over(wl, hit))) {
            const Rect dot{x + tw - 6, cy - 31, 18, 18};
            Glow(wl, key + ".dotglow", 303, dot, kRed, 0.4);
            pmui::Box(wl, key + ".dot", 304, dot, kRed, 9).Outline(2, ux::Hex("#0D0E11"));
            pmui::Text(wl, key + ".n", 305, std::to_string(badge), 12, "Bold", kWhite, dot.Cx(), dot.Cy(), 0.5);
        }
        if (pmui::Click(wl, hit, key)) SetPage(page);
        x += tw + 34;
    }
    // close: a bare x (Lvcid), grey; white hovered, red while pressed, white again on release (it closes then, the x
    // white as the window fades). Clicked as a circle, not its square.
    {
        const Rect close{W - 22 - 44, cy - 22, 44, 44};
        const Rect on = pmui::OnScreen(wl, close);
        const pmui::Pointer& m = pmui::Mouse();
        const double dx = m.x - on.Cx(), dy = m.y - on.Cy();
        const bool over = pmui::Over(wl, close) && dx * dx + dy * dy <= on.w * on.w / 4;
        // hovered: lime, glowing, a little bigger; pressed: white at once, easing back to lime quickly (closed by it:
        // still lit as the window fades)
        // (the pages' curve and speeds: Lvcid)
        const double h = HoverCurve("top.close", over || (!gS.open && gClosedByX), kHoverIn, kHoverOut);
        double& flash = gAnim["top.close.press"].value;
        flash = over && pmui::Held("top.close") ? 1.0 : ux::Approach(flash, 0.0, pmui::Dt(), 0.05);
        const Color ink = Mix(Mix(kSub, kLime, h), kWhite, flash);
        const double s = 33 * (1 + 0.2 * h), gs = s * 2;     // (bigger again: Lvcid, 2026-10-10; grows more hovered: 2026-10-11)
        pmui::Pic(wl, "top.close.glow", 301, {close.Cx() - gs / 2, close.Cy() - gs / 2, gs, gs}, "pm-x-glow.png",
                  A(Mix(kLime, kWhite, flash), 0.75 * h));
        pmui::Pic(wl, "top.close.i", 302, {close.Cx() - s / 2, close.Cy() - s / 2, s, s}, "pm-x.png", ink);
        if (over && pmui::Click(wl, close, "top.close")) {
            gClosedByX = true;
            Close();                        // (the rest of the window is still drawn this frame: it fades from here)
        }
    }
    // update all and its note, left of the folder (Lvcid: on the top bar, every page)
    UpdateAll(wl, termX - 16, cy);
    // the rule under the bar
    Rule(wl, "top.rule", 299, {14, kTopH, W - 28, 3}, kRule);
}

// Debug switches (advanced settings' DEBUG group): pretend states, to see the look without anything being wrong.
bool gTestNew = false;                      // every plugin on browse shows its NEW tag
bool gTestFirstOpen = false;                // every open counts as the first since an install or update (the welcome)
// The update banner offering an update right now (shown, not "later", not updating): the corner logo replays meanwhile.
bool gBannerLive = false;
bool gBannerUpdateClicked = false;          // "update now" clicked this session: the replays stop

// The first open since this plugin manager was installed or updated (Lvcid): the welcome plays then (manager_logo.inc:
// the screen dims, the big logo animates and flies to its corner, the window comes in). Kept as the build it was last
// shown for ("manager.welcomed": the version), so an update shows it again (a rebuild of the same version doesn't: DEBUG's
// "welcome on every open" tests it); there's no separate "first ever" (an install is an update from nothing). It plays
// when the window is first opened, never by itself when the game starts.
bool gFirstOpen = false;
const std::string kBuild = plugins::kHostVersion;
bool FirstOpenNow() {
    const std::string seen = storage::Get(kSelf, "manager.welcomed", "");
    const bool first = gTestFirstOpen || seen != kBuild;
    if (first && !gTestFirstOpen) storage::Set(kSelf, "manager.welcomed", kBuild);
    hostlog::Info("manager: welcome last shown for " + (seen.empty() ? std::string("nothing yet") : seen) + "; this is " + kBuild);
    return first;
}

// The plugin manager's update started (the banner's update now / try again, update all, update plugin manager): the
// banner shows how it goes, even after "later".
void StartHostUpdate() {
    const HostNow h = Host();
    if (h.stage != HostStage::Available && h.stage != HostStage::Failed) return;
    gS.bannerLater = false;
    gBannerUpdateClicked = true;
    if (gTestHost > 0) {
        gTestHost = static_cast<int>(HostStage::Downloading);
        gTestHostAt = game::Seconds();
        hostlog::Info("manager: updating the plugin manager to 9.9.9 (pretend, from DEBUG: nothing is downloaded)");
        return;
    }
    hostlog::Info("manager: updating the plugin manager to " + h.version);
    registry::UpdateHost();
}

// --- the update banner ------------------------------------------------------------------------------------------------------
// A newer plugin manager in the registry: slides down under the top bar and pushes the page down. "later" hides it while
// it's out or its update failed; downloading and installed it stays.
double Banner(Layer& wl, double W, double y) {
    const HostNow h = Host();
    const bool test = gTestHost > 0;
    const std::string latest = h.version, state = h.stage == HostStage::Downloading ? "downloading"
                                                 : h.stage == HostStage::Installed ? "restart"
                                                 : h.stage == HostStage::Failed    ? h.error
                                                                                   : "";
    const bool forced = h.stage == HostStage::Downloading || h.stage == HostStage::Installed;
    const bool show = h.stage != HostStage::None && (!gS.bannerLater || forced);
    gBannerLive = show && h.stage == HostStage::Available && !gBannerUpdateClicked;
    gS.bannerT = ux::Approach(gS.bannerT, show ? 1.0 : 0.0, pmui::Dt(), 0.08);
    if (gS.bannerT < 0.01) return 0;
    pmui::Level(kPageLevel);
    const double t = Smooth(gS.bannerT);
    Layer& b = pmui::Group(wl, "banner", 50, {22, y + 14 - (1 - t) * 40, W - 44, 66}, false, t);
    const Rect r{0, 0, W - 44, 66};
    Glow(b, "glow", 0, r, kLime, 0.35);
    pmui::Bubble(b, "bub", 1, r, 16, 1);
    pmui::Box(b, "ico", 2, {20, 13, 40, 40}, kBlack, 20);
    pmui::Pic(b, "ico.i", 3, {29, 22, 22, 22}, state == "restart" ? "pm-refresh.png" : "pm-down.png", kLime);
    std::string title = "Plugin Manager " + latest + " is out", sub = "you have " + std::string(plugins::kHostVersion) + "  \xC2\xB7  it finishes when the game restarts";
    if (state == "downloading") title = "Updating the plugin manager to " + latest + "...", sub = "downloading and checking every file";
    if (state == "restart") title = "Plugin Manager " + latest + " is installed", sub = "restart the game to finish";
    if (state.rfind("error", 0) == 0) title = "The update did not finish", sub = state;
    // the buttons first, from the right; the words keep left of them (Lvcid: they ran under "what's new"): the line
    // after the title shortens, then goes; the title shortens last
    double x = r.R() - 12;
    if (state != "downloading" && state != "restart") {
        const std::string now = state.rfind("error", 0) == 0 ? "try again" : "update now";
        const double nw = PillWidth(b, "banner.now", now, 18, nullptr, 22);
        if (Pill(b, "banner.now", 4, {x - nw, 12, nw, 42}, now, 18, nullptr, PillKind::White, false, 12)) StartHostUpdate();
        x -= nw + 8;
        const double lw = PillWidth(b, "banner.later", "later", 17, nullptr);
        if (Pill(b, "banner.later", 4, {x - lw, 12, lw, 42}, "later", 17, nullptr, PillKind::Dark, false, 12)) gS.bannerLater = true;
        x -= lw + 4;
    }
    const double ww = PillWidth(b, "banner.new", "what's new", 17, nullptr);
    if (Pill(b, "banner.new", 4, {x - ww, 12, ww, 42}, "what's new", 17, nullptr, PillKind::Dark, false, 12))
        OpenUrl(test ? std::string(kReleases) : ReleasePage(latest));       // (it stays: the banner isn't closed)
    const double wordsEnd = x - ww - 20;
    const double titleW = pmui::Width(title, 22, "Bold"), titleMax = std::max(60.0, wordsEnd - 76);
    const double tw = pmui::Text(b, "title", 3, pmui::Fit(title, titleW, titleMax), 22, "Bold", kBlack, 76, 33);
    const double subX = 76 + std::min(tw > 0 ? tw : titleW, titleMax) + 16, subRoom = wordsEnd - subX;
    if (subRoom >= 80)
        pmui::Text(b, "sub", 3, pmui::Fit(sub, pmui::Width(sub, 17, "Semilight"), subRoom), 17, "Semilight", A(kBlack, 0.75), subX, 34);
    return t * 80;
}

// --- the plugins page --------------------------------------------------------------------------------------------------------
bool gTestError = false;                    // a pretend load error on a plugin (advanced settings)

// What went wrong with a plugin, in full: its load or run error, or an install's; "" when nothing did. (With the test on,
// the Plugin Manager's own row shows a pretend one.)
std::map<std::string, std::string> gLastError;  // a plugin's error, kept to show while it's turned off
std::string ErrorOf(const Plug& p);
// Its error to show: what's wrong now, or (turned off) the last one it had.
std::string ErrorShown(const Plug& p) {
    std::string e = ErrorOf(p);
    if (!e.empty()) gLastError[p.id] = e;
    else if (p.enabled) gLastError.erase(p.id);
    else if (gLastError.count(p.id)) e = gLastError[p.id];
    return e;
}
std::string WarningOf(const Plug& p) {
    if (gTestToggles && p.id == gTestWarnId) return "the map list came back late: the counts may be a moment old (a test warning: nothing is wrong)";
    if (!p.warned) return "";
    const auto it = gWarnText.find(p.id);
    return it == gWarnText.end() || it->second.empty() ? "it logged a warning (see the console)" : it->second;
}

std::string ErrorOf(const Plug& p) {
    if (gTestError && p.id == kSelf) return "error: main.as (12, 5): Expected ';' (a test error: nothing is wrong)";
    if (gTestToggles && p.id == gTestErrId) return "error: main.as (3, 1): No matching signatures (a test error: nothing is wrong)";
    if (p.pending.rfind("error", 0) == 0 && p.pending.rfind("error: needs host", 0) != 0) return p.pending;
    if (p.installed && p.enabled && !p.status.empty() && p.status != "running") return p.status;
    return "";
}

// `right`: where "view source" and its </> end (pinned to the card's right edge, on the facts row: Lvcid).
void Details(Layer& row, const Plug& p, const std::string& /*u*/, double x, double y, double maxW, double hover, double* bottom, double right) {
    // (a paragraph being measured again keeps its last height meanwhile: what's under it doesn't jump)
    static std::map<std::string, double> lastHeight;
    auto para = [&](const std::string& key, const std::string& text, double px, const char* face, const Color& c, double w, double guess) {
        const double ph = pmui::Para(row, key, 4, text, px, face, c, {x, y, std::floor(w), 600});
        double& last = lastHeight[p.id + "." + key];
        if (ph > 0) last = ph;
        return last > 0 ? last : guess;
    };
    // what went wrong, in full, above the description: its error (red), then its warning (gold) (Lvcid)
    if (const std::string error = ErrorShown(p); !error.empty()) {
        y += std::max(20.0, para("err.t", "[ERROR]  " + error, 14.5, "Regular", kErr, maxW, 20)) + 8;
    }
    if (const std::string warning = WarningOf(p); !warning.empty()) {
        y += std::max(20.0, para("warn.t", "[WARNING]  " + warning, 14.5, "Regular", kGold, maxW, 20)) + 8;
    }
    y += para("full", p.description.empty() ? "No description." : p.description, 17, "Semilight", Mix(kDescOpen, kBlack, hover), maxW, 52) + 16;
    std::vector<std::pair<std::string, std::string>> kv;
    if (!p.author.empty()) kv.push_back({"by", p.author});
    kv.push_back({"version", p.installed && p.update ? p.version + "  \xE2\x86\x92  " + p.latest : p.version});
    std::string needs;
    for (const std::string& n : p.needs) {
        const Plug* other = FindPlug(n);
        needs += (needs.empty() ? "" : ", ") + (other ? other->name : n);
    }
    if (!p.minHost.empty()) needs += (needs.empty() ? "host " : ", host ") + p.minHost;
    kv.push_back({"needs", needs.empty() ? "nothing else" : needs});
    // the plugin id on the right, right-justified, darker (Lvcid: as "built in")
    const double pidW = std::max(Measured(row, "pid.k", "plugin id", 14), Measured(row, "pid.v", p.id, 17));
    const double factsRight = right - pidW - 48;
    const Color factName = Mix(A(kLime, 0.8), kBlack, hover), factValue = Mix(ux::Hex("#B4B6BA"), kBlack, hover);
    double cx = x, firstY = y, line = y + 22;
    for (size_t i = 0; i < kv.size(); ++i) {
        const std::string key = "kv" + std::to_string(i);
        const double kw = Measured(row, key + ".k", kv[i].first, 14), vw = Measured(row, key + ".v", kv[i].second, 17);
        if (i > 0 && cx + kw + 10 + vw > factsRight) {   // (one that would run into the id goes on to a second line)
            cx = x;
            line += 32;
        }
        pmui::Text(row, key + ".k", 4, kv[i].first, 14, "Bold", factName, cx, line);
        // (alone on its line and still too long: shortened before the id)
        pmui::Text(row, key + ".v", 4, pmui::Fit(kv[i].second, vw, std::max(40.0, factsRight - cx - kw - 10)), 17, "Regular", factValue, cx + kw + 10, line);
        cx += kw + 10 + vw + 40;
    }
    // the servers it may connect to, on a line of their own in the warning gold (as the Classic menu's card): players see
    // where a plugin talks to
    if (!p.net.empty()) {
        std::string list;
        for (const std::string& d : p.net) list += (list.empty() ? "" : ", ") + d;
        line += 32;
        const double kw = Measured(row, "net.k", "connects to", 14);
        pmui::Text(row, "net.k", 4, "connects to", 14, "Bold", Mix(WarnAccent(), kBlack, hover), x, line);
        pmui::Text(row, "net.v", 4, pmui::Fit(list, Measured(row, "net.v", list, 17), std::max(40.0, factsRight - x - kw - 10)), 17, "Regular",
                   factValue, x + kw + 10, line);
    }
    // (the plugin id itself: Row pins it to the card's bottom right corner, at any size, opening or open: Lvcid)
    *bottom = std::max(line + 16, firstY + 44) + 14;
}

// "view source" with the </> on its right end, as one button (the Customize page's tabs' structure: the pill runs on
// under the </>, which sits on top of it; one hover lights both, one click), across `r` (Lvcid: in the button panel,
// at its bottom, as wide as its buttons).
// `settingsOf`: a click opens that plugin's settings instead of the link (`url` "" still blacks it out).
void SourceButton(Layer& l, const std::string& id, int z, const Rect& r, const std::string& url, const char* label = "view source",
                  const char* picture = "pm-code.png", double shine = 0, const std::string& settingsOf = "") {
    static const Color kCodeEdge = ux::Hex("#43474E"), kPill = ux::Hex("#40444B");
    if (url.empty()) {
        // nothing to open (no link given): blacked out, as an unclickable button
        const Rect icon{r.R() - r.h, r.y, r.h, r.h};
        const Rect pill{r.x, r.Cy() - (r.h - 4) / 2, icon.Cx() - r.x, r.h - 4};
        pmui::Box(l, id + ".b", z, pill, kOffFill, pill.h / 2).Outline(2, A(kAbEdge, 0.5));     // (its left end round: Lvcid)
        pmui::Text(l, id + ".t", z + 2, label, 16, "Regular", A(kSub, 0.3), (pill.x + icon.x) / 2, r.Cy(), 0.5);
        pmui::Box(l, id + ".i.b", z + 4, icon, kOffFill, 12).Outline(2, A(kAbEdge, 0.6));
        const double s = std::string(picture) == "pm-play.png" ? 22 : std::string(picture) == "pm-cog.png" ? 22 : 16;
        pmui::Pic(l, id + ".i", z + 5, {icon.Cx() - s / 2, icon.Cy() - s / 2, s, s}, picture, A(kSub, 0.25));
        return;
    }
    const double kIcon = r.h, kPillH = r.h - 4;
    const Rect icon{r.R() - kIcon, r.y, kIcon, kIcon};
    const Rect pill{r.x, r.Cy() - kPillH / 2, icon.Cx() - r.x, kPillH};
    const double h = Ease(id, pmui::Over(l, r));
    // (its left end round; hovered, as the top bar's pages: a black bubble inside a thin white edge, the words white:
    // Lvcid)
    pmui::Box(l, id + ".b", z, pill, A(kPill, 1 - h), kPillH / 2).Outline(0, A(kWhite, 0));      // (no edge: the blacked-out one's had one)
    if (h > 0.01) {
        pmui::Bubble(l, id + ".blk", z + 1, pill.Grow(kBubbleOut), kPillH / 2 + kBubbleOut, h, 2);
        EdgeOutline(l, id + ".ring", z + 1, pill, kPillH / 2, 2, A(kWhite, 0.85 * h));
    }
    pmui::Text(l, id + ".t", z + 2, label, 16, "Regular", Mix(kTabHover, kWhite, h), (pill.x + icon.x) / 2, r.Cy(), 0.5);
    // the icon end: the settings key's hover (Lvcid): white with a white glow, growing from its middle quickly and
    // shrinking back a little slower
    const double w = EaseUpDown(id + ".grow", pmui::Over(l, r), 0.05, 0.12);
    const double grow = 1 + 0.15 * w, sz = kIcon * grow;
    const Rect g{icon.Cx() - sz / 2, icon.Cy() - sz / 2, sz, sz};
    Glow(l, id + ".glow", z + 3, g, kWhite, 0.4 * w + 0.12 * shine);
    Shine(l, id + ".shine", z + 3, g, "slot", 0.3 * shine * (1 - w));      // (a little shine on the icon while open)
    pmui::Box(l, id + ".i.b", z + 4, g, Mix(kButtonFill, kWhite, w), 12 * grow).Outline(2, Mix(kCodeEdge, kWhite, w));
    // (the triangle a touch bigger; the cog bigger still: Lvcid)
    const double s = (std::string(picture) == "pm-play.png" ? 24 : std::string(picture) == "pm-cog.png" ? 25 : 18) * grow;
    pmui::Pic(l, id + ".i", z + 5, {icon.Cx() - s / 2, icon.Cy() - s / 2, s, s}, picture, Mix(kSub, kBlack, w));
    if (pmui::Click(l, r, id)) {
        if (!settingsOf.empty()) OpenSettingsOf(settingsOf);
        else OpenUrl(url);
    }
}

std::string StatusText(const Plug& p, ChipKind* kind) {
    *kind = ChipKind::Plain;
    if (!p.pending.empty() && p.pending.rfind("error", 0) != 0) {       // installing, removing (an error: ErrorOf's)
        *kind = ChipKind::Off;
        return p.pending;
    }
    if (!p.installed) return "";
    if (!p.enabled) return *kind = ChipKind::Off, "off";
    if (p.status != "running") return *kind = ChipKind::Error, p.status;
    if (p.update) return *kind = ChipKind::Update, "update " + p.latest;
    if (p.essential) return *kind = ChipKind::Built, "built in";
    return *kind = ChipKind::Run, "running";
}

// Browse rows: the get button sits left of the row's card, in a black tab of its own whose top and bottom lines run in
// from the left into the card's rounded corners (Lvcid's sketch). The tab grows down with the card when it opens; the
// button stays at the top.
constexpr double kGetTab = 84;             // (room round the toggle on its left: Lvcid)

void TabLines(Layer& list, const std::string& u, const Rect& tab, double /*cardRadius*/, double hover) {
    Color edge = Mix(kCardEdge, kHoverEdge, hover);
    // (Ballest Light: on the dark column, the lines a mid grey, the same all along)
    std::optional<ux::RawColors> raw;
    if (BallestLight()) {
        edge = Mix(ux::Hex("#3A3C41"), ux::Hex("#5A5D63"), hover);
        raw.emplace();
    }
    // (the card's left end is a pill now: the lines stop short of it, fading out, not running on over its curve; Lvcid)
    constexpr double kIn = 30, kOut = 34, kShort = 2;      // (a longer, softer fade into the pill, ending by its tip: Lvcid)
    const double reach = tab.w - kShort;
    for (const auto& [key, y] : {std::pair<const char*, double>{"t", tab.y}, {"b", tab.B() - 2}}) {
        pmui::Box(list, u + "tab." + key, 1, {tab.x + kIn, y, reach - kIn - kOut, 2}, edge);
        pmui::Pic(list, u + "tab." + key + ".fade", 1, {tab.x, y, kIn, 2}, "pm-fade-right.png", edge);
        pmui::Pic(list, u + "tab." + key + ".fade2", 1, {tab.x + reach - kOut, y, kOut, 2}, "pm-fade-left.png", edge);
    }
}

// A get that can't be had yet (it needs a newer plugin manager), in the square b: the get button's circle, blacked out
// (Lvcid, 2026-10-11): near black inside, its chrome ring dark as gunmetal, a dim lock; hovered, the ring and the lock
// catch a little light and `tip` says why; pressed, it gives a short shake, as a locked door does.
// (the get button's chrome, darker for the dark theme: linear 0.42, about 67% grey; Lvcid)
const Color kChromeDim{0.42f, 0.43f, 0.45f, 1};
constexpr double kChromeTurn = 24;          // the chrome rings turn this many degrees a second (Lvcid: slowly)

// A chrome ring in r, its light turning round it at `degreesPerSecond`. Not a picture rotated (that shimmered and shook:
// Lvcid): the light pre-turned in 16 frames (pm-ring-chrome-00..15.png, generated: the script is kept outside this
// repo), cross-faded; the ring itself stays still, its box snapped to whole units.
void ChromeRing(Layer& l, const std::string& key, int z, const Rect& r, const Color& tint, double degreesPerSecond) {
    constexpr int kFrames = 16;
    const Rect at{std::round(r.x), std::round(r.y), std::round(r.w), std::round(r.h)};
    const double f = std::fmod(pmui::Now() * degreesPerSecond / 360.0, 1.0) * kFrames;
    const int f0 = static_cast<int>(f) % kFrames, f1 = (f0 + 1) % kFrames;
    char name[32];
    std::snprintf(name, sizeof name, "pm-ring-chrome-%02d.png", f0);
    pmui::Pic(l, key + ".a", z, at, name, tint);
    std::snprintf(name, sizeof name, "pm-ring-chrome-%02d.png", f1);
    pmui::Pic(l, key + ".b", z, at, name, A(tint, f - std::floor(f)));      // (over it, fading in: no dip between)
}
void LockedCircle(Layer& l, const std::string& key, int z, const Rect& b, const std::string& tip) {
    static std::map<std::string, double> triedAt;
    const Rect hit = b.Grow(b.w * 0.08);
    const double h = Ease(key + ".lock", pmui::Over(l, hit), 0.08);
    const double since = pmui::Now() - triedAt[key];
    const double shake = since < 0.32 ? std::sin(since * 70) * 3.2 * (1 - since / 0.32) : 0;
    const double gs = b.w * 1.16;
    const Rect g{b.Cx() - gs / 2 + shake, b.Cy() - gs / 2, gs, gs};
    pmui::Box(l, key + ".b", z, g, Mix(ux::Hex("#08090B"), ux::Hex("#101114"), h), gs / 2).Outline(1, A(kBlack, 0.9));
    // (linear: about 33% grey, 45% hovered; turning, slower: it's locked)
    ChromeRing(l, key + ".ring", z + 1, g.Grow(3.5), Mix(Color{0.085f, 0.088f, 0.095f, 1}, Color{0.16f, 0.165f, 0.175f, 1}, h), kChromeTurn * 0.4);
    pmui::Pic(l, key + ".i", z + 2, g.Grow(-gs * 0.27), "pm-lock.png", Mix(ux::Hex("#3E4146"), ux::Hex("#5E6168"), h));
    if (pmui::Click(l, hit, key)) triedAt[key] = pmui::Now();
    if (!tip.empty()) Tip(l, key, hit, tip);
}

// The get button in a square b (browse rows: in the right panel, where installed rows have remove).
void GetButton(Layer& list, const Plug& p, const std::string& u, const Rect& b, int z) {
    const double s = b.w;
    if (p.installed || p.pending == "installing") {
        // got: a quiet check; getting: the arrows turning. Round as the get button (its own keys: a box keeps the
        // shape it was first drawn with, and the lock's circle and this one shared one before)
        const double gs = s * 1.16;
        const Rect g{b.Cx() - gs / 2, b.Cy() - gs / 2, gs, gs};
        pmui::Box(list, u + "got.b", z, g, kButtonFill, gs / 2).Outline(2, kAbEdge);
        const bool busy = p.pending == "installing";
        pmui::Pic(list, u + "got.i", z + 1, g.Grow(-gs * 0.27), busy ? "pm-refresh.png" : "check.png", busy ? kSub : A(kLime, 0.75))
            .Transform(1, busy ? std::fmod(pmui::Now() * 300, 360) : 0);
        return;
    }
    // it needs a newer plugin manager: a lock, why on hover (Lvcid: before the click, not an error after it)
    if (const std::string need = NeedsHost(p.id); !need.empty()) {
        LockedCircle(list, u + "lockget", z, b, LockReason(need));
        return;
    }
    // hovered, as the settings cog: it grows a little from its middle, turns white and shines. A circle, a little bigger
    // than the square it has, a thin white edge round it (Lvcid, 2026-10-11, a try)
    const Rect hit = b.Grow(s * 0.08);
    const double h = HoverCurve(u + "get", pmui::Over(list, hit), kHoverIn, kHoverOut);      // (the pages' curve and speeds: Lvcid)
    const double k = 1 + 0.09 * h, gs = s * 1.16 * k;
    const Rect g{b.Cx() - gs / 2, b.Cy() - gs / 2, gs, gs};
    // (round, as the button: the globe's shine, a round glow; the rounded square ones showed round it: Lvcid)
    pmui::Pic(list, u + "get.glow", z, g.Grow(gs * 0.35), "pm-glow-round.png", A(kLime, 0.35 * (1 - h)));      // (round, centred: Lvcid)
    pmui::Bubble(list, u + "get.bub", z + 1, g, gs / 2, 1);
    pmui::Box(list, u + "get.w", z + 2, g, A(kWhite, h), gs / 2);
    // (its edge chrome, grey-white and metallic: the pm-ring-chrome frames; Lvcid)
    ChromeRing(list, u + "get.ring", z + 2, g.Grow(3.5), Mix(kChromeDim, kWhite, 0.25 * h), kChromeTurn);       // (turning slowly round it; darker, a touch lighter hovered: Lvcid)
    // (its hover shine over the ring, Lvcid; only as it nears full size)
    Shine(list, u + "get.shine", z + 3, g, "globe", Smooth((h - 0.65) / 0.35));
    pmui::Pic(list, u + "get.i", z + 3, g.Grow(-gs * 0.23), "pm-down.png", kBlack);      // (the arrow a little bigger: Lvcid)
    if (pmui::Click(list, hit, u + "get")) Get(p);
}

// NEW: the letters in ballest's pink, a thin pink edge glow on them only (Lvcid): faint copies a hair out on each side.
void NewLetters(Layer& l, const std::string& key, int z, double cx, double cy, double px, double alpha = 1) {
    const double breathe = 0.5 + 0.5 * std::sin(pmui::Now() * 1.6), d = std::max(0.8, px * 0.07);
    const double off[4][2] = {{-d, 0}, {d, 0}, {0, -d}, {0, d}};
    for (int i = 0; i < 4; ++i)
        pmui::Text(l, key + ".e" + std::to_string(i), z, "NEW", px, "Bold", A(kPink, alpha * (0.22 + 0.1 * breathe)), cx + off[i][0], cy + off[i][1], 0.5);
    pmui::Text(l, key, z + 1, "NEW", px, "Bold", A(kPink, alpha), cx, cy, 0.5);
}

bool gCosRows = false;                      // the cosmetics page is drawing the plugins page's rows now (its packs)

void GetTab(Layer& list, const Plug& p, const std::string& u, const Rect& tab, double cardRadius, double cy, double hover, bool compact) {
    TabLines(list, u, tab, cardRadius, hover);
    // what it is, in the space the installed page's toggle has (Lvcid): a plugin's puzzle piece, a cosmetic pack's
    // symbol; grey, lime while its card is hovered. New: just NEW there instead, a little bigger
    if (p.fresh || gTestNew) {
        NewLetters(list, u + "new", 4, tab.x + tab.w / 2, tab.y + cy, compact ? 15.5 : 18);
        return;
    }
    const double is = compact ? 28 : 32;
    pmui::Pic(list, u + "kind", 3, {tab.x + tab.w / 2 - is / 2, tab.y + cy - is / 2, is, is}, gCosRows ? "pm-pack.png" : "pm-puzzle.png",
              Mix(ux::Hex("#4A4D52"), kLime, hover));
}

// Installed rows: the on/off toggle in the left tab.
void ToggleTab(Layer& list, const Plug& p, const std::string& u, const Rect& tab, double cardRadius, double cy, double hover, bool compact) {
    TabLines(list, u, tab, cardRadius, hover);
    const double k = (compact ? 0.85 : 1) * 0.92;      // (ever so slightly smaller: Lvcid, 2026-10-11)
    // on but not working right: gold for warnings, red for errors (Lvcid)
    const bool error = p.enabled && !ErrorOf(p).empty();
    const bool warned = p.enabled && (p.warned || (gTestToggles && p.id == gTestWarnId));
    const Color* tint = error ? &ErrAccent() : warned ? &WarnAccent() : nullptr;
    if (Toggle(list, u + "tog", 3, tab.x + tab.w / 2 - 27 * k, tab.y + cy - 15 * k + 1, p.enabled, p.essential, k, tint)) {
        hostlog::Info(std::string("manager: turn ") + (p.enabled ? "off " : "on ") + p.id);
        plugins::SetEnabled(p.id, !p.enabled);
        gModelAt = -100;
    }
}

// Right of the card: a tab like the left one (its lines run out of the card's corners and fade to the right), with the
// plugin's links as bigger buttons (its GitHub page, its source) and, installed, remove (red and a little bigger when
// hovered). The same width on every row, so the cards line up.
// (Lvcid's sizes: 46 buttons 11 apart, the first 24 in from the panel's round edge)
constexpr double kLinkButton = 46, kLinkGap = 11, kLinkStart = -6;
constexpr double kLinkTab = kLinkStart + kLinkButton + kLinkGap + kLinkButton + 36, kLinkTabInstalled = kLinkTab + kLinkGap + kLinkButton;
constexpr double kPanelRound = 30;          // the button panel's round edge, over the card's right end
// The panel, open: its three buttons as always, then the joined buttons (40 tall, 20 under them, 14 apart), as wide as
// the three.
constexpr double kJoinedH = 40, kJoinedGap = 14, kJoinedUnder = kJoinedGap;     // (even gaps between the rows: Lvcid)
constexpr double kStackH = kLinkButton + kJoinedUnder + kJoinedH, kStackEnds = 16;
// The top of the panel's three buttons (Lvcid: open, they come down into a stack with the joined buttons, centred).
double ButtonsTop(double top, double height, double cy, double open) {
    const double closed = top + cy - kLinkButton / 2;
    const double opened = top + std::max(kStackEnds, (height - kStackH) / 2);
    return closed + (opened - closed) * Smooth(open);
}
const Color kRemoveRed = ux::Hex("#E8344E");

void LinkTab(Layer& list, const Plug& p, const std::string& u, const Rect& tab, double /*cardRadius*/, double cy, double hover, double open = 0) {
    // The panel mirrors the left side (Lvcid): its round edge faces the card (curving the other way to the card's
    // corner, a small gap between), and its far end is open, its top and bottom lines fading out as the left tab's do.
    const Color edge = Mix(kTrim, kTrimHover, 0.6 * hover);
    constexpr double kRound = kPanelRound;
    constexpr double kFade = 40;
    const double px = tab.x - kRound, pr = tab.R(), fadeAt = pr - kFade;    // over the card's last kRound units
    pmui::BoxCorners(list, u + "links.round", 3, {px, tab.y, kRound * 2, tab.h}, kLinkPanel, kRound, 0, 0, kRound).Outline(2, edge);
    pmui::Box(list, u + "links.fill", 3, {px + kRound, tab.y + 2, fadeAt - px - kRound, tab.h - 4}, kLinkPanel);
    pmui::Pic(list, u + "links.fillfade", 3, {fadeAt, tab.y + 2, kFade, tab.h - 4}, "pm-fade-left.png", kLinkPanel);
    if (BallestLight()) {
        // (Ballest Light: its lines the same darker grey as its round edge, fading out evenly along their whole length
        // to nothing at the panel's right end: Lvcid's pick)
        const Color line = ux::Themed(edge, 3);
        ux::RawColors raw;
        for (const auto& [key, y] : {std::pair<const char*, double>{"t", tab.y}, {"b", tab.B() - 2}})
            pmui::Pic(list, u + "links." + key + ".even", 4, {px + kRound, y, pr - px - kRound, 2}, "pm-fade-left.png", line);
    } else {
        for (const auto& [key, y] : {std::pair<const char*, double>{"t", tab.y}, {"b", tab.B() - 2}}) {
            pmui::Box(list, u + "links." + key, 4, {px + kRound, y, fadeAt - px - kRound, 2}, edge);
            pmui::Pic(list, u + "links." + key + ".fade", 4, {fadeAt, y, kFade, 2}, "pm-fade-left.png", edge);
        }
    }
    // installed: its GitHub page, its folder, then remove at the far end; browse: its source, its GitHub page, get
    // (always these three, open or not: Lvcid)
    double x = tab.x + kLinkStart;
    const double y = ButtonsTop(tab.y, tab.h, cy, open);
    // open: a slight, slow shimmer along the panel's round edge (Lvcid: subtle)
    // (in bands, so it fades in where it meets the top and out going down towards the middle: Lvcid, softer)
    if (const double t = Smooth(open); t > 0.01) {
        const double glint = 0.05 + 0.035 * (0.5 + 0.5 * std::sin(pmui::Now() * 1.7));
        constexpr double kBand = 4, kIn = 12, kReach = 72;
        for (int i = 0; i * kBand < std::min(tab.h, kReach); ++i) {
            const double y0 = i * kBand, mid = y0 + kBand / 2;
            const double fade = Smooth(mid / kIn) * (1 - Smooth((mid - kIn) / (kReach - kIn)));
            if (fade < 0.01) continue;
            Layer& sh = pmui::Group(list, u + "links.shimmer" + std::to_string(i), 4, {px - 2, tab.y + y0, kRound + 6, kBand}, true);
            pmui::BoxCorners(sh, "e", 0, {2, -y0, kRound * 2, tab.h}, A(kWhite, 0), kRound, 0, 0, kRound).Outline(2, A(kWhite, glint * t * fade));
        }
    }
    if (p.installed) {
        // (its GitHub page first, then its folder next to remove: the world's files, mine, the void: Lvcid)
        if (p.home.empty()) OffButton(list, u + "github", 5, {x, y, kLinkButton, kLinkButton}, "pm-source.png");
        else if (IconButton(list, u + "github", 5, {x, y, kLinkButton, kLinkButton}, "pm-source.png", 0, 13, nullptr, 0, &kButtonFill)) OpenUrl(p.home);
        x += kLinkButton + kLinkGap;
        if (IconButton(list, u + "folder", 5, {x, y, kLinkButton, kLinkButton}, "pm-folder.png", 0, 13, nullptr, 0, &kButtonFill))
            OpenDir(plugins::FolderOf(p.id).empty() ? plugins::Dir() + L"\\" + eng::Widen(p.id) : plugins::FolderOf(p.id));
        x += kLinkButton + kLinkGap;
        const bool busy = p.pending == "installing" || p.pending == "removing";
        const Rect rb{x, y, kLinkButton, kLinkButton};
        if (p.essential || busy) {
            // can't be removed (built in, or busy): the button dimmed, no hover
            OffButton(list, u + "remove", 5, rb, "pm-trash.png");
        } else if (IconButton(list, u + "remove", 5, rb, "pm-trash.png", 0, 13, &kRemoveRed, 0.08, &kButtonFill)) {
            gS.popup = "remove";
            gS.popupId = p.id;
            gS.forget = false;
        }
        return;
    }
    // browse: its source, its GitHub page, then get in remove's place
    if (p.page.empty()) OffButton(list, u + "code", 5, {x, y, kLinkButton, kLinkButton}, "pm-code.png");
    else if (IconButton(list, u + "code", 5, {x, y, kLinkButton, kLinkButton}, "pm-code.png", 0, 13, nullptr, 0, &kButtonFill)) OpenUrl(p.page);
    x += kLinkButton + kLinkGap;
    if (p.home.empty()) OffButton(list, u + "github", 5, {x, y, kLinkButton, kLinkButton}, "pm-source.png");
    else if (IconButton(list, u + "github", 5, {x, y, kLinkButton, kLinkButton}, "pm-source.png", 0, 13, nullptr, 0, &kButtonFill)) OpenUrl(p.home);
    x += kLinkButton + kLinkGap;
    GetButton(list, p, u, {x, y, kLinkButton, kLinkButton}, 5);
}

// The hover's lime bar on a card's left edge: it grows from the middle to the card's length, its glow breathing slowly,
// with light soft particles drifting off it to the left. `R` the card in the list's units.
// The lit edge's colour: lime, or (a plugin that's on but not quite right) its toggle's warning gold or error red.
Color gAccent = kLime;
Color AccentPale() { return gAccent.r == kLime.r && gAccent.g == kLime.g ? ux::Hex("#F2FFC8") : Mix(gAccent, kWhite, 0.7); }
Color AccentCore() { return gAccent.r == kLime.r && gAccent.g == kLime.g ? ux::Hex("#E4FF8A") : Mix(gAccent, kWhite, 0.45); }

// How far in a rounded box's left edge is, `d` units down from its top (or up from its bottom) round a corner of `r`.
double CornerInset(double r, double d) {
    if (d >= r) return 0;
    return r - std::sqrt(std::max(0.0, r * r - (r - d) * (r - d)));
}

// Draws `draw` all over `extent` except inside `hole`, a box with round left corners of `rad` (a pill's: half its
// height), and inside `also` when given (a box over the hole's edge: a card's icon tile), in l's units (Lvcid,
// 2026-10-11: a hover's glow and smoke round its button, never over it). In slices: above and below the hole the whole
// width; beside it, strips that stop at its left edge (each at the strip's innermost point, so none covers it) or at
// `also`'s, whichever is nearer. `draw` gets each slice and where the hole's top left is in it; it draws in the hole's
// units.
void AroundPill(Layer& l, const std::string& key, int z, const Rect& extent, const Rect& hole, double rad,
                const std::function<void(Layer&, double, double)>& draw, const Rect* also = nullptr) {
    int n = 0;
    auto slice = [&](const Rect& r) {
        if (r.w <= 0.5 || r.h <= 0.25) return;
        Layer& g = pmui::Group(l, key + ".s" + std::to_string(n++), z, r, true);
        draw(g, hole.x - r.x, hole.y - r.y);
    };
    // a band from y0 to y1 whose open part runs from the extent's left to `right`: `also` cut out of it
    auto band = [&](double y0, double y1, double right) {
        y0 = std::max(y0, extent.y);
        y1 = std::min(y1, extent.B());
        if (y1 - y0 <= 0.25) return;
        if (also && also->x < right && y0 < also->B() && y1 > also->y) {
            // (split where `also` starts and ends, so only the rows it's in are cut)
            if (also->y > y0) {
                slice({extent.x, y0, right - extent.x, also->y - y0});
                y0 = also->y;
            }
            const double y2 = std::min(y1, also->B());
            slice({extent.x, y0, std::min(right, also->x) - extent.x, y2 - y0});
            if (also->R() < right) slice({also->R(), y0, right - also->R(), y2 - y0});
            if (y2 < y1) slice({extent.x, y2, right - extent.x, y1 - y2});
            return;
        }
        slice({extent.x, y0, right - extent.x, y1 - y0});
    };
    if (hole.y > extent.y) band(extent.y, hole.y, extent.R());
    if (extent.B() > hole.B()) band(hole.B(), extent.B(), extent.R());
    const double r = std::min(rad, hole.h / 2);
    constexpr int kStrips = 9;
    for (int i = 0; i < kStrips; ++i) {
        const double y0 = r * i / kStrips, y1 = r * (i + 1) / kStrips, in = CornerInset(r, y1);
        band(hole.y + y0, hole.y + y1, hole.x + in);
        band(hole.B() - y1, hole.B() - y0, hole.x + in);
    }
    if (hole.h > 2 * r) band(hole.y + r, hole.B() - r, hole.x);
}

void Accent(Layer& list, const std::string& u, const Rect& R, double radius, bool over, const Rect* tile = nullptr) {
    // it lights like a light turning on: in from dim, a brief whiter flare as it comes on, settling to lime; unhovered,
    // it fades out where it is (Lvcid, 2026-10-11: not shrinking back)
    Anim& lamp = gAnim[u + "accent.on"];
    const double before = lamp.value;
    const double on = EaseUpDown(u + "accent.on", over, 0.09, 0.2), lit = Smooth(on);
    const double flare = on > before ? std::sin(3.14159265 * on) : 0;
    // its reveal up and down the edge: as the top bar's pages grow, the same curve and speed (Lvcid, 2026-10-11); it
    // stays full while it fades, and only goes back once it's gone
    const double grow = HoverCurve(u + "accent.len", over || lit > 0.01, kHoverIn, kHoverOut);
    gAnim[u + "accent.grow"].value = grow;          // (for AccentTrim)
    if (lit < 0.01) return;
    // The light: the card's left edge lit (Lvcid's drawing), any size of card. A thin lime rim just outside the card's
    // trim, the card's own shape a little bigger under it (so it follows the rounded corners exactly), a lighter core
    // next to the card; shown on the card's left side and round its corners a little way. Brightest in the middle,
    // softer to its ends: bands from the middle out, each a little longer and fainter, laid over each other. It shows
    // from the middle outward as it grows (the side first, the corners last). Its glow: soft, to the left mostly.
    const double now = pmui::Now(), cy = R.Cy(), rim = 1.4;     // (the card's own trim is lit too: AccentTrim)
    const double len = std::max(4.0, (R.h + 2 * rim) * grow);
    const Rect bar{R.x - 3, cy - len / 2, 4, len};                  // (where the particles and sparkles come from)
    const double breathe = 0.5 + 0.5 * std::sin(now * 1.7);
    // Left of the edge, within the card's own top and bottom (Lvcid): its glow, filling most of that height, and a
    // soft lime smoke drifting away left; the particles too (below), all clipped to it. Inside the card: 3 at most.
    // (the card's pill cut out of them: they wrap round its left end, never over the card: Lvcid)
    constexpr double kReach = 170;
    const double gh = std::max(len, R.h * 0.85 * grow) * 1.35;
    struct Puff {
        double x, y, size, alpha;
    };
    std::vector<Puff> puffs;
    const double frames = std::fmod(now / 7.3, 1.0) * 32;
    const int f0 = static_cast<int>(frames) % 32;
    const double frac = frames - std::floor(frames);
    auto drawSide = [&](Layer& side, double dx, double dy) {
        // (in the card's units, dx / dy: where its top left is in this slice)
        pmui::Pic(side, "glow", 0, {dx - 110, dy + R.h / 2 - gh / 2, 220, gh}, "fx-glow.png", A(gAccent, (0.5 + 0.25 * breathe) * lit + 0.3 * flare));
        // the smoke: two frames of the hover effect's smoke cross-faded (it drifts left as it plays)
        char name[32];
        for (int k = 0; k < 2; ++k) {
            std::snprintf(name, sizeof name, "fx-smoke%02d.png", (f0 + k) % 32);
            pmui::Pic(side, "smoke" + std::to_string(k), 1, {dx - 165, dy + R.h * 0.04, 168, R.h * 0.92}, name,
                      A(gAccent, 0.4 * lit * (k == 0 ? 1 - frac : frac)));
        }
        for (size_t i = 0; i < puffs.size(); ++i) {
            const Puff& p = puffs[i];
            pmui::Pic(side, "p" + std::to_string(i), 2, {dx + p.x - p.size / 2, dy + p.y - p.size / 2, p.size, p.size}, "fx-puff.png",
                      A(Mix(gAccent, kWhite, 0.35), p.alpha));
        }
    };
    // the rim, in bands
    const Color line = Mix(gAccent, AccentPale(), 0.6 * flare), core = Mix(AccentCore(), kWhite, 0.5 * flare);
    constexpr int kBands = 10;
    const double reach = radius + 2;
    // (outside the card only along its side and just into the corners: round them the card's own lit trim carries
    // the line, so it never stands proud of the border there: Lvcid)
    const double outsideHalf = std::min(len / 2, R.h / 2 - radius * 0.55);
    for (int i = 0; i < kBands; ++i) {
        const double half = outsideHalf * (i + 1) / kBands;
        if (half <= 0) continue;
        const std::string key = u + "edge.b" + std::to_string(i);
        // (under the card: only what's outside it shows)
        Layer& band = pmui::Group(list, key, 1, {R.x - rim - 1, cy - half, reach + rim + 1, 2 * half}, true);
        const double ox = R.x - rim - 1, oy = cy - half;            // the band's origin: the boxes in its units
        const double a = 0.22 * lit;
        pmui::Box(band, "rim", 0, {R.x - rim - ox, R.y - rim - oy, R.w + 2 * rim, R.h + 2 * rim}, A(line, a), radius + rim);
        pmui::Box(band, "core", 1, {R.x - rim * 0.45 - ox, R.y - rim * 0.45 - oy, R.w + rim * 0.9, R.h + rim * 0.9}, A(core, a), radius + rim * 0.45);
    }
    // sparkles along it while it's lit: tiny white points flickering on and off up and down the line
    for (int i = 0; i < 5; ++i) {
        const double period = 0.55 + 0.13 * i, t = now / period + i * 0.37, local01 = t - std::floor(t);
        const unsigned seed = static_cast<unsigned>(std::floor(t)) * 2246822519u + static_cast<unsigned>(i) * 3266489917u;
        const double r1 = (seed % 1000) / 1000.0;
        const double sy = bar.y + 8 + (bar.h - 16) * r1, size = 10 * (0.7 + 0.6 * ((seed / 1000) % 100) / 100.0);
        const double tw = std::sin(3.14159265 * local01);
        const double ex = R.x + CornerInset(radius, std::min(sy - R.y, R.B() - sy));      // (on the pill's curve)
        pmui::Pic(list, u + "spark" + std::to_string(i), 10, {ex - 1.5 - size / 2, sy - size / 2, size, size}, "fx-spark.png",
                  A(Mix(kWhite, gAccent, 0.3), (tile && tile->Grow(2).Has(ex, sy) ? 0 : 1) * 0.85 * tw * tw * lit));     // (none on the tile)
    }
    // the particles: each on its own loop, a fresh height every time round (from a hash of the loop), drifting left
    // and a little up, in and out softly
    for (int i = 0; i < 7; ++i) {
        const double period = 1.5 + 0.11 * ((i * 37) % 7);
        const double t = now / period + i * 0.143;
        const double local = t - std::floor(t);
        const unsigned seed = static_cast<unsigned>(std::floor(t)) * 2654435761u + static_cast<unsigned>(i) * 40503u;
        const double r1 = (seed % 1000) / 1000.0, r2 = ((seed / 1000) % 1000) / 1000.0;
        const double py = bar.y + 6 + (bar.h - 12) * r1 - 10 * local;
        const double px = bar.x - 2 - (60 + 90 * r2) * local;      // well away to the left
        const double size = 10 * (1 - 0.3 * local) * (0.8 + 0.4 * r2);
        const double alpha = std::sin(3.14159265 * local) * 0.55 * lit;   // (the lime smoke off the line)
        puffs.push_back({px - R.x, py - R.y, size, alpha});
    }
    // (within the card's own top and bottom, as before; left of it)
    // (and the icon's tile; over the left tab's toggle and tags (Lvcid: the smoke and particles drift over them))
    AroundPill(list, u + "edge.glow", 8, {R.x - kReach, R.y, kReach, R.h}, R, radius, drawSide, tile);
}


// A test of the update tag (Lvcid): the Plugin Manager's row offers "update 0.2.0" that only plays the animations.
bool gTestUpdate = false, gTestFailed = true;          // (off by default; it alternates: the first click succeeds)
double gTestBusyUntil = -100, gTestHiddenUntil = -100;

// Shown while it pretends to update, and again once its pause after that is over.
bool TestUpdate(const Plug& p) {
    const double now = pmui::Now();
    return gTestUpdate && p.id == kSelf && !p.update && (now < gTestBusyUntil || now > gTestHiddenUntil);
}

// The spinner while an update downloads: eight dots round a circle, the lit one going round.
void Spinner(Layer& l, const std::string& key, int z, double cx, double cy, double size, const Color& c) {
    const double t = pmui::Now() * 1.4, rad = size * 0.38, dot = size * 0.2;
    for (int i = 0; i < 8; ++i) {
        const double a = i / 8.0 * 6.28318530718;
        double f = std::fmod(i / 8.0 - t, 1.0);
        if (f < 0) f += 1;
        const double lit = 1 - f;
        pmui::Box(l, key + std::to_string(i), z, {cx + std::cos(a) * rad - dot / 2, cy + std::sin(a) * rad - dot / 2, dot, dot},
                  A(c, 0.2 + 0.8 * lit * lit), dot / 2);
    }
}

// The title row's "update x.y.z": the update button itself. At rest the lime bubble with a soft white shine; hovered it
// grows and shines stronger (as the settings cog), turns white, and a download icon slides out on its right, widening
// it; a click or leaving puts the icon away at once. While it downloads: a spinner in the icon's place. Returns its
// width at rest (for laying out the row).
// While busy: black, the spinner and its words in lime and white. `failed` (0..1, fading): red, "update failed".
// The top bar's update all (Lvcid, 2026-10-10): `iconLeft` (every mode: it's pinned on its right) its icon slides out on
// its left and the words stay put; `keyLook` (as "update plugin manager") black with a grey ring as the top bar's round
// keys, its words nulime; hovered, the same as the others (white, growing, the shine round it).
double UpdatePill(Layer& row, const std::string& key, double x, double cy, const std::string& label, bool busy, double k, bool* clicked,
                  bool fromRight = false, double failed = 0, bool keyLook = false, bool iconLeft = false) {
    const double px = 14.5 * k, hgt = 28 * k, icon = px * 1.45;
    const double tw = Measured(row, key + ".t", label, px), restW = tw + 22 * k;
    Anim& slide = gAnim[key + ".icon"];
    const double hitW = restW + (icon + 6 * k) * slide.value;
    const Rect hit{fromRight ? x - hitW : x, cy - hgt / 2, hitW, hgt};
    const bool over = pmui::Over(row, hit) && !busy;
    const double hv = EaseUpDown(key, over, 0.12, 0.18);
    static std::map<std::string, double> clickedAt;
    const bool quiet = pmui::Now() - clickedAt[key] < 0.35;          // just clicked: the icon stays away
    slide.value = ux::Approach(slide.value, (over && !quiet) || busy ? 1.0 : 0.0, pmui::Dt(), over ? 0.08 : 0.04);
    const double grow = 1 + 0.08 * hv, wd = (restW + (icon + 6 * k) * Smooth(slide.value)) * grow, ht = hgt * grow;
    const Rect r{fromRight ? x - wd : x, cy - ht / 2, wd, ht};
    const double dark = EaseUpDown(key + ".busy", busy, 0.06, 0.1);
    if (failed < 0.01) Shine(row, key + ".shine", 4, r, "pill", (keyLook ? hv : 0.3 + 0.7 * Smooth((hv - 0.4) / 0.6)) * (1 - dark));
    else Glow(row, key + ".fail.glow", 4, r, kRemoveRedGlow, 0.45 * failed);
    if (keyLook) pmui::Box(row, key + ".k", 5, r, kBlack, ht / 2).Outline(static_cast<float>(3.5 * k), Mix(kRing, kWhite, hv));
    // (keyLook, at rest: the quiet grey bubble drifting over the black, as "view source" shows it: Lvcid)
    // (inside its ring, rounded as the pill is: the ring shows all the way round)
    if (keyLook) pmui::Bubble(row, key + ".qb", 5, r.Grow(-3.5 * k), hgt / 2 - 3.5 * k, (1 - hv) * (1 - dark), true);
    else pmui::Bubble(row, key + ".bub", 5, r, ht / 2, 1);
    pmui::Box(row, key + ".w", 6, r, A(kWhite, hv), ht / 2);
    pmui::Box(row, key + ".dark", 6, r, A(kBlack, dark), ht / 2).Outline(static_cast<float>(1.5 * dark), A(kLime, 0.6 * dark));
    pmui::Box(row, key + ".fail", 6, r, A(kRemoveRedGlow, failed), ht / 2);
    // busy: the spinning wheel first, its words after it; else the words, and the download icon sliding out after them
    // (iconLeft: the words from its right end, the icon or spinner on its left)
    const double textX = iconLeft ? r.R() - (11 * k + tw) * grow : busy ? r.x + (9 * k + icon + 5 * k) * grow : r.x + 11 * k * grow;
    const Color base = keyLook ? Mix(ux::Hex("#CFD1D5"), kBlack, hv) : kBlack;     // (keyLook: a soft white at rest: Lvcid)
    const Color ink = Mix(Mix(base, kWhite, dark), kWhite, failed);
    pmui::Text(row, key + ".t", 7, label, px, "Bold", ink, textX, cy);      // (its size fixed: re-sized each frame it twitched)
    const double ix = iconLeft ? r.x + 10 * k * grow : r.x + (11 * k + tw + 6 * k) * grow;
    if (busy) Spinner(row, key + ".spin", 7, r.x + (9 * k + icon / 2) * grow, cy, icon * grow, Mix(kBlack, kLime, dark));
    else if (slide.value > 0.02)
        pmui::Pic(row, key + ".i", 7, {ix, cy - icon * grow / 2, icon * grow, icon * grow}, "pm-down.png", A(base, Smooth(slide.value)));
    *clicked = !busy && pmui::Click(row, hit, key);
    if (*clicked) {
        clickedAt[key] = pmui::Now();
        slide.value = 0;
    }
    return restW;
}

void Tip(Layer& pane, const std::string& key, const Rect& hit, const std::string& text, const Color* ink, double width,
         bool alignRight);

// A soft white light running round a pill's edge with a short fading trail (Lvcid: "awaiting game restart" catches the
// eye with motion, nothing louder). One lap every 2.4 s.
void RunningLight(Layer& l, const std::string& key, int z, const Rect& r) {
    constexpr double kPi = 3.14159265358979, kPeriod = 2.4;
    const double rad = r.h / 2, straight = std::max(0.0, r.w - r.h), around = 2 * straight + 2 * kPi * rad;
    auto at = [&](double s, double* px, double* py) {
        double d = (s - std::floor(s)) * around;
        if (d < straight) return void((*px = r.x + rad + d, *py = r.y));
        d -= straight;
        if (d < kPi * rad) {
            const double a = -kPi / 2 + d / rad;
            return void((*px = r.R() - rad + rad * std::cos(a), *py = r.Cy() + rad * std::sin(a)));
        }
        d -= kPi * rad;
        if (d < straight) return void((*px = r.R() - rad - d, *py = r.B()));
        d -= straight;
        const double a = kPi / 2 + d / rad;
        *px = r.x + rad + rad * std::cos(a);
        *py = r.Cy() + rad * std::sin(a);
    };
    const double t = std::fmod(pmui::Now() / kPeriod, 1.0);
    constexpr int kTrail = 14;
    for (int i = 0; i < kTrail; ++i) {
        double px = 0, py = 0;
        at(t - i * 0.0075, &px, &py);
        const double a = std::pow(1 - static_cast<double>(i) / kTrail, 1.8), sz = 5.5 - 2.5 * i / kTrail;
        pmui::Box(l, key + std::to_string(i), z, {px - sz / 2, py - sz / 2, sz, sz}, A(kWhite, 0.85 * a), sz / 2);
    }
    double hx = 0, hy = 0;
    at(t, &hx, &hy);
    pmui::Pic(l, key + ".halo", z, {hx - 14, hy - 14, 28, 28}, "fx-glow.png", A(kWhite, 0.5));
}

// A pill that can't be pressed (Lvcid): dark, its words a light grey, a lock before them when `lock`; hovered, `tip`
// (when given) as a tooltip; `light`: the running light round it. Its rect in `drawn`; returns its width.
double LockedPill(Layer& l, const std::string& key, double x, double cy, const std::string& label, double k, bool fromRight, bool lock,
                  bool light, const std::string& tip, Rect* drawn = nullptr) {
    const double px = 14.5 * k, hgt = 28 * k, icon = px * 1.05;
    const double tw = Measured(l, key + ".t", label, px), wd = tw + 22 * k + (lock ? icon + 6 * k : 0);
    const Rect r{fromRight ? x - wd : x, cy - hgt / 2, wd, hgt};
    pmui::Box(l, key + ".off", 6, r, kOffFill, hgt / 2).Outline(2, A(kAbEdge, light ? 1.0 : 0.7));
    double tx = r.x + 11 * k;
    if (lock) {
        pmui::Pic(l, key + ".lock", 7, {tx, cy - icon / 2, icon, icon}, "pm-lock.png", A(kSub, 0.8));
        tx += icon + 6 * k;
    }
    pmui::Text(l, key + ".t", 7, label, px, "Bold", light ? ux::Hex("#A4A7AE") : A(kSub, 0.62), tx, cy);
    if (light) RunningLight(l, key + ".light", 8, r);
    if (!tip.empty()) Tip(l, key, r, tip);
    if (drawn) *drawn = r;
    return wd;
}

// An update, unless it's a DEBUG pretend one (nothing to download: it only says so).
void InstallUpdate(const Plug& p) {
    if (gTestUpdateIds.count(p.id)) {
        hostlog::Info("manager: update " + p.id + " (pretend, from DEBUG: nothing is downloaded)");
        return;
    }
    hostlog::Info("manager: update " + p.id);
    registry::Install(p.id);
}

double UpdateTag(Layer& row, const std::string& u, double x, double cy, const std::string& version, bool busy, double k, bool* clicked,
                 double failed = 0) {
    return UpdatePill(row, u + "upd", x, cy, busy ? "updating..." : failed > 0.01 ? "update failed" : "update " + version, busy, k, clicked, false,
                      failed);
}

// A row of the installed or browse list (thick or compact), and its details when open. `whole` is in the list's units:
// the left tab (toggle or get), the card, the right tab (its links, and remove).
// The lit edge on the card's own trim (inside the card, under its icon and text), so the light sits flush on the
// border, not just outside it (Lvcid): in the same bands as Accent's rim, from the middle out, the ends fading along
// the edges. `row` is the card's own group.
void AccentTrim(Layer& row, const std::string& u, double wd, double ht, double radius, const Color& back) {
    const double grow = gAnim[u + "accent.grow"].value, lit = Smooth(gAnim[u + "accent.on"].value);
    if (lit < 0.01) return;
    constexpr double kTrimW = 2;
    const double len = std::max(4.0, ht * grow), cy = ht / 2, reach = radius + 2;
    constexpr int kBands = 10;
    for (int i = 0; i < kBands; ++i) {
        const double half = len / 2 * (i + 1) / kBands;
        // (just over the card's own box, under its hover light: the light shows over it, no dark patch)
        Layer& band = pmui::Group(row, "trim.b" + std::to_string(i), 0, {0, cy - half, reach, 2 * half}, true);
        const double oy = cy - half;
        pmui::Box(band, "lime", 0, {0, -oy, wd, ht}, A(gAccent, 0.22 * lit), radius);
        pmui::Box(band, "fill", 1, {kTrimW, kTrimW - oy, wd - 2 * kTrimW, ht - 2 * kTrimW}, back, radius - kTrimW);
    }
    const double ends = lit * Smooth((grow - 0.92) / 0.08);
    if (ends > 0.01)
        for (const auto& [key, y] : {std::pair<const char*, double>{"t", 0.0}, {"b", ht - kTrimW}})
            pmui::Pic(row, std::string("trim.end.") + key, 0, {reach, y, 34, kTrimW}, "pm-fade-left.png", A(gAccent, 0.22 * ends));
}

// The cosmetics page draws the plugins page's rows for the cosmetic packs (manager_cosmetics.inc): these from there.
std::string PackSymbol(const std::string& id);     // an owned pack's symbol: its slot's when all of it is one slot, else the pack
double PackItems(Layer& row, const std::string& u, const std::string& id, double x0, double y, double right);

void Row(Layer& list, const Plug& p, const Rect& whole, double baseH, bool compact, bool browse) {
    const std::string id = std::string(gCosRows ? "c" : "") + (browse ? "b." : "i.") + p.id, u = id + ".";
    Anim& a = gAnim["row." + id];
    const double right = kLinkTabInstalled;                  // (three slots on either tab: the cards line up)
    const Rect R{whole.x + kGetTab, whole.y, whole.w - kGetTab - right, whole.h};     // the card, between its tabs
    const bool over = pmui::Over(list, R);
    if (over && !a.was) a.since = pmui::Now();
    a.was = over;
    a.value = ux::Approach(a.value, over ? 1.0 : 0.0, pmui::Dt(), kRowEase, 0.4);
    // (open, once it's all but there, is exactly there: what grows with it stops moving, Lvcid: the facts wiggled)
    const double h = Smooth(a.value), open = a.open > 0.995 ? 1.0 : a.open < 0.005 ? 0.0 : Smooth(a.open);
    const bool off = p.installed && !p.enabled && !browse;
    const bool clicked = pmui::Click(list, R, "row." + id);       // before the row's own buttons, which win
    // (its left end a pill, every list and both views: half the closed row's height, kept as it opens; its lit edge
    // and the tabs' lines follow it: Lvcid, 2026-10-11)
    const double radius = baseH / 2, k = compact ? 0.85 : 1;
    Glow(list, "glow." + id, 1, R, kWhite, 0.035 * h);
    if (BallestLight()) Glow(list, "lift." + id, 1, R.Moved(0, 4), kBlack, 0.32 * h);      // (Ballest Light: it lifts off the panel)
    if (browse) GetTab(list, p, u, {whole.x, whole.y, kGetTab, whole.h}, radius, baseH / 2, h, compact);
    else ToggleTab(list, p, u, {whole.x, whole.y, kGetTab, whole.h}, radius, baseH / 2, h, compact);
    LinkTab(list, p, u, {R.R(), whole.y, right, whole.h}, radius, baseH / 2, h, a.open);
    // open: under the three buttons, "view source", a joined button as wide as the three (Lvcid; "watch vid" went with
    // the registry's video links: AnythingGoes)
    // (in a group clipped to the panel's inside: revealed as the panel grows open, hidden again as it closes)
    const double span = 3 * kLinkButton + 2 * kLinkGap;
    if (a.open > 0.05) {
        const double shine = Smooth((a.open - 0.6) / 0.4);
        const Rect area{R.R() + kLinkStart - 24, whole.y + 2, span + 48, std::max(0.0, whole.h - 4)};
        Layer& jl = pmui::Group(list, u + "joined", 8, area, true);
        const double jy = ButtonsTop(whole.y, whole.h, baseH / 2, a.open) + kLinkButton + kJoinedUnder - area.y;
        const Rect jb{24, jy, span, kJoinedH};
        // (Lvcid: "settings" for now, where "view source" was: its settings; blacked out when it has none to open)
        SourceButton(jl, u + "d.settings", 0, jb, !browse && p.installed && p.settings ? "settings" : "", "settings", "pm-cog.png", shine, p.id);
    }
    // its lit edge and effects in its toggle's colour: lime, or gold for a warning, red for an error (Lvcid)
    gAccent = kLime;
    if (!browse && p.enabled) {
        if (!ErrorOf(p).empty()) gAccent = ErrAccent();
        else if (p.warned || (gTestToggles && p.id == gTestWarnId)) gAccent = WarnAccent();
    }
    // (the icon's tile, where Row draws it below (closed: over the card's ends; open: at its top, grown): cut out of the
    // lit edge's glow and smoke too, Lvcid)
    const double tileS0 = compact ? 62 : 102, tileS = tileS0 * (1 + 0.226 * open);
    // (hovered, the tile pops: a few units bigger, evenly from its middle, on the pages' curve; Lvcid)
    const double tilePop = 3 * HoverCurve(u + "tile.pop", over, kHoverIn, kHoverOut);
    const Rect tileAt = Rect{R.x + (!browse ? 14 + (compact ? 30 : 35) + 10 : 18.0), R.y + baseH / 2 - tileS0 / 2, tileS, tileS}.Grow(tilePop);
    Accent(list, u, R, radius, over, &tileAt);
    Layer& row = pmui::Group(list, "row." + id, 2, R, true);
    const double wd = R.w, cy = baseH / 2;
    const Color back = BallestLight() ? Mix(off ? kOffRow : kCard, kWhite, 0.6 * h) : off ? kOffRow : kCard;     // (Ballest Light: hovered, brighter)
    // square on the right: the button panel's round edge covers that end, so only its curve shows (Lvcid)
    pmui::BoxCorners(row, "bg", 0, {0, 0, wd, R.h}, back, radius, 0, 0, radius).Outline(2, Mix(gS.expanded == p.id ? kOpenEdge : kTrim, kTrimHover, 0.6 * h));
    // hovered, its left side lit on its trim (just over its box); the trim's shine: a faint light along its top
    AccentTrim(row, u, wd, R.h, radius, back);
    gAccent = kLime;                        // (back to lime for the rest)
    if (BallestLight()) {
        // (Ballest Light: a white line catching the light along the card's top, its edge a darker grey: contrast)
        ux::RawColors raw;
        pmui::Box(row, "trim", 1, {radius, 1.5, wd - radius, 1.2}, A(kWhite, 0.75 + 0.2 * h), 0.6);
    } else {
        pmui::Box(row, "trim", 1, {radius, 1.5, wd - radius, 1}, A(kWhite, 0.07 + 0.05 * h), 0.5);
    }
    // hovered: a soft light in from the left edge (no bubbles: their swirl was too busy, Lvcid); its edge is lit lime (Accent)
    pmui::Pic(row, "sheen", 1, {0, 0, std::min(220.0, wd * 0.3), R.h}, "pm-fade-left.png", A(kWhite, 0.07 * h));
    // The left (both views, Lvcid 2026-10-10): the cog (installed), where the arrow was (no arrow: a click on the card
    // opens it), always there (muted and still when there's nothing to set), then the icon. The right end: the status
    // pills right to left; the text keeps left of them.
    double ax = wd - 18 - kPanelRound;      // (the button panel covers the card's last kPanelRound units)
    const double fullRight = ax - 14;       // (the description's line is under the pills': it can run on under them)
    const double cs = compact ? 30 : 35;    // (a tad bigger: Lvcid)
    const double lead = !browse ? 14 + cs + 10 : 18;     // where the icon starts (browse / explore: nothing in the cog's spot)
    bool keyClicked = false;
    if (gCosRows && !browse) {
        // the cosmetics page, owned (Lvcid): the pack's symbol exactly where a plugin's cog is, bare: grey at rest, lime
        // (no halo) while the card is hovered, a little bigger hovered itself. All of the pack one slot (ball, hat, ...):
        // that slot's symbol; several slots: the pack's (known once it's installed and has run). A click opens the card
        const Rect hit{14 - 3, cy - cs / 2 - 4, cs + 6, cs + 8};
        const double ch2 = EaseUpDown(u + "pk", pmui::Over(row, hit), 0.08, 0.14), gs = cs * (1 + 0.1 * Smooth(ch2));
        const Rect ir{hit.Cx() - gs / 2, cy - gs / 2, gs, gs};
        pmui::Pic(row, "pk", 4, ir, PackSymbol(p.id).c_str(), Mix(Mix(ux::Hex("#62656B"), kLime, h), kWhite, 0.3 * ch2));
        keyClicked = pmui::Click(row, hit, u + "pk");
    } else if (!browse) {
        const double cogX = 14;
        if (!p.settings) {
            // nothing to set: muted, no hover of its own; a touch lighter than it was on the right (the card's left is a
            // shade lighter), and lighter still while the card is hovered (its sheen lights that end: Lvcid)
            if (BallestLight()) {
                // (Ballest Light: faint, barely off the card: nothing to set (Lvcid: it couldn't be told from one that has))
                ux::RawColors raw;
                pmui::Pic(row, "cog", 4, {cogX, cy - cs / 2, cs, cs}, "pm-cog.png", Mix(ux::Hex("#B9BBBF"), ux::Hex("#AEB0B4"), h));
            } else {
                pmui::Pic(row, "cog", 4, {cogX, cy - cs / 2, cs, cs}, "pm-cog.png", Mix(ux::Hex("#2B2E33"), ux::Hex("#373A3F"), h));   // (hovered: a touch dimmer, Lvcid)
            }
        } else {
            // a bare icon (Lvcid): dark grey, white and a little bigger when hovered
            const Rect hit{cogX - 3, cy - cs / 2 - 4, cs + 6, cs + 8};
            const double ch2 = EaseUpDown(u + "cog", pmui::Over(row, hit), 0.08, 0.14), gs = cs * (1 + 0.1 * Smooth(ch2));
            if (BallestLight()) {
                // (Ballest Light: dark, clearly there; darker on a hovered card, near black hovered itself: it darkens
                // where the dark themes brighten (white on the light card was lost: Lvcid))
                ux::RawColors raw;
                pmui::Pic(row, "cog", 4, {hit.Cx() - gs / 2, cy - gs / 2, gs, gs}, "pm-cog.png",
                          Mix(Mix(ux::Hex("#45484E"), ux::Hex("#2E3035"), h), ux::Hex("#0E0F11"), ch2));
            } else {
                pmui::Pic(row, "cog", 4, {hit.Cx() - gs / 2, cy - gs / 2, gs, gs}, "pm-cog.png", Mix(Mix(ux::Hex("#62656B"), ux::Hex("#868990"), h), kWhite, ch2));   // dark grey at rest, lighter on a hovered card (Lvcid)
            }
            if (pmui::Click(row, hit, u + "cog")) OpenSettingsOf(p.id);
        }
    }
    // the pills: running / off rightmost (none when it failed to run), then its error, then its warning; turned
    // off, those two stay, blacked out somewhat. Compact: in the row's middle; big rows: on the title's line (Lvcid)
    if (!browse) {
        const double py = compact ? cy : cy - 16;
        ChipKind rk;
        std::string rs = StatusText(p, &rk);
        if (rk == ChipKind::Update || rk == ChipKind::Built) rs = p.enabled ? "running" : "off", rk = p.enabled ? ChipKind::Run : ChipKind::Off;
        const bool failed = p.enabled && p.installed && p.pending.empty() && !ErrorOf(p).empty() && p.status != "running";
        if (failed) rs.clear();
        // (error / warning: hovered, a compact tooltip with the message, as the open card shows it: Lvcid)
        auto chip = [&](const std::string& key, const std::string& text, ChipKind kind, double fade, const std::string& tip = "") {
            const double cw = Measured(row, key + ".t", text, 13) + 18 + (kind == ChipKind::Run ? 11 : 0);
            ax -= cw;
            Chip(row, key, 4, ax, py, text, kind, h, fade);
            const Rect pr{ax, py - 11, cw, 22};
            // (the tooltips' own: on gFx, its delay; its right edge at the pill's)
            if (!tip.empty()) Tip(row, u + key, pr, tip, kind == ChipKind::Error ? &kErr : &kGold, 380, true);
            ax -= 8;
        };
        const double fade = p.enabled ? 1.0 : 0.4;
        if (!rs.empty()) chip("chip", rs, rk, 1);
        if (const std::string e = ErrorShown(p); !e.empty()) chip("chip.err", "error", ChipKind::Error, fade, "[ERROR]  " + e);
        if (const std::string w = WarningOf(p); !w.empty()) chip("chip.warn", "warning", ChipKind::Warn, fade, "[WARNING]  " + w);
        ax -= 4;
    }
    // "by <author>" left of the pills (both tabs, both views; Lvcid): "by" in the quiet colour, the author a step lighter;
    // it fades as the card opens (the open card has it in its facts). When the card is too narrow for it and the title
    // (name at least 120, version, update), it gives way first.
    const double ts0 = compact ? 62 : 102, ts = ts0 * (1 + 0.226 * open);
    const double infoX = lead + ts + (compact ? 14 : 18);
    const bool testTag = !browse && TestUpdate(p);
    const double titleNeed = 120 + Measured(row, "ver", p.version, compact ? 14 : 16) + 12 +
                             (!browse && (p.update || testTag) ? pmui::Width("update " + p.latest, 14.5 * k, "Bold") + 22 * k + 40 : 0);
    const double byShow = 1 - open;
    const double byWidth = p.author.empty() ? 0 : Measured(row, "by", p.author, compact ? 14 : 15) + 6 + Measured(row, "by.k", "by", compact ? 14 : 15) + 14;
    if (!p.author.empty() && byShow > 0.01 && ax - byWidth - 14 - infoX >= titleNeed) {
        const double bpx = compact ? 14 : 15, py = compact ? cy : cy - 16;
        const double aw = Measured(row, "by", p.author, bpx), kw = Measured(row, "by.k", "by", bpx);
        ax -= aw;
        pmui::Text(row, "by", 4, p.author, bpx, "Semilight", A(Lift(ux::Hex("#45484D"), h), byShow), ax, py);
        ax -= 6 + kw;
        pmui::Text(row, "by.k", 4, "by", bpx, "Semilight", A(Lift(kCogOff, h), byShow), ax, py);
        ax -= 14;
    }
    const double textRight = ax - 14;
    const bool test = !browse && TestUpdate(p);
    const std::string dotKey = "upd." + p.id + "@" + (test ? std::string("test") : p.latest);     // (its red dot: on the update pill)
    // the left (both views, Lvcid): after the cog, the icon a touch bigger than the card, drawn over its edges; open, it
    // grows down and right from its top left corner (which stays put), the text moving right with it
    Tile(list, u + "tile", 6, Rect{R.x + lead, R.y + cy - ts0 / 2, ts, ts}.Grow(tilePop), p.icon, (compact ? 14 : 22) * ts / ts0, 3, 0, off, h);     // (hovered: its edge the black bubble's)
    // the title row: name, version (muted), then its tags in order: built in, its status, the update button. All of it
    // clipped before the settings button.
    const Color ink = Lift(off ? kOffName : ux::Hex("#F2F3F5"), h);   // (white lifts to full white)
    // (compact, open: the name grows a little with the icon, its top staying put: it grows down and right)
    // (open: the name grows, mostly downward and a little up, even past the card's top edge: Lvcid; in half steps)
    const double titleGrow = (compact ? 11 : 13) * open;
    const double nameY = compact ? cy : cy - 17, npx = std::round(((compact ? 19 : 25) + titleGrow) * 2) / 2, small = compact ? 14 : 16;
    ChipKind kind;
    std::string status = StatusText(p, &kind);
    if (browse && p.installed && status.empty()) status = "installed", kind = ChipKind::Run;
    if (kind == ChipKind::Update || kind == ChipKind::Built) status = p.enabled ? "running" : "off", kind = p.enabled ? ChipKind::Run : ChipKind::Off;
    const bool updateTag = !browse && (p.update || test);
    const bool busy = p.pending == "installing" || (test && pmui::Now() < gTestBusyUntil);
    // (on the list, not clipped by the card: a grown name may rise past its top edge; clipped on the right, before the
    // pills and the cog)
    Layer& title = pmui::Group(list, u + "title", 7, {R.x + infoX, R.y + nameY - 44, std::max(40.0, textRight - infoX), 88}, true);
    double tagsW = Measured(title, "ver", p.version, small) + 12;
    if (!status.empty() && browse) tagsW += Measured(title, "chip.t", status, 13) + 18 + 11 + 12;
    if (updateTag) tagsW += Measured(title, u + "upd.t", busy ? "updating..." : "update " + (test ? std::string("0.2.0") : p.latest), 14.5 * k) + 22 * k + 40;
    const double ty = 44 + titleGrow * 0.3;
    const double maxName = std::max(60.0, textRight - infoX - tagsW);
    const double nw = Measured(title, "name", p.name, npx);
    // (compact, open: lime, fading in and out with its growing: Lvcid)
    const double drawnW = pmui::Text(title, "name", 0, pmui::Fit(p.name, nw, maxName), npx, "Regular", Mix(ink, kLime, open), 0, ty);
    double lx = std::min(drawnW, maxName) + 12;
    lx += pmui::Text(title, "ver", 0, p.version, small, "Semilight", Lift(kVerMuted, h), lx, ty + 2) + 12;
    // (browse: its status in the title, after the name; installed rows have theirs on the right)
    const bool statusFirst = !compact;
    if (statusFirst && !status.empty() && browse) lx += Chip(title, "chip", 0, lx, ty + 1, status, kind, h) + 12;
    if (updateTag) {
        // a failure: the install was running and now is an error (or the test's turn to fail): red for a few seconds
        Anim& was = gAnim[u + "upd.was"];
        const bool erred = p.pending.rfind("error", 0) == 0 || (test && gTestFailed && pmui::Now() >= gTestBusyUntil);
        if (was.was && !busy && erred) was.since = pmui::Now();
        was.was = busy;
        const double sinceFail = pmui::Now() - was.since;
        const double failed = was.since > 0 && sinceFail < 3.2 ? std::min(1.0, sinceFail / 0.15) * (1 - Smooth((sinceFail - 2.6) / 0.6)) : 0;
        bool go = false;
        const std::string need = test ? "" : NeedsHost(p.id);
        double uw = 0;
        if (!need.empty() && !busy) {
            // (the tooltip on the list: the title is clipped to its line)
            Rect tr;
            uw = LockedPill(title, u + "upd", lx, ty + 1, "update " + p.latest, k, false, true, false, "", &tr);
            Tip(list, u + "upd.lock", tr.Moved(R.x + infoX, R.y + nameY - 44), LockReason(need));
        } else {
            uw = UpdateTag(title, u, lx, ty + 1, test ? "0.2.0" : p.latest, busy, k, &go, failed);
        }
        {
            const double dd = 12;
            const Rect dr{lx - 3, ty + 1 - 15 * k - 3, dd, dd};
            if (DotShown(dotKey, pmui::Over(title, {lx, ty + 1 - 15 * k, uw, 30 * k}))) {
                Glow(title, "upd.dot.glow", 6, dr.Grow(-2), kRed, 0.5);
                pmui::Box(title, "upd.dot", 7, dr, kRed, dd / 2).Outline(2, kCard);
            }
            // (its width now: hovered, it grows and its icon slides out on its right; what follows moves with it)
            const double slide = 14.5 * k * 1.45 + 6 * k;
            lx += (uw + slide * Smooth(gAnim[u + "upd.icon"].value)) * (1 + 0.08 * gAnim[u + "upd"].value) + 12;
        }
        if (go && test) {
            // pretend: a spinner a moment, then (every other time) it fails, red a moment; or it's gone a moment
            gTestFailed = !gTestFailed;
            gTestBusyUntil = pmui::Now() + 2.6;
            gTestHiddenUntil = gTestFailed ? gTestBusyUntil : gTestBusyUntil + 3;
            hostlog::Info("manager: test update tag clicked (nothing is updated)");
        } else if (go) {
            InstallUpdate(p);
            gModelAt = -100;
        }
    }
    if (!statusFirst && !status.empty() && browse) lx += Chip(title, "chip", 0, lx, ty + 1, status, kind, h) + 12;
    // the description: one line (scrolling when long, on the hovered row), or all of it when the row is open
    const double descX = compact ? infoX + std::min(lx, textRight - infoX) + 4 : infoX;
    const double descW = std::max(40.0, std::min(1000.0, (compact ? textRight : fullRight) - descX));
    const std::string desc = p.description.empty() ? "No description." : p.description;
    if (open < 0.5 && !compact)          // (compact: no description; it reads in the open row: Lvcid)
        Description(row, "desc", 4, desc, compact ? 15 : 16.5, Lift(kDescLine, h), {descX, compact ? cy - 12 : cy + 7, descW, 24}, h,
                    a.since, back);
    if (a.open > 0.01 || gS.expanded == p.id) {
        double bottom = 0;
        // (its width: as it is fully open, from the start: the icon growing moves infoX, but the text wraps once, not
        // again at every step of the opening: Lvcid)
        const double infoOpen = lead + ts0 * 1.226 + (compact ? 14 : 18);
        Details(row, p, u, infoX, (compact ? baseH - 4 : cy + 9) + titleGrow, std::min(1000.0, fullRight - 10 - infoOpen), 0, &bottom, wd - kPanelRound - 12);
        const double pidW = std::max(Measured(row, "pid.k", "plugin id", 14), Measured(row, "pid.v", p.id, 17));
        if (gCosRows) bottom = PackItems(row, u, p.id, infoX + 16, bottom + 4, wd - kPanelRound - 24 - pidW - 24) + 10;     // (what's in it)
        // the plugin id: the card's bottom right corner, always (Lvcid), fading in as it opens
        const double pidX = wd - kPanelRound - 12, idA = Smooth(std::clamp(a.open * 2 - 0.6, 0.0, 1.0));
        pmui::Text(row, "pid.k", 4, "plugin id", 14, "Semilight", A(kCogOff, idA), pidX, R.h - 45, 1);
        pmui::Text(row, "pid.v", 4, p.id, 17, "Regular", A(kCogOff, idA), pidX, R.h - 22, 1);
        const double panelNeed = kStackH + 2 * kStackEnds;
        gOpenExtra[id] = std::max({0.0, bottom - baseH, panelNeed - baseH});
    }
    if (clicked || keyClicked) {
        const bool opening = gS.expanded != p.id;
        gS.expanded = opening ? p.id : "";
        // a NEW one opened: seen, for good (kept by its id: an update doesn't bring its NEW back)
        if (opening && browse && p.fresh) {
            storage::Set(kSelf, "manager.opened." + p.id, "1");
            gModelAt = -100;
        }
    }
}

// --- the search: one for the page, both its tabs ------------------------------------------------------------------------
FieldRef gInstalledField;

// From `left` to `right`: the search (filtering the list as it's typed: name, author, description). No sort (Lvcid).
void BrowseTools(Layer& wl, double cy, double left, double right) {
    const double sx = left;
    const Rect sr{sx, cy - 20, std::max(44.0, right - sx), 40};      // (it narrows before anything overlaps it)
    // (one search for the page, both its tabs: its results are both, under INSTALLED / OWNED and AVAILABLE: Lvcid)
    const std::string fk = "isearch";
    std::string& query = gS.installedSearch;
    double& readAt = gS.installedReadAt;
    bool& wasFocused = gS.installedFocused;
    FieldRef& field = gInstalledField;
    field = pmui::Field(wl, fk + ".field", 16, {sr.x + 46, sr.y + 4, sr.w - 56, sr.h - 8}, "", 16,
                        CurrentTheme() == 1 ? ux::Hex("#1E1F23") : kWhite);       // (Ballest Light: dark words in its light box)
    const bool focused = Focused(field);
    const double t = Ease(fk, focused || !query.empty());
    SearchLook(wl, fk, 13, sr, gCosRows ? "search cosmetics" : "search plugins", 16, t, query.empty() && !focused);
    const double now = pmui::Now();
    if (field && ((focused && now - readAt > 0.12) || (wasFocused && !focused))) {
        readAt = now;
        query = w::ReadText(field);
    }
    wasFocused = focused;
}

Rect gSlideFrom, gSlideNow;                 // installed / browse: where the lime box set off from, and where it is

// The page's sub bar: installed / browse, the counts, update all, the folder or refresh, the views.

void SubBar(Layer& wl, double W, double top) {
    const double cy = top + kSubH / 2;
    // installed / browse, on a small strip (the Customize page's local/public strip)
    // (the cosmetics page: owned / explore, its packs only)
    // (each page its own: the plugins page's installed leaves out the cosmetics, the cosmetics page's owned has them: Lvcid)
    auto mine = [](const Plug& p) { return gCosRows ? p.category == "cosmetics" : p.category != "cosmetics"; };
    auto mineBrowse = [](const Plug& p) { return gCosRows ? p.category == "cosmetics" : p.category != "cosmetics"; };
    const std::vector<std::pair<std::string, int>> tabs = {
        {"installed", static_cast<int>(std::count_if(gInstalled.begin(), gInstalled.end(), mine))},
        {"browse", static_cast<int>(std::count_if(gBrowse.begin(), gBrowse.end(), [&](const Plug& p) { return !p.installed && mineBrowse(p); }))}};
    auto label = [](const std::string& tab) { return !gCosRows ? tab : tab == "installed" ? std::string("owned") : std::string("explore"); };
    // Lvcid's layout: refresh, the installed / browse strip, the plugins folder and the search on the left; the views
    // and (installed) the updates on the right. Refresh first, on both tabs: the online registry again (the browse
    // list, the newest versions: the update counts), the lists rebuilt.
    // (after the views, on the left: Lvcid; Ballest Night: the strip first, the views after the search, by the version)
    const bool viewsRight = BallestNight();
    const double sx0 = viewsRight ? 22 : 22 + (4 + 2 * 40 + 4 + 4) + 12;
    double x = sx0 + 8;
    std::vector<std::pair<Rect, double>> segs;
    for (const auto& [tab, count] : tabs) {
        const double tw = Measured(wl, "seg." + tab, label(tab), 21), cw = Measured(wl, "seg." + tab + ".n", std::to_string(count), 14) + 14;
        double sw = 22 + tw + 10 + cw + 22;
        {
            // (Lvcid: each tab as wide as the wider of the two pages' tabs in its place, installed / owned and browse /
            // explore, so the strip and the search after it sit exactly the same on the plugins and cosmetics pages)
            const bool first = tab == "installed";
            const size_t pc = first ? static_cast<size_t>(std::count_if(gInstalled.begin(), gInstalled.end(), [](const Plug& q) {
                                          return q.category != "cosmetics";
                                      }))
                                    : static_cast<size_t>(std::count_if(gBrowse.begin(), gBrowse.end(), [](const Plug& q) {
                                          return !q.installed && q.category != "cosmetics";
                                      }));
            const size_t cc = first ? static_cast<size_t>(std::count_if(gInstalled.begin(), gInstalled.end(), [](const Plug& q) {
                                          return q.category == "cosmetics";
                                      }))
                                    : static_cast<size_t>(std::count_if(gBrowse.begin(), gBrowse.end(), [](const Plug& q) {
                                          return !q.installed && q.category == "cosmetics";
                                      }));
            auto wide = [](const std::string& name, size_t n) {
                return 22 + pmui::Width(name, 21, "Regular") + 10 + pmui::Width(std::to_string(n), 14, "Bold") + 14 + 22;
            };
            sw = std::max({sw, wide(tab, pc), wide(first ? "owned" : "explore", cc)});
        }
        segs.push_back({{x, cy - 21, sw, 42}, tw});
        x += sw + 8;
    }
    {
        // (Ballest Light: the strip's own space round its two tabs stays dark: Lvcid)
        std::optional<ux::RawColors> raw;
        if (BallestLight()) raw.emplace();
        pmui::BoxCorners(wl, "strip", 10, {sx0, cy - 29, x - sx0, 58}, kStrip, 16, 22, 8, 8).Outline(2, kBlack);
    }
    // A switch: the lime box glides over to the other tab, quick and smooth (Lvcid: snappier, sleeker): its leading edge
    // goes first and its trailing edge follows a moment later (a slight stretch that settles), from wherever it is (a
    // second click mid-way turns it round); a small puff of sparks where it arrives. One box all the time (at rest too),
    // so nothing swaps at the end. The tabs' words go black where it covers them.
    constexpr double kLead = 0.13, kTrail = 0.2;
    const double since = gS.swapFrom.empty() ? 1e9 : pmui::Now() - gS.swapAt;
    if (since > kTrail + 0.12) gS.swapFrom.clear();
    const bool sliding = !gS.swapFrom.empty();
    const Rect& target = gS.tab == tabs[0].first ? segs[0].first : segs[1].first;
    Rect slider = target;
    const bool rightward = target.Cx() > gSlideFrom.Cx();
    if (sliding) {
        const Rect& f = gSlideFrom;
        const double lead = EaseOut(since / kLead, 3), trail = EaseOut(since / kTrail, 3);
        const double l = f.x + (target.x - f.x) * (rightward ? trail : lead);
        const double rr = f.R() + (target.R() - f.R()) * (rightward ? lead : trail);
        slider = {l, target.y, rr - l, target.h};
    }
    gSlideNow = slider;
    // Ballest Night: the open tab in the game's colours, not the lime bubble: installed / owned pink, browse / explore
    // cyan (Lvcid)
    const bool tabColours = BallestNight();
    auto tabColour = [&](size_t i) { return !tabColours ? kLime : i == 0 ? kPink : kBallestCyan; };
    for (size_t i = 0; i < tabs.size(); ++i) {
        const std::string& tab = tabs[i].first;
        const Rect r = segs[i].first;
        const bool on = gS.tab == tab;
        // how much of it the lime covers: all or nothing at rest; while sliding, as the box passes over it
        double lit = on ? 1.0 : 0.0;
        if (sliding) {
            const double overlap = std::max(0.0, std::min(r.R(), slider.R()) - std::max(r.x, slider.x));
            lit = std::clamp(overlap / std::max(1.0, r.w), 0.0, 1.0);
        }
        const double h = Ease("seg." + tab, pmui::Over(wl, r));
        pmui::Box(wl, "seg." + tab + ".b", 11, r, kBlack, 9);
        pmui::Box(wl, "seg." + tab + ".hover", 12, r, A(ux::Hex("#1C1E22"), (1 - lit) * h), 9);
        const std::string n = std::to_string(tabs[i].second);
        const double cw = Measured(wl, "seg." + tab + ".n", n, 14) + 14;
        const double tx = r.x + (r.w - (segs[i].second + 10 + cw)) / 2;     // (the name and its count, centred)
        const Rect chip{tx + segs[i].second + 10, cy - 12, cw, 24};
        if (tabColours) {
            // (as the game's leaderboard writes on its colours: on pink, white with a black drop shadow; on cyan,
            // black; the name and the count; the count's box fades away under the colour)
            const bool pink = i == 0;
            const Color word = pink ? kWhite : kBlack;
            const double shadow = pink ? 0.8 * lit : 0;
            ux::RawColors raw;                  // (Ballest Light: these words on the pink / cyan as they are, Lvcid)
            // (a tab not open, hovered: white in Night; black in Ballest Light, on its light tab: Lvcid)
            const Color ink = Mix(Mix(kSegText, BallestLight() ? kBlack : kWhite, h), word, lit);
            // (the shadows a level under their words: the same level drew them over the words)
            pmui::Text(wl, "seg." + tab + ".sh", 13, label(tab), 21, "Regular", A(kBlack, shadow), tx + 1.5, cy + 1.5);
            pmui::Text(wl, "seg." + tab, 14, label(tab), 21, "Regular", ink, tx, cy);
            pmui::Box(wl, "seg." + tab + ".c", 13, chip, A(kChip, 1 - lit), 8);
            pmui::Text(wl, "seg." + tab + ".n.sh", 14, n, 14, "Bold", A(kBlack, shadow), chip.Cx() + 1.2, cy + 1.2, 0.5);
            pmui::Text(wl, "seg." + tab + ".n", 15, n, 14, "Bold", Mix(kSub, word, lit), chip.Cx(), cy, 0.5);
        } else {
            pmui::Text(wl, "seg." + tab, 13, label(tab), 21, "Regular", Mix(Mix(kSegText, kWhite, h), kBlack, lit), tx, cy);
            pmui::Box(wl, "seg." + tab + ".c", 13, chip, Mix(kChip, A(kBlack, 0.85), lit), 8);
            pmui::Text(wl, "seg." + tab + ".n", 14, n, 14, "Bold", Mix(kSub, kLime, lit), chip.Cx(), cy, 0.5);
        }
        // browse: a NEW on its top right corner while the registry has a plugin new to it (the browse rows' tag), gone
        // once the tab has been hovered, as the red dots (Lvcid)
        if (tab == "browse") {
            std::string fresh;
            for (const Plug& q : gBrowse)
                if (!q.installed && mineBrowse(q) && (q.fresh || gTestNew)) fresh += q.id + ",";
            if (!fresh.empty() && DotShown(std::string(gCosRows ? "new.explore@" : "new.browse@") + fresh, pmui::Over(wl, r)))
                NewLetters(wl, "seg.browse.new", 15, r.R() - 8, r.y - 3, 14);
        }
        if (pmui::Click(wl, r, "seg." + tab) && gS.tab != tab) {
            gSlideFrom = gSlideNow;         // (from where the box is now)
            gS.swapFrom = gS.tab;
            gS.swapAt = pmui::Now();
            gS.tab = tab;
            gS.expanded.clear();
        }
    }
    if (tabColours) {
        // (the test: a flat box in the colour of the tab it's over, blending across as it slides)
        const double span = std::max(1.0, segs[1].first.Cx() - segs[0].first.Cx());
        const double f = std::clamp((slider.Cx() - segs[0].first.Cx()) / span, 0.0, 1.0);
        const Color c = Mix(tabColour(0), tabColour(1), f);
        Glow(wl, "seg.slide.glow", 11, slider, c, 0.3);
        pmui::Box(wl, "seg.slide.flat", 12, slider, c, 9);
    } else {
        Glow(wl, "seg.slide.glow", 11, slider, kLime, 0.3);
        pmui::Bubble(wl, "seg.slide", 12, slider, 9, 1);
    }
    if (sliding) {
        // the sparks: a small quick puff off its leading side as it arrives
        const double hit = std::clamp((since - kLead * 0.75) / 0.18, 0.0, 1.0);
        if (hit > 0 && hit < 1) {
            const double ex = rightward ? target.R() : target.x, out = rightward ? 1 : -1;
            for (int i = 0; i < 6; ++i) {
                const double r1 = ((i * 61 + 7) % 100) / 100.0, r2 = ((i * 29 + 43) % 100) / 100.0;
                const double ang = (r1 - 0.5) * 2.4;
                const double dist = (8 + 14 * r2) * EaseOut(hit, 2);
                const double sx = ex + out * std::cos(ang) * dist, sy = cy + (r2 - 0.5) * 18 + std::sin(ang) * dist;
                const double size = 10 * (1 - 0.5 * hit) * (0.7 + 0.5 * r1);
                pmui::Pic(wl, "seg.spark" + std::to_string(i), 15, {sx - size / 2, sy - size / 2, size, size}, "fx-spark.png",
                          A(Mix(kWhite, kLime, hit), 0.75 * (1 - hit) * (1 - hit)));
            }
        }
    }
    x += 16;
    // on the right (Lvcid): this version, muted, and "what's new" (the plugin manager's GitHub page), a dark, muted button
    const std::string ver = std::string("v") + plugins::kHostVersion;
    const double ww = PillWidth(wl, "whatsnew", "what's new", 16, nullptr, 18);
    const Rect wr{W - 22 - ww, cy - 20, ww, 40};
    {
        // (dark, its words a light grey: readable on the window, Lvcid; hovered, lighter and white)
        const double h = Ease("whatsnew", pmui::Over(wl, wr));
        if (BallestNight()) {
            // (the Ballest themes: ballest cyan, black words, as the game's leaderboard; hovered, as the get button in
            // the dark: white with a white glow, growing a little from its middle, on the pages' curve: Lvcid)
            const double hv = HoverCurve("whatsnew.hv", pmui::Over(wl, wr), kHoverIn, kHoverOut);
            const Rect g = wr.Grow(3 * hv);
            Glow(wl, "whatsnew.glow", 10, g, Mix(kBallestCyan, kWhite, hv), 0.2 + 0.25 * hv);
            pmui::Box(wl, "whatsnew.b", 11, g, Mix(kBallestCyan, kWhite, hv), 12 + 3 * hv)
                .Outline(BallestLight() ? 2.2f : 0.0f, A(kBlack, 0.9));      // (Ballest Light: a black trim, as the lime, thicker up here: Lvcid)
            pmui::Text(wl, "whatsnew.t", 12, "what's new", 16, "Regular", kBlack, wr.Cx(), cy, 0.5);
            if (pmui::Entry* e = pmui::Find(wl, "whatsnew.t")) e->node.Transform(1 + 0.06 * hv, 0);      // (the words grow with it)
        } else {
            pmui::Box(wl, "whatsnew.b", 11, wr, Mix(ux::Hex("#1D1F23"), ux::Hex("#2C2F35"), h), 12)
                .Outline(2, Mix(ux::Hex("#2F3238"), ux::Hex("#4C5058"), h));
            pmui::Text(wl, "whatsnew.t", 12, "what's new", 16, "Regular", Mix(ux::Hex("#9A9EA6"), kWhite, h), wr.Cx(), cy, 0.5);
        }
        if (pmui::Click(wl, wr, "whatsnew")) OpenUrl(kRepo);      // (the plugin manager's GitHub page: its releases and notes)
    }
    const double vtw = Measured(wl, "ver.t", ver, 16);
    {
        // (Ballest Light: the gold version's letters outlined in black, no drop shadow: Lvcid)
        std::optional<ux::RawColors> raw;
        if (BallestLight()) raw.emplace();
        pmui::Text(wl, "ver.t", 10, ver, 16, "Regular", BallestNight() ? kBallestGold : Mix(kVer, kWindow, 0.3), wr.x - 14 - vtw, cy);      // (a touch dimmer)
        pmui::TextOutline(wl, "ver.t", BallestLight() ? 1 : 0, A(kBlack, 0.9));
    }
    double rightEdge = wr.x - 14 - vtw - 18;
    // the views: compact, thick (Lvcid: no grid)
    const std::vector<std::pair<std::string, const char*>> views = {{"compact", "pm-compact.png"}, {"thick", "pm-thick.png"}};
    const double vw = 4 + 2 * 40 + 4 + 4;
    const Rect box{viewsRight ? rightEdge - 6 - vw : 22, cy - 23, vw, 46};
    if (viewsRight) rightEdge = box.x - 18;
    // (Ballest Light: no black line round the views' box: only the picked one's lime gets its trim, Lvcid)
    pmui::Box(wl, "views", 10, box, kStrip, 12).Outline(BallestLight() ? 0.0f : 2.0f, kBlack);
    double vx = box.x + 4;
    for (const auto& [view, icon] : views) {
        const Rect r{vx, box.y + 6, 40, 34};
        const bool on = gPrefs.view == view;
        const double h = Ease("view." + view, pmui::Over(wl, r));
        const Color viewOn = kLime;
        if (on) Glow(wl, "view." + view + ".glow", 11, r, viewOn, 0.35);
        pmui::Box(wl, "view." + view + ".b", 12, r, on ? viewOn : kBlack, 8)
            .Outline(BallestLight() && on ? 2.2f : 0.0f, A(kBlack, 0.9));      // (Ballest Light: the picked one's black trim, thicker up here: Lvcid)
        pmui::Pic(wl, "view." + view + ".i", 13, {r.Cx() - 10, r.Cy() - 10, 20, 20}, icon, on ? kBlack : Mix(kSegText, kWhite, h));
        if (pmui::Click(wl, r, "view." + view) && !on) {
            gPrefs.view = view;
            PrefsChanged();
        }
        vx += 44;
    }
    // this page's own search, from the strip to the version (Lvcid: a search for each page)
    BrowseTools(wl, cy, x - 4, rightEdge);
}

// Update all and its note, right-aligned at `right` (on the top bar, every page: Lvcid, 2026-10-09).
void UpdateAll(Layer& wl, double right, double cy) {
    constexpr double kPillK = 1.4;
    // Update all on rails (Lvcid, 2026-10-09): what can update
    // now, with the plugin manager's own update in mind. While it's out (or failed) and none of the updates need it,
    // update all does both; when some need it, the plugin manager comes first (from the banner, or as "update plugin
    // manager" once the banner is hidden); installed, the game's restart; ones needing a plugin manager not out yet are
    // left out.
    const HostNow host = Host();
    std::vector<const Plug*> ready;         // can update now
    bool waiting = false, notOut = false;   // some wait for the plugin manager's update; some need one not out yet
    for (const Plug& p : gInstalled) {
        if (!p.update) continue;
        const std::string need = NeedsHost(p.id);
        if (need.empty()) ready.push_back(&p);
        else if (host.stage != HostStage::None && plugins::CompareVersions(host.version, need) >= 0) waiting = true;
        else notOut = true;
    }
    const bool hostShown = HostShown(host);
    const int total = gUpdates + (hostShown ? 1 : 0);
    const bool busy = std::any_of(gInstalled.begin(), gInstalled.end(), [](const Plug& p) { return p.update && p.pending == "installing"; });
    enum class All { None, Go, Busy, Grey, Restart } mode = All::None;
    std::string label = "update all", tip;
    bool withHost = false, onlyHost = false;
    if (busy) {
        mode = All::Busy, label = "updating...";
    } else if (host.stage == HostStage::Installed) {
        mode = All::Restart, label = "awaiting game restart";
        tip = std::string("The plugin manager is updated: restart the game for it to take effect.") + (gUpdates ? " Then update all does the rest." : "");
    } else if (host.stage == HostStage::Downloading) {
        mode = All::Grey, tip = "The plugin manager is updating (the banner shows how it goes).";
    } else if (host.stage == HostStage::Failed) {
        if (gS.bannerLater) mode = All::Go, label = "update plugin manager", onlyHost = true;
        else mode = All::Grey, tip = "The plugin manager's update didn't finish: try again on the banner.";
    } else if (host.stage == HostStage::Available) {
        if (!waiting && !ready.empty()) mode = All::Go, withHost = true;
        else if (gS.bannerLater) mode = All::Go, label = "update plugin manager", onlyHost = true;
        else mode = All::Grey, tip = waiting ? "Some of these need the new plugin manager: update it first (the banner)." : "Update the plugin manager on the banner.";
    } else if (!ready.empty()) {
        mode = All::Go;
    } else if (notOut) {
        mode = All::Grey, tip = "These need a plugin manager that isn't out yet.";
    }
    if (total == 0 && mode != All::Restart && mode != All::Busy) mode = All::None;
    const double pillRest = pmui::Width(mode == All::None ? "update all" : label, 14.5 * kPillK, "Bold") + 22 * kPillK;
    const double pillSlide = 14.5 * kPillK * 1.45 + 6 * kPillK;     // (hovered, its icon slides out on its left)
    // the note: how many (the plugin manager's own counted), or up to date (Lvcid: never "plugin manager")
    // (its dot after it: Lvcid; the count is the note's, no red dot on the pill)
    const std::string note = (total == 0 ? std::string("up to date") : std::to_string(total) + (total == 1 ? " update" : " updates")) + "  \xC2\xB7";
    const double noteW = Measured(wl, "meta.b", note, 17);
    const double pillRight = right;
    if (mode != All::None) {
        // the update tags' pill, bigger (it grows leftward); the note glides left with it
        Layer& tools = pmui::Group(wl, "updateall.g", 310, {0, cy - 40, right + 40, 80}, false);
        Rect r;
        if (mode == All::Go || mode == All::Busy) {
            bool go = false;
            const double restW = UpdatePill(tools, "updateall", pillRight, 40, label, mode == All::Busy, kPillK, &go, true, 0, onlyHost, true);
            if (go) {
                if (!onlyHost)
                    for (const Plug* p : ready)
                        if (p->pending != "installing" && p->pending != "removing") InstallUpdate(*p);
                if (withHost || onlyHost) StartHostUpdate();
                hostlog::Info(std::string("manager: ") + (onlyHost ? "update plugin manager" : withHost ? "update all, the plugin manager too" : "update all"));
                gModelAt = -100;
            }
            const double wide = (restW + pillSlide * Smooth(gAnim["updateall.icon"].value)) * (1 + 0.08 * gAnim["updateall"].value);
            const double ph = 28 * kPillK * (1 + 0.08 * gAnim["updateall"].value);
            r = {pillRight - wide, cy - ph / 2, wide, ph};
        } else {
            // greyed: shown, counted, not pressed (why on hover); awaiting the restart: the running light round it
            LockedPill(tools, "updateall", pillRight, 40, label, kPillK, true, false, mode == All::Restart, tip, &r);
            r.y += cy - 40;
        }
        pmui::Text(wl, "meta.b", 10, note, 17, "Regular", kLime, r.x - 14 - noteW, cy);
    } else {
        // nothing to update: the pill blacked out (as an unclickable button), "up to date" darker
        const double ph = 28 * kPillK;
        const Rect r{pillRight - pillRest, cy - ph / 2, pillRest, ph};
        pmui::Box(wl, "updateall.off", 10, r, kOffFill, ph / 2).Outline(2, A(kAbEdge, 0.5));
        pmui::Text(wl, "updateall.off.t", 11, "update all", 14.5 * kPillK, "Bold", A(kSub, 0.25), r.Cx(), cy, 0.5);
        pmui::Text(wl, "meta.b", 10, note, 17, "Regular", kCogOff, r.x - 14 - noteW, cy);
    }
}

// The cosmetics page's two sections (Lvcid, 2026-10-10): EXAMPLES first (these, in this order), then COMMUNITY (the
// rest), sorted by its compact sort on its heading: newest, oldest, A-Z, Z-A. Age: the day this plugin manager first
// saw it (the registry has no dates; kept by its id), then its place in the registry (later is newer).
const char* const kCosExamples[] = {"example-cosmetics", "example-arms", "example-bounce"};
const char* const kCosRowSorts[] = {"newest", "oldest", "A-Z", "Z-A"};
int gCosSort = 0;
// Folded sections (Lvcid, 2026-10-11): a click on a section's heading folds it (and opens it again). Kept in the
// plugin manager's storage as "<page>:<heading>" ("plugins:INSTALLED", "cosmetics:COMMUNITY"), so it lasts through
// updates; a heading renamed is simply open again (only the names kept are folded). Failsafe: anything in an unexpected
// shape, or under another version mark than kFoldVersion, and everything is open. A search shows every section open.
constexpr const char* kFoldVersion = "v1";
std::set<std::string> gFolded;
bool gFoldedRead = false;
void ReadFolded() {
    gFoldedRead = true;
    gFolded.clear();
    const std::string v = storage::Get(kSelf, "manager.folded", "");
    size_t at = 0;
    std::vector<std::string> parts;
    while (at <= v.size()) {
        const size_t bar = v.find('|', at);
        parts.push_back(v.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    if (parts.empty() || parts[0] != kFoldVersion) return;          // (none yet, or another shape: all open)
    for (size_t i = 1; i < parts.size(); ++i) {
        const std::string& f = parts[i];
        const size_t colon = f.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= f.size() || f.size() > 80) {
            gFolded.clear();                // (something unexpected: everything open)
            return;
        }
        gFolded.insert(f);
    }
}
void SaveFolded() {
    std::string v = kFoldVersion;
    for (const std::string& f : gFolded) v += "|" + f;
    storage::Set(kSelf, "manager.folded", v);
}

int CosExample(const std::string& id) {
    for (size_t i = 0; i < std::size(kCosExamples); ++i)
        if (id == kCosExamples[i]) return static_cast<int>(i);
    return -1;
}

// A section heading's small fold arrow. Ballest Night: lime with a soft lime glow; Ballest Light: a dark grey (Lvcid);
// else `c`.
ux::Node& ChevLook(Layer& list, const std::string& k, int z, const Rect& r, const Color& c, double fh, bool night, bool light) {
    if (night) {
        // (lime at rest with no glow; hovered, it lights up: brighter, a lime glow round it: Lvcid)
        if (fh > 0.01) Glow(list, k + ".chev.glow", z, r.Grow(-1), kLime, 0.5 * fh);
        return pmui::Pic(list, k + ".chev", z, r, "pm-chev.png", Mix(A(kLime, 0.75), Mix(kLime, kWhite, 0.25), fh));
    }
    if (light) {
        ux::RawColors raw;
        return pmui::Pic(list, k + ".chev", z, r, "pm-chev.png", A(Mix(ux::Hex("#4A4D53"), ux::Hex("#1E1F23"), fh), 1));
    }
    return pmui::Pic(list, k + ".chev", z, r, "pm-chev.png", c);
}

void PluginsPage(Layer& wl, const Rect& area) {
    pmui::Level(kPageLevel);
    if (BallestLight()) {
        // Ballest Light (Lvcid): the strip, the search, the views, the version and what's new on a dark band, drawn
        // as Ballest Night draws them: some dark done right, to balance the light panel's top
        ux::RawColors raw;
        pmui::Box(wl, "sub.band", 9, {12, area.y + 4, area.w - 24, kSubH - 8}, ux::Hex("#121316"), 18).Outline(1.5f, ux::Hex("#2A2C31"));
        gNightBand = true;
        SubBar(wl, area.w, area.y);
        gNightBand = false;
    } else {
        // (Ballest Night: the band too, a touch lighter than the window, for the pink / cyan shape under it: Lvcid)
        if (CurrentTheme() == 0)
            pmui::Box(wl, "sub.band", 9, {12, area.y + 4, area.w - 24, kSubH - 8}, ux::Hex("#15161B"), 18).Outline(1.5f, ux::Hex("#2A2C31"));
        SubBar(wl, area.w, area.y);
    }
    const Rect view{0, area.y + kSubH, area.w, area.h - kSubH};
    // switched tabs: the new rows come in with the lime box (a short fade, a small slide from the side it goes to)
    const double in = gS.swapFrom.empty() ? 1.0 : EaseOut((pmui::Now() - gS.swapAt) / 0.2, 3);
    const double dx = (1 - in) * 14 * (gS.tab == "browse" ? 1 : -1);
    Layer& clip = pmui::Group(wl, "list", 20, view, true);
    Layer& list = pmui::Group(clip, "list.in", 0, {dx, 0, view.w, view.h}, false, 0.2 + 0.8 * in);
    const bool browse = gS.tab == "browse";
    // a search (the page's one) finds in both tabs: what you have first, then what's available (Lvcid)
    const std::string& query = gS.installedSearch;
    const bool searching = !Bare(query).empty();
    auto matches = [&](const Plug& p) { return !searching || SearchScore(p.name + " " + p.author + " " + p.description + " " + p.id, query) >= 0; };
    std::vector<const Plug*> rows;
    std::vector<bool> rowBrowse;            // (each row as browse shows it: get, NEW; else as installed: its toggle)
    if (searching || !browse)
        for (const Plug& p : gInstalled)
            if ((gCosRows ? p.category == "cosmetics" : p.category != "cosmetics") && matches(p)) rows.push_back(&p), rowBrowse.push_back(false);
    if (searching || browse)
        for (const Plug& p : gBrowse)
            if (!p.installed && (gCosRows ? p.category == "cosmetics" : p.category != "cosmetics") && matches(p)) rows.push_back(&p), rowBrowse.push_back(true);
    if (gCosRows && !searching) {
        // examples first, in their order; the community after them, in the sort's order
        std::map<std::string, std::pair<long, long>> age;
        const auto& entries = registry::Entries();
        for (const Plug* p : rows) {
            long at = -1;
            for (size_t i = 0; i < entries.size(); ++i)
                if (entries[i].id == p->id) at = static_cast<long>(i);
            age[p->id] = {std::atol(storage::Get(kSelf, "manager.seen." + p->id, "0").c_str()), at};
        }
        std::stable_sort(rows.begin(), rows.end(), [&](const Plug* a, const Plug* b) {     // (one tab: rowBrowse all the same)
            const int ea = CosExample(a->id), eb = CosExample(b->id);
            if ((ea >= 0) != (eb >= 0)) return ea >= 0;
            if (ea >= 0) return ea < eb;
            switch (gCosSort) {
                case 0: return age[a->id] > age[b->id];
                case 1: return age[a->id] < age[b->id];
                case 2: return Lower(a->name) < Lower(b->name);
                default: return Lower(a->name) > Lower(b->name);
            }
        });
    }
    const std::string key = std::string(gCosRows ? "cos." : "") + "list." + gS.tab + "." + gPrefs.view;     // (each page its own scroll)
    const double rowW = view.w - 22 - 34;
    if (rows.empty()) gS.sortOpen = false;      // (no rows, no heading: the sort's list closed, not left to come back open)
    if (rows.empty() && searching) {
        pmui::Text(list, "empty", 1, "Nothing matches \"" + query + "\".", 18, "Semilight", kSub, view.w / 2, 80, 0.5);
        return;
    }
    if (rows.empty() && browse && registry::State() == "ready") {
        pmui::Text(list, "empty", 1, gCosRows ? "Every pack in the registry is yours." : "Every plugin in the registry is installed.", 18, "Semilight", kSub,
                   view.w / 2, 80, 0.5);
        return;
    }
    if (rows.empty() && gCosRows && !browse && registry::State() == "ready") {
        pmui::Text(list, "empty", 1, "No packs yet: get some from explore.", 18, "Semilight", kSub, view.w / 2, 80, 0.5);
        return;
    }
    if (rows.empty()) {
        const std::string state = registry::State();
        pmui::Text(list, "empty", 1, state == "ready" ? "Nothing in the registry yet." : state == "loading" ? "Loading the registry..." : "The registry isn't loaded: refresh to try again.",
                   18, "Semilight", kSub, view.w / 2, 80, 0.5);
        return;
    }
    {
        const bool compact = gPrefs.view == "compact";
        const double base = compact ? kCompactH : kThickH;
        // a heading over each category, in the featured order
        constexpr double kHeadH = 40;
        const bool heads = true;            // (the cosmetics page: EXAMPLES, COMMUNITY)
        std::vector<std::string> titles(rows.size());
        if (heads)
            for (size_t i = 0; i < rows.size(); ++i) {
                auto title = [&](size_t n) {
                    if (searching) return std::string(rowBrowse[n] ? "AVAILABLE" : gCosRows ? "OWNED" : "INSTALLED");
                    // (the cosmetics page: owned, one OWNED section (it said COMMUNITY: Lvcid); explore, EXAMPLES then COMMUNITY)
                    if (gCosRows) return std::string(!rowBrowse[n] ? "OWNED" : CosExample(rows[n]->id) >= 0 ? "EXAMPLES" : "COMMUNITY");
                    // (the plugins page: one section a tab, INSTALLED / AVAILABLE (Lvcid); the rows still in the categories' order)
                    return std::string(rowBrowse[n] ? "AVAILABLE" : "INSTALLED");
                };
                const std::string t = title(i);
                const std::string before = i ? title(i - 1) : "-";
                if (t != before) titles[i] = t;
            }
        // folded sections (not while searching): their rows aren't listed; each row's section, for that
        if (!gFoldedRead) ReadFolded();
        const std::string foldPage = gCosRows ? "cosmetics:" : "plugins:";
        std::vector<std::string> section(rows.size());
        std::map<std::string, int> sectionCount;
        for (size_t i = 0; i < rows.size(); ++i) {
            section[i] = !titles[i].empty() ? titles[i] : i ? section[i - 1] : "";
            ++sectionCount[section[i]];
        }
        auto folded = [&](size_t i) { return !searching && gFolded.count(foldPage + section[i]) > 0; };
        std::vector<double> heights;
        double content = 4;
        for (size_t ri = 0; ri < rows.size(); ++ri) {
            const Plug* p = rows[ri];
            if (!titles[ri].empty()) content += kHeadH;
            if (folded(ri)) {
                heights.push_back(0);
                continue;
            }
            const std::string id = std::string(gCosRows ? "c" : "") + (rowBrowse[ri] ? "b." : "i.") + p->id;
            Anim& a = gAnim["row." + id];
            a.open = ux::Approach(a.open, gS.expanded == p->id ? 1.0 : 0.0, pmui::Dt(), 0.07);
            const double extra = gOpenExtra.count(id) ? gOpenExtra[id] : 170;
            heights.push_back(base + extra * Smooth(a.open));
            if (gS.scrollTo == p->id) {
                gS.scrolls[key].target = std::max(0.0, content - 12);
                gS.scrollTo.clear();
            }
            content += heights.back() + kGap;
        }
        content += 22;
        gS.scrollTo.clear();                // (its row not listed: dropped, not kept to jump later)
        const double scroll = Scrolled(key, list, {0, 0, view.w, view.h}, content);
        double y = 4 - scroll;
        Rect sortAt;                        // (the cosmetics page: its sort, when its heading is drawn)
        // Ballest Light: the dark column behind the toggles (pieces with the rows, below, scrolling with them) and the
        // medium dark strip between the band and the first section (Lvcid)
        const bool lightCol = BallestLight(), nightCol = CurrentTheme() == 0;
        constexpr double kColW = 90, kColFade = 54;
        const Color kColour = ux::Hex("#121316");
        if (lightCol || nightCol) {
            // (under the band, the band's own shape again a little lower, as a coloured drop shadow (Lvcid): ballest
            // blue on the plugins page; the cosmetics page tries its open tab's pink (owned) or cyan (explore). Drawn
            // under the band; its foot again over the list (the rows scroll under it))
            ux::RawColors raw;
            // Its ends cut in a little, both the same (Lvcid); scrolled down it fades away, back at the list's top.
            // (Ballest Night too, under its band.)
            constexpr double kDrop = 6, kCut = 10;
            const double shown = 1 - std::clamp(scroll / 12, 0.0, 1.0);
            const Color drop = A(browse ? kBallestCyan : kPink, shown);      // (the open tab's colour, both pages: Lvcid)
            const double dx0 = 12 + kCut, dw = view.w - 24 - 2 * kCut;
            pmui::Box(wl, "sub.drop", 7, {dx0, view.y - kSubH + 4 + kDrop, dw, kSubH - 8}, drop, 18);
            pmui::BoxCorners(clip, "sub.drop.foot", 50, {dx0, kDrop - 4 - 18, dw, 18}, drop, 0, 0, 18, 18);
        }
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!titles[i].empty()) {
                if (y + kHeadH > -40 && y < view.h + 40) {
                    const std::string k = "head." + titles[i];
                    // folding (Lvcid: quietly): hovered, a faint bevel lights the heading's line and its edge; folded, its
                    // count after it, faint, and a small faint arrow (pointing on); a click folds or opens it
                    const bool isFolded = folded(i);
                    const Rect hr{14, y + 4, rowW + 8, kHeadH - 8};
                    // (Ballest Light: each section's heading a tab: the light panel rising into a dark bar, holding the
                    // title; its arrow always there, faint: it folds (Lvcid). The bar is drawn below, once the tab's
                    // width is known.)
                    const bool lightHead = BallestNight();      // (both Ballest themes now: Night's bar a lighter dark, Lvcid)
                    const bool nightHead = CurrentTheme() == 0;
                    const int hz = lightHead ? 2 : 1;          // (Ballest Light: the words over the tab's hover light, z 1; the bar z 0)
                    const double fh = searching ? 0 : Ease(k + ".fold", pmui::Over(list, hr), 0.08);
                    if (fh > 0.01 && !lightHead) {
                        pmui::Box(list, k + ".bevel", 0, hr, A(kWhite, 0.022 * fh), 10).Outline(1, A(kWhite, 0.05 * fh));
                        pmui::Box(list, k + ".bevel.t", 0, {hr.x + 10, hr.y + 1, hr.w - 20, 1}, A(kWhite, 0.05 * fh), 0.5);
                    }
                    Tick(list, k + ".bar", hz, {22, y + 15, 3, 14});
                    const double headW = pmui::Text(list, k, hz, titles[i], 14, "Bold", Mix(kHead, kSub, 0.5 * fh), 34, y + 22);
                    const double after = 34 + (headW > 0 ? headW : pmui::Width(titles[i], 14, "Bold")) + 10;
                    double tabEnd = after + 12;         // (what the tab holds ends here)
                    if (isFolded) {
                        const std::string n = std::to_string(sectionCount[titles[i]]);
                        // (its count: lime, white hovered (Ballest Light: dark, by its colour map); Ballest Night: the tab's grey,
                        // turning lime hovered with a faint, tight lime glow (Lvcid))
                        const Color nc = nightHead ? Mix(Mix(kHead, kSub, 0.5 * fh), kLime, fh) : Mix(kLime, kWhite, fh);
                        const double nw = pmui::Text(list, k + ".n", hz, n, 13, "Bold", nc, after, y + 22);
                        if (nightHead && fh > 0.01)
                            Glow(list, k + ".n.glow", hz - 1, {after, y + 15, nw > 0 ? nw : 10, 14}, kLime, 0.22 * fh);
                        tabEnd = after + (nw > 0 ? nw : 10) + 6 + 12;
                        ChevLook(list, k, hz, {after + (nw > 0 ? nw : 10) + 6, y + 16, 12, 12}, A(kHead, 0.55 + 0.45 * fh), fh, nightHead, lightHead && !nightHead)
                            .Transform(1, -90);
                    } else if (fh > 0.01 || lightHead) {
                        ChevLook(list, k, hz, {after, y + 16, 12, 12}, A(kHead, lightHead ? 0.5 + 0.5 * fh : 0.55 * fh), fh, nightHead, lightHead && !nightHead)
                            .Transform(1, 0);
                    }
                    if (lightHead) {
                        // the bar: its left end round the tab's corner, a strip over the tab, the tab's right end sloping
                        // down to the bar's foot, the bar on, its right end dissolving into the halftone on a slant
                        // (pm-section-trail.png); hovered, the tab brightens. The tab's pictures: pm-section-tab-*.png
                        // (both generated: the scripts are kept outside this repo).
                        ux::RawColors raw;
                        constexpr double kLW = 16, kSW = 22, kIn = 3;
                        const Color bar = ux::Hex(nightHead ? "#27292F" : "#17181B");
                        const Rect tb = hr.Grow(-1);
                        if (gCosRows && titles[i] == "COMMUNITY" && !isFolded)
                            tabEnd = std::max(tabEnd, after + 22 + 16 + Measured(list, "cs.sort.t", kCosRowSorts[gCosSort], 13) + 22);
                        const double slopeX = std::max(tb.x + kLW, tabEnd + 8), cut = std::max(tb.x + tb.w * 0.55, slopeX + kSW + 40);
                        pmui::Pic(list, k + ".tab.l", 0, {tb.x, tb.y, kLW, tb.h}, "pm-section-tab-l.png", bar);
                        pmui::Box(list, k + ".tab.top", 0, {tb.x + kLW - 0.5, tb.y, slopeX - tb.x - kLW + 1, kIn}, bar, 0);
                        pmui::Pic(list, k + ".tab.r", 0, {slopeX, tb.y, kSW, tb.h}, "pm-section-tab-r.png", bar);
                        pmui::Box(list, k + ".tab", 0, {slopeX + kSW - 0.5, tb.y, cut - slopeX - kSW + 2.5, tb.h}, bar, 0);
                        pmui::Pic(list, k + ".trail", 0, {cut, tb.y, tb.R() - cut, tb.h}, "pm-section-trail.png", bar);
                        const Color lit = A(kWhite, (nightHead ? 0.05 : 0.5) * fh);      // (Night: a faint lift on the dark)
                        pmui::Pic(list, k + ".tab.l.in", 1, {tb.x, tb.y, kLW, tb.h}, "pm-section-tab-l-in.png", lit);
                        pmui::Box(list, k + ".tab.in", 1, {tb.x + kLW, tb.y + kIn, slopeX - tb.x - kLW, tb.h - kIn}, lit, 0);      // (edge to edge with its ends: overlapping, the hover light doubled into a bright line, Lvcid)
                        pmui::Pic(list, k + ".tab.r.in", 1, {slopeX, tb.y, kSW, tb.h}, "pm-section-tab-r-in.png", lit);
                    }
                    if (!searching && pmui::Click(list, hr, k + ".fold")) {
                        const std::string f = foldPage + titles[i];
                        if (!gFolded.erase(f)) gFolded.insert(f);
                        SaveFolded();
                        hostlog::Info("manager: " + std::string(isFolded ? "opened " : "folded ") + f);
                    }
                    if (gCosRows && titles[i] == "COMMUNITY" && !isFolded) {
                        // the sort, quiet, right after the community's heading (Lvcid: its right end was awkward): a dot,
                        // the order's name and a small chevron, no box; hovered or open, white; its list under it
                        const std::string lab = kCosRowSorts[gCosSort];
                        const double tw = Measured(list, "cs.sort.t", lab, 13);
                        sortAt = {after + 22, y + 9, 16 + tw + 22, 26};
                        if (gS.sortOpen) pmui::Level(kDropLevel);
                        const double sh = EaseTo("cs.sort", pmui::Over(list, sortAt) ? 1.0 : gS.sortOpen ? kOpenLit : 0.0);
                        // (a soft pill at rest, to tell it from the tab; brighter hovered or open: Lvcid)
                        if (BallestLight()) {
                            ux::RawColors raw;
                            pmui::Box(list, "cs.sort.b", 3, sortAt, A(kBlack, 0.07 + 0.08 * sh), 13).Outline(1.2f, A(kBlack, 0.22 + 0.2 * sh));
                        } else {
                            pmui::Box(list, "cs.sort.b", 3, sortAt, A(kWhite, 0.05 + 0.05 * sh), 13).Outline(1.2f, A(kWhite, 0.14 + 0.16 * sh));
                        }
                        pmui::Box(list, "cs.sort.dot", 4, {sortAt.x + 6, sortAt.Cy() - 2, 4, 4}, A(kHead, 0.8), 2);
                        pmui::Text(list, "cs.sort.t", 4, lab, 13, "Regular", Mix(kHead, kWhite, sh), sortAt.x + 16, sortAt.Cy());
                        pmui::Pic(list, "cs.sort.i", 4, {sortAt.R() - 18, sortAt.Cy() - 6, 12, 12}, "pm-chev.png", Mix(kHead, kWhite, sh))
                            .Transform(1, gS.sortOpen ? 180 : 0);
                        if (pmui::Click(list, sortAt, "cs.sort")) gS.sortOpen = !gS.sortOpen;
                        pmui::Level(kPageLevel);
                    }
                }
                y += kHeadH;
            }
            if (folded(i)) continue;
            if (lightCol && y + heights[i] + kGap > -40 && y < view.h + 40) {
                // (Ballest Light: the dark column behind the toggles, a piece behind each row (round the headings'
                // tabs: the panel shows through them), its right edge fading out; it ends with the list: Lvcid)
                ux::RawColors raw;
                // (its foot at the window's bottom, round as the window's corner: it poked out past it, Lvcid)
                const double floorY = view.h - 4, top = y, h = std::min(heights[i] + kGap, floorY - top);      // (4 above the window's foot: under its edge's curve)
                if (h > 0) {
                    const bool foot = top + heights[i] + kGap >= floorY;
                    const double fr = foot ? std::min(24.0, h) : 0;      // (the foot's corner, no taller than the piece)
                    pmui::BoxCorners(list, "col." + std::to_string(i), -1, {12, top, kColW, h}, kColour, 0, 0, 0, fr);      // (under its lighter wash, z 0)
                    // (lighter at the window's edge, black by the toggles' right: a softer black, Lvcid)
                    pmui::Pic(list, "col.l." + std::to_string(i), 0, {12, top, kColW, h - fr}, "pm-fade-left.png", ux::Hex("#2A2C30"));
                    if (foot) pmui::BoxCorners(list, "col.lf." + std::to_string(i), 0, {12, top + h - fr, fr, fr}, ux::Hex("#2A2C30"), 0, 0, 0, fr);
                    pmui::Pic(list, "col.f." + std::to_string(i), 0, {12 + kColW, top, kColFade, h}, "pm-fade-left.png", kColour);
                }
            }
            if (nightCol && y + heights[i] + kGap > -40 && y < view.h + 40) {
                // (Ballest Night: as Light's column, in its own colours: the bars' lighter dark at the window's edge,
                // fading into the window past the toggles; a piece per row, none behind the headings' tabs: Lvcid)
                const double floorY = view.h - 4, top = y, h = std::min(heights[i] + kGap, floorY - top);
                if (h > 0) {
                    const bool foot = top + heights[i] + kGap >= floorY;
                    const Color wash = ux::Hex("#1E2025");
                    const double fr = foot ? std::min(24.0, h) : 0;      // (the foot's corner, no taller than the piece)
                    // (soft where it meets a heading, above a section's first row and under its last: a hard edge there
                    // made a toggle's or a hovered card's lime glow look cut off, Lvcid)
                    constexpr int kBands = 8;
                    constexpr double kSoft = 16;
                    const bool first = !titles[i].empty(), last = !foot && (i + 1 >= rows.size() || !titles[i + 1].empty());
                    const double softTop = first ? std::min(kSoft, (h - fr) / 2) : 0, softBottom = last ? std::min(kSoft, (h - fr) / 2) : 0;
                    for (int bnd = 0; bnd < kBands; ++bnd) {
                        const double a = (bnd + 0.5) / kBands, bh = softTop / kBands, bb = softBottom / kBands;
                        if (softTop > 0)
                            pmui::Pic(list, "ncol.t" + std::to_string(bnd) + "." + std::to_string(i), 0, {12, top + bnd * bh, kColW + kColFade, bh},
                                      "pm-fade-left.png", A(wash, a));
                        if (softBottom > 0)
                            pmui::Pic(list, "ncol.b" + std::to_string(bnd) + "." + std::to_string(i), 0,
                                      {12, top + h - fr - (bnd + 1) * bb, kColW + kColFade, bb}, "pm-fade-left.png", A(wash, a));
                    }
                    pmui::Pic(list, "ncol." + std::to_string(i), 0, {12, top + softTop, kColW + kColFade, h - fr - softTop - softBottom}, "pm-fade-left.png", wash);
                    if (foot) {
                        pmui::BoxCorners(list, "ncol.f." + std::to_string(i), 0, {12, top + h - fr, fr, fr}, wash, 0, 0, 0, fr);
                        pmui::Pic(list, "ncol.g." + std::to_string(i), 0, {12 + fr, top + h - fr, kColW + kColFade - fr, fr}, "pm-fade-left.png", A(wash, 0.85));
                    }
                }
            }
            const Rect r{14, y, rowW + 8, heights[i]};
            if (r.B() > -40 && r.y < view.h + 40) Row(list, *rows[i], r, base, compact, rowBrowse[i]);
            y += heights[i] + kGap;
        }
        if (gCosRows && gS.sortOpen) {
            if (sortAt.w <= 0) {
                gS.sortOpen = false;        // (its heading not drawn: scrolled away, or no community: closed)
            } else {
                pmui::Level(kDropLevel);
                Rect lr{sortAt.x, sortAt.B() + 4, 130, 8 + 4 * 34.0};      // (from its left, as it sits after the heading)
                // (Ballest Light: the list drawn dark, as Night's: its grey words on the light popup were hard to read, Lvcid)
                std::optional<ux::RawColors> rawList;
                const bool darkList = BallestLight();
                if (darkList) rawList.emplace(), gNightBand = true;
                if (lr.B() > view.h - 8) lr.y = sortAt.y - 4 - lr.h;          // (no room under it: above)
                Glow(list, "cs.sort.shadow", 600, lr, kBlack, 0.8);
                pmui::Box(list, "cs.sort.list", 601, lr, kPopup, 10).Outline(2, kPopupRing);
                for (int n = 0; n < 4; ++n) {
                    const Rect o{lr.x + 4, lr.y + 4 + n * 34.0, lr.w - 8, 32};
                    const std::string ok = "cs.sort.o" + std::to_string(n);
                    const double oh = Ease(ok, pmui::Over(list, o));
                    pmui::Box(list, ok + ".b", 602, o, A(kWhite, 0.08 * oh), 8);
                    pmui::Text(list, ok, 603, kCosRowSorts[n], 15, "Regular", n == gCosSort ? kLime : Mix(kSub, kWhite, oh), o.x + 12, o.Cy());
                    if (pmui::Click(list, o, ok)) {
                        gCosSort = n;
                        gS.sortOpen = false;
                    }
                }
                if (pmui::PressedOutside(list, lr) && pmui::PressedOutside(list, sortAt)) gS.sortOpen = false;
                gNightBand = false;
                pmui::Level(kPageLevel);
            }
        }
    }
    ScrollBar(list, key, 40, {view.w - 20, 8, 8, view.h - 30}, view.h);      // (8 wide: near the game's own, Lvcid)
}

// --- the cosmetics page (the cosmetic browser moves in here next) ------------------------------------------------------------
#include "manager_cosmetics.inc"

// --- settings ------------------------------------------------------------------------------------------------------------------
struct ChoiceList {
    std::string key;
    Rect r;                                 // the dropdown's box, in the overlay's units
    std::vector<std::string> options;
    int current = -1;
    std::function<void(int)> pick;
    bool custom = false;                    // its row draws its own list (the theme's)
};
std::vector<ChoiceList> gChoices;           // the dropdowns drawn this frame (the open one's list goes over everything)
std::map<std::string, std::string> gFieldShown;

constexpr double kCtlW = 400, kRowH = 56;
constexpr double kLabelMax = 680;           // the grey label box at most this wide: rows look the same in any window size
double gRowH = kRowH;                       // the last row's height (a long description makes it taller)

// The label box's width in a pane `w` wide; its control follows it.
double LabelW(double w) { return std::min(kLabelMax, w - kCtlW - 20); }
bool gNoteTips = false;                     // (the plugin manager's own pages: descriptions as tooltips)
// The plugin manager's own pages, tight (Lvcid, 2026-10-11): no label boxes; the names in from the group's
// heading, the controls in one column after them, the rows close.
constexpr double kTightLabelX = 32, kTightCtlX = 206, kTightCtlW = 374, kTightRowH = 42;
// (a page's controls in one column: past its longest name when that's longer than kTightCtlX allows; from the last frame)
std::map<std::string, double> gTightCol;
std::string gTightPage;
double TightCol() { return std::max(kTightCtlX, gTightCol[gTightPage]); }     // this page's control column
double TightRight() { return TightCol() + kTightCtlW; }                        // and its right edge
// After a row: its height and the gap to the next.
double Step() { return gRowH + (gNoteTips ? 0 : 8); }
void Tip(Layer& pane, const std::string& key, const Rect& hit, const std::string& text, const Color* ink, double width,
         bool alignRight);

// A setting's grey label box, its description wrapped under its name (the row grows a line at a time); returns where
// its control goes, right of it.
Rect SettingLabel(Layer& pane, const std::string& key, double y, double w, const std::string& label, const std::string& note) {
    const double lw = LabelW(w);
    double h = kRowH, noteH = 0;
    if (gNoteTips) {
        // (the plugin manager's own pages, tight: the name alone, no box; what it does a tooltip on the row; a name too
        // long for its column pushes its control along)
        h = kTightRowH;
        gRowH = h;
        double& col = gTightCol[gTightPage];
        col = std::max(col, kTightLabelX + pmui::Width(label, 18, "Regular") + 24);
        const double cx = std::max(kTightCtlX, col);
        const Rect row{kTightLabelX - 16, y, cx + kTightCtlW - kTightLabelX + 16, h};
        pmui::Text(pane, key + ".l", 3, label, 18, "Regular", kRowLabel, kTightLabelX, y + h / 2);
        Tip(pane, key, row, note);
        return {cx, y, kTightCtlW, h};
    }
    if (!note.empty()) {
        // its wrapped height as last laid out, else an estimate from its length
        const pmui::Entry* e = pmui::Find(pane, key + ".n");
        noteH = e && e->height > 0 ? e->height : 18 * std::ceil(pmui::Width(note, 13.5, "Semilight") / std::max(80.0, lw - 40));
        h = std::max(kRowH, 34 + noteH + 11);
    }
    gRowH = h;
    const Rect r{0, y, lw, h};
    pmui::Box(pane, key + ".bg", 1, r, kSetRow, 12);
    if (note.empty()) {
        pmui::Text(pane, key + ".l", 3, label, 18, "Regular", kRowLabel, 20, y + kRowH / 2);
    } else {
        pmui::Text(pane, key + ".l", 3, label, 18, "Regular", kRowLabel, 20, y + 19);
        pmui::Para(pane, key + ".n", 3, note, 13.5, "Semilight", kVer, {20, y + 31, lw - 40, std::max(20.0, noteH)});
    }
    return {lw + 20, y, kCtlW, h};
}

bool ToggleRow(Layer& pane, const std::string& key, double y, double w, const std::string& label, const std::string& note, bool on, bool locked = false) {
    const Rect c = SettingLabel(pane, key, y, w, label, note);
    return Toggle(pane, key + ".tog", 3, c.x, c.Cy() - 15, on, locked);      // right beside its label box
}

// A slider's test-drag (Lvcid, 2026-10-11): dragged with the right mouse button, or moved with the pad while X is held,
// it shows the change live but keeps nothing: let go and it's back where it was (and nothing is saved meanwhile).
struct SliderTest {
    std::string key;                        // the slider being test-dragged, or ""
    double orig = 0;                        // its value before
    bool pad = false;                       // by the pad (X held), else by the mouse (right button)
    double seen = -100;                     // the game's clock when it was last drawn
    double backFrom = -1, backAt = 0;       // let go: easing back from this value, since then (quick, eased: Lvcid)
    Prefs prefs;                            // the prefs before (put back if the slider goes away mid-test)
};
SliderTest gSliderTest;
std::string gSliderFocused;                 // the slider holding the pad's focus (its left / right move it)
int gSliderStep = 0;                        // a left / right step for it, from NavKeys
bool gNavHeld = false;                      // a direction held (a slider moved by the pad saves once it's let go)

// A slider on `track`, its value right-aligned at `valueRight`; *t is 0..1 and changes while it is dragged (true then):
// with the left button (or the pad's left / right on it, `step` at a time), kept; `testable`: also test-dragged
// (SliderTest), shown in cyan with "test" by its value, then put back.
bool Slider(Layer& pane, const std::string& key, const Rect& track, double* t, const std::string& value, double valueRight,
            double step = 0.01, bool testable = true, const Color* fillColor = nullptr) {
    const Rect hit = track.Grow(14);
    const std::string id = key + ".s";
    pmui::Click(pane, hit, id);             // (it takes the pad's focus)
    const bool focused = pmui::Navigating() && pmui::Focus() == id;
    if (focused) gSliderFocused = id;
    else if (gSliderFocused == id) gSliderFocused.clear();
    bool changed = false;
    auto set = [&](double nt) {
        nt = std::clamp(nt, 0.0, 1.0);
        changed = changed || std::fabs(nt - *t) > 1e-4;
        *t = nt;
    };
    const double mouseT = (pmui::Mouse().x - pane.ox - track.x) / track.w;
    if (testable && gSliderTest.key.empty() && !pmui::Held(id)) {
        const bool mouse = input::Pressed(VK_RBUTTON) && pmui::Over(pane, hit), pad = focused && input::Pressed(input::kPadX);
        if (mouse || pad) {
            gSliderTest = {key, *t, pad, game::Seconds(), -1, 0, gPrefs};
            hostlog::Info("manager: test-drag " + key);
        }
    }
    const bool testing = gSliderTest.key == key;
    if (testing) {
        gSliderTest.seen = game::Seconds();
        constexpr double kBack = 0.16;      // (the snap back: quick, easing out)
        if (gSliderTest.backFrom >= 0) {
            const double u = std::clamp((game::Seconds() - gSliderTest.backAt) / kBack, 0.0, 1.0);
            set(gSliderTest.backFrom + (gSliderTest.orig - gSliderTest.backFrom) * (1 - std::pow(1 - u, 3)));
            if (u >= 1) {
                set(gSliderTest.orig);      // (exactly back)
                gSliderTest.key.clear();
            }
        } else if (!(gSliderTest.pad ? input::Down(input::kPadX) : input::Down(VK_RBUTTON))) {
            gSliderTest.backFrom = *t;      // (let go: it goes back where it was)
            gSliderTest.backAt = game::Seconds();
        } else if (!gSliderTest.pad) {
            set(mouseT);
        }
    } else if (pmui::Held(id)) {
        set(mouseT);
    }
    if (focused && gSliderStep != 0 && (!testing || (gSliderTest.pad && gSliderTest.backFrom < 0))) set(*t + step * gSliderStep);
    if (focused) gSliderStep = 0;           // (used, or not usable now: not kept for later)
    const bool test = gSliderTest.key == key;
    const double h = Ease(key + ".h", pmui::Over(pane, hit) || pmui::Held(id) || test || focused);
    const Color fill = test ? kBallestCyan : fillColor ? *fillColor : kNuLime;      // (fillColor: its own, the pink volume's)
    pmui::Box(pane, key + ".track", 3, track, kTileRing, 2);
    const Rect knob{track.x + track.w * *t - 10, track.Cy() - 10, 20, 20};
    if (BallestLight()) {
        // (Ballest Light: a slight drop shadow under the white knob, as its lime / gold words have, so it holds on the
        // light panel: Lvcid)
        ux::RawColors raw;
        // (the coloured fill: a lime one gets its black trim from the theme; pink / cyan hold on their own)
        pmui::Box(pane, key + ".knob.sh", 4, knob.Moved(0.6, 1.4), A(kBlack, 0.28), 10);
    }
    pmui::Box(pane, key + ".fill", 4, {track.x, track.y, track.w * *t, 4}, fill, 2);
    Glow(pane, key + ".kglow", 4, knob, test ? kBallestCyan : kWhite, 0.3 + 0.2 * h);
    pmui::Box(pane, key + ".knob", 5, knob, kWhite, 10);
    pmui::Text(pane, key + ".v", 3, value, 18, "Regular", test ? kBallestCyan : kWhite, valueRight, track.Cy(), 1);
    if (test) pmui::Text(pane, key + ".test", 3, "test", 13, "Bold", A(kBallestCyan, 0.8), valueRight + 10, track.Cy());
    return changed;
}

bool SliderRow(Layer& pane, const std::string& key, double y, double w, const std::string& label, const std::string& note, double* t,
               const std::string& value, double step = 0.01, bool testable = true) {
    const Rect c = SettingLabel(pane, key, y, w, label, note);
    const Rect track{c.x, c.Cy() - 2, c.w - (gNoteTips ? 64 : 90), 4};       // (tight: 310 of the column's 374)
    return Slider(pane, key, track, t, value, c.R(), step, testable);
}

bool ButtonRow(Layer& pane, const std::string& key, double y, double w, const std::string& label, const std::string& note, const std::string& button,
               bool lime = false) {
    const Rect c = SettingLabel(pane, key, y, w, label, note);
    const double bw = PillWidth(pane, key + ".btn", button, 17, nullptr);
    return Pill(pane, key + ".btn", 3, {c.R() - bw, c.Cy() - 20, bw, 40}, button, 17, nullptr, lime ? PillKind::Lime : PillKind::Ghost, false, 11);
}

void ChoiceRow(Layer& pane, Layer& overlay, const std::string& key, double y, double w, const std::string& label, const std::string& note,
               const std::vector<std::string>& options, int current, std::function<void(int)> pick, bool locked = false) {
    const Rect c = SettingLabel(pane, key, y, w, label, note);
    const Rect r{c.x, c.Cy() - 19, c.w, 38};
    if (gS.choiceOpen == key) pmui::Level(kDropLevel);     // (its list open: the box still answers, to close it)
    const double h = locked ? 0 : EaseTo(key + ".h", pmui::Over(pane, r) ? 1.0 : gS.choiceOpen == key ? kOpenLit : 0.0);
    pmui::Box(pane, key + ".box", 3, r, kField, 10).Outline(2, Mix(kAbEdge, kLime, h));
    const std::string shown = current >= 0 && current < static_cast<int>(options.size()) ? options[current] : "";
    pmui::Text(pane, key + ".t", 4, shown, 17, "Regular", locked ? kVer : kRowLabel, r.x + 16, r.Cy());
    pmui::Pic(pane, key + ".chev", 4, {r.R() - 32, r.Cy() - 10, 20, 20}, "pm-chev.png", locked ? kVer : kSub)
        .Transform(1, gS.choiceOpen == key ? 180 : 0);
    if (!locked && pmui::Click(pane, r, key + ".box")) gS.choiceOpen = gS.choiceOpen == key ? "" : key;
    pmui::Level(kBarLevel);
    if (!locked) gChoices.push_back({key, pmui::OnScreen(pane, r).Moved(-overlay.ox, -overlay.oy), options, current, std::move(pick)});
}

// A text box: Enter (or leaving it) keeps what was typed.
void TextRow(Layer& pane, const std::string& key, double y, double w, const std::string& label, const std::string& note, const std::string& value,
             std::function<void(const std::string&)> commit) {
    const Rect c = SettingLabel(pane, key, y, w, label, note);
    const Rect r{c.x, c.Cy() - 19, c.w, 38};
    Obj field = pmui::Field(pane, key + ".field", 5, {r.x + 12, r.y + 3, r.w - 24, r.h - 6}, "", 17, kWhite);
    const bool focused = Focused(field);
    const double h = Ease(key + ".h", focused || pmui::Over(pane, r));
    pmui::Box(pane, key + ".box", 3, r, kField, 10).Outline(2, Mix(kAbEdge, kLime, h));
    std::string& shown = gFieldShown[key];
    Anim& a = gAnim[key + ".focus"];
    const bool ended = a.was && !focused, enter = focused && input::Pressed(VK_RETURN);
    a.was = focused;
    if ((ended || enter) && field) {
        const std::string typed = w::ReadText(field);
        if (typed != value) commit(typed);
        shown.clear();                      // shows what was kept (a clamped value, say)
    }
    if (field && !focused && shown != value) {
        w::SetText(field, value);
        shown = value;
    }
}


double gBackOff = 0;                        // the pane's content pushed down by its back button, this frame

// The section's heading, and how to get back.
double Header(Layer& pane, const std::string& title, const std::string& sub, double w) {
    gTightPage = title;
    pmui::Text(pane, "head.t", 3, title, 34, "Regular", kWhite, 0, 34);
    pmui::Text(pane, "head.s", 3, sub, 17, "Semilight", kSub, 0, 70);
    (void)w;                                // (how to get back: the input legend under the window, Lvcid)
    return 112;
}

// A group's heading: a short lime bar and its name (and a chip: "coming soon").
double GroupHead(Layer& pane, const std::string& key, double y, const std::string& title, const std::string& chip = "") {
    Tick(pane, key + ".bar", 3, {4, y + 3, 3, 14});
    const double tw = pmui::Text(pane, key + ".t", 3, title, 15, "Bold", kVer, 16, y + 10);
    if (!chip.empty()) Chip(pane, key + ".chip", 3, 16 + (tw > 0 ? tw : 150) + 12, y + 10, chip, ChipKind::Plain, 0);
    return y + 32;
}

std::string Percent(double v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; }

constexpr double kGroupGap = 22;

// --- the plugin manager's own settings (Lvcid, 2026-10-08): GENERAL > appearance, advanced; then each plugin --------
// On these pages a setting's description is a tooltip on its row (plugins' own pages keep theirs under the name).
void ClosePopup();

// A tooltip for `hit` (in pane's units) while it's hovered: the popup's look, under it (above it when there's no room
// under it). Drawn on gFx, over everything and anywhere on the screen (Lvcid, 2026-10-11), kept 8 units inside the
// screen's edges. It waits a moment before it shows (as tooltips do: Lvcid, no popping up at once); one hovered soon
// after another closed shows almost at once. `ink`: its words' colour (else the popups'); `width`: else from the hit's;
// `alignRight`: its right edge at the hit's (else its left 16 in).
Rect gFxScreen;                             // gFx's screen, in its own units
void Tip(Layer& pane, const std::string& key, const Rect& hit, const std::string& text, const Color* ink, double width, bool alignRight) {
    constexpr double kDelay = 1.0, kWarmDelay = 0.1, kWarmFor = 0.45;     // (a second: Lvcid)
    static std::map<std::string, double> since;
    static double lastShownAt = -100;
    const double now = pmui::Now();
    const bool over = !text.empty() && pmui::Over(pane, hit) && !pmui::Mouse().down;      // (not while dragging)
    if (!over) since.erase(key);
    else if (!since.count(key)) since[key] = now;
    const bool warm = now - lastShownAt < kWarmFor;
    const bool want = over && now - since[key] >= (warm ? kWarmDelay : kDelay);
    const double th = Ease(key + ".tip", want, 0.05);
    if (th < 0.01 || text.empty()) return;
    lastShownAt = now;
    static std::map<std::string, double> heights;
    double& lastH = heights[key];
    const double tw = width > 0 ? width : std::min(440.0, std::max(260.0, hit.w * 0.6)), hh = std::max(36.0, lastH + 20);
    Layer& on = gFx ? *gFx : pane;
    const Rect s = pmui::OnScreen(pane, hit);
    const Rect h{s.x - on.ox, s.y - on.oy, s.w, s.h};      // (the hit in `on`'s units)
    const Rect screen = gFx ? gFxScreen : Rect{h.x - 4000, h.y - 4000, 8000, 8000};
    double x = alignRight ? h.R() - tw : h.x + 16, y = h.B() + 6;
    if (y + hh > screen.B() - 8) y = h.y - 6 - hh;                      // (no room under it: above)
    x = std::clamp(x, screen.x + 8, std::max(screen.x + 8, screen.R() - 8 - tw));
    y = std::clamp(y, screen.y + 8, std::max(screen.y + 8, screen.B() - 8 - hh));
    Layer& tl = pmui::Group(on, key + ".tipg", 40, {x, y + (1 - th) * 6, tw, hh}, false, th);
    Glow(tl, "shadow", 0, {0, 0, tw, hh}, kBlack, 0.7);
    pmui::Box(tl, "bg", 1, {0, 0, tw, hh}, kPopup, 10).Outline(2, kPopupRing);
    const double ph = pmui::Para(tl, "t", 2, text, 14, "Regular", ink ? *ink : kPopupText, {12, 10, tw - 24, 120});
    if (ph > 0) lastH = ph;
}

// --- appearance ---------------------------------------------------------------------------------------------------------
constexpr double kScaleBase = 0.85;         // the window's size "100%" (what it's drawn at by default)
int gThemeHover = -1;                       // the theme list's hovered (or key-picked) entry

struct ThemeEntry {
    const char *name, *about, *picture;     // picture: its preview's name, pm-theme-<picture>-1.png
    int frames = 1;                         // more than one: the preview cross-fades through -1, -2, ...
};
const ThemeEntry kThemes[] = {{"Ballest Night", "the plugin manager's look, with the game's own lime, pink, cyan and gold", "night", 3},
                              {"Ballest Light", "Ballest Night on the game's own light panel", "light", 3},
                              {"Lucid Lime", "the plugin manager's own look", "limelit"},
                              {"Classic (the old look)", "the old plugins menu; its cog opens these same settings", "classic"}};
constexpr int kThemeCount = 4;
// (the setting's values, as the plugin's choices spell them)
const char* const kThemeValues[] = {"Ballest Night", "Ballest Light", "Lucid Lime", "Classic"};

// (the Plugin Manager plugin's "Theme" setting: Ballest Night (the default, first: Lvcid), Ballest Light, Lucid Lime,
// or Classic, the old menu. An older name ("Limelit Dark", "Lucid Lime (Dark)", from test builds only) isn't one of the setting's
// choices, so the settings loader keeps the default instead: Ballest Night)
// (read once a frame, not on each of its dozens of asks a frame: Frame() and PickTheme forget it)
int gThemeNow = -1;
int CurrentTheme() {
    if (gThemeNow >= 0) return gThemeNow;
    const int nl = NewLookIndex();
    if (nl < 0) return gThemeNow = 0;
    const std::string v = settings::Get(static_cast<size_t>(nl));
    return gThemeNow = v == "Classic" ? 3 : v == kThemeValues[1] ? 1 : v == kThemeValues[0] || v.empty() ? 0 : 2;
}
bool BallestNight() { return CurrentTheme() <= 1; }
bool BallestLight() { return CurrentTheme() == 1 && !gNightBand; }

// Ballest Light (Lvcid, 2026-10-11): Ballest Night drawn on the game's own light panel. Every colour the window draws
// goes through this (ux::SetThemeMap, only while it draws): the greys turned round (the near-black backgrounds to the
// panel's light grey, the cards a shade lighter, white words dark, grey words mid-dark), the game's colours (pink,
// cyan, lime, gold, red) kept as they are; black words and icons (on a lime or cyan button) and white fills (a hover's
// white) as they are. Lime and gold words get a tight black drop shadow (kind 4) to read on the light grey.
Color LightMap(const Color& c, int kind) {
    auto toS = [](float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 1 / 2.2f); };
    auto toL = [](float v) { return std::pow(std::clamp(v, 0.0f, 1.0f), 2.2f); };
    const float r = toS(c.r), g = toS(c.g), b = toS(c.b);
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float lum = 0.299f * r + 0.587f * g + 0.114f * b, sat = mx > 0 ? (mx - mn) / mx : 0;
    const float colour = mx > 0.24f ? std::clamp((sat - 0.28f) / 0.3f, 0.0f, 1.0f) : 0.0f;     // how much of a colour it is
    if (kind == 4) {
        if (colour < 0.6f || c.a < 0.05f) return {0, 0, 0, 0};
        float hue = 0;                                                  // (degrees)
        if (mx - mn > 1e-4f) {
            if (mx == r) hue = 60 * std::fmod((g - b) / (mx - mn), 6.0f);
            else if (mx == g) hue = 60 * ((b - r) / (mx - mn) + 2);
            else hue = 60 * ((r - g) / (mx - mn) + 4);
            if (hue < 0) hue += 360;
        }
        const bool lime = hue >= 62 && hue <= 105, gold = hue >= 36 && hue < 62;
        return lime || gold ? Color{0, 0, 0, 0.45f * c.a} : Color{0, 0, 0, 0};      // (a light one: the words are darker themselves now)
    }
    if (kind == 5) {
        // a lime fill's trim (Lvcid: anything lime on the light panel has a black trim round it)
        if (colour < 0.6f || c.a < 0.3f) return {0, 0, 0, 0};
        float hue = 0;
        if (mx - mn > 1e-4f) {
            if (mx == r) hue = 60 * std::fmod((g - b) / (mx - mn), 6.0f);
            else if (mx == g) hue = 60 * ((b - r) / (mx - mn) + 2);
            else hue = 60 * ((r - g) / (mx - mn) + 4);
            if (hue < 0) hue += 360;
        }
        return hue >= 62 && hue <= 105 ? Color{0, 0, 0, 0.85f * c.a} : Color{0, 0, 0, 0};
    }
    if (lum < 0.02f && kind != 0) return c;                         // black words, icons, edges: as they are
    if (kind == 1 && colour >= 0.6f) {
        // lime and gold words a deeper shade of themselves: the game's bright ones were low contrast on the light panel
        // (AnythingGoes' review; Lvcid), the other colours (pink, cyan, red) as they are
        float hue = 0;
        if (mx - mn > 1e-4f) {
            if (mx == r) hue = 60 * std::fmod((g - b) / (mx - mn), 6.0f);
            else if (mx == g) hue = 60 * ((b - r) / (mx - mn) + 2);
            else hue = 60 * ((r - g) / (mx - mn) + 4);
            if (hue < 0) hue += 360;
        }
        if (hue >= 36 && hue <= 105) {
            constexpr float kDeep = 0.6f;
            return {toL(r * kDeep), toL(g * kDeep), toL(b * kDeep), c.a};
        }
    }
    // Each kind one way only, so a fade never goes dark and back (Lvcid): fills lighter as they were lighter (the
    // near-black backgrounds the panel's grey, the cards a shade lighter, a hover's white white); words and icons the
    // other way round (white words dark, grey ones mid); edges darker, for contrast (a card's trim a mid grey, white
    // rings dark).
    float grey;
    if (kind == 0) grey = lum <= 0.12f ? 0.76f + lum / 0.12f * 0.10f : 0.86f + (lum - 0.12f) / 0.88f * 0.14f;
    else if (kind == 3) grey = 0.60f - std::pow(lum, 0.8f) * 0.46f;
    // (words and icons: a notch darker all through (Lvcid: the top bar's greys too faded): the muted ones a readable
    // mid grey, the plain ones darker, white the game's #2B)
    else grey = lum <= 0.08f ? 0.62f + lum / 0.08f * 0.08f : 0.70f - std::pow((lum - 0.08f) / 0.92f, 0.5f) * 0.56f;
    auto out = [&](float v) { return toL(grey + (v - grey) * colour); };
    return {out(r), out(g), out(b), c.a};
}
const Color& ErrAccent() { return BallestNight() ? kErrAccent : kRed; }
const Color& WarnAccent() { return BallestNight() ? kWarnAccent : kGold; }

// A theme picked: it just changes; settings stays open where it is (Lvcid). Classic: once settings is left, the old
// menu shows again (the window is only settings there: its cog opens it).
void PickTheme(int n) {
    n = std::clamp(n, 0, kThemeCount - 1);
    if (n == CurrentTheme()) return;
    const int nl = NewLookIndex();
    if (nl < 0) return;
    hostlog::Info(std::string("manager: theme ") + kThemes[n].name);
    settings::Set(static_cast<size_t>(nl), kThemeValues[std::clamp(n, 0, kThemeCount - 1)]);
    // (the new theme from this frame on: its limes and colour map at once, not the old one's for the rest of the frame)
    gThemeNow = -1;
    const bool night = BallestNight();      // (as ThemeLimes, which Frame runs)
    kLime = night ? kNightLime : kLucidLime;
    kNuLime = night ? kNightLime : kLucidNuLime;
    kTopLime = night ? kNightLime : kLucidTopLime;
    ux::SetThemeMap(BallestLight() ? LightMap : nullptr);
}

// The theme's preview (Lvcid): its pictures (pm-theme-<theme>-1.png, and -2, -3 for a slideshow), in a frame; another
// theme hovered in the dropdown, its picture crossfades in.
int gPreviewShown = -1, gPreviewWas = -1;
double gPreviewAt = -100;
void ThemePreview(Layer& pane, const Rect& r, int theme) {
    if (theme != gPreviewShown) {
        gPreviewWas = gPreviewShown;
        gPreviewShown = theme;
        gPreviewAt = pmui::Now();
    }
    pmui::Box(pane, "pv.bg", 1, r, kCard, 18).Outline(2, kTrim);
    const Rect ir = r.Grow(-12);
    Layer& img = pmui::Group(pane, "pv.img", 2, ir, true);
    pmui::Box(img, "back", 0, {0, 0, ir.w, ir.h}, ux::Hex("#0A0A0C"), 12);
    char name[48];
    const double u = Smooth(std::min(1.0, (pmui::Now() - gPreviewAt) / 0.16));
    if (gPreviewWas >= 0 && u < 1) {
        std::snprintf(name, sizeof name, "pm-theme-%s-1.png", kThemes[gPreviewWas].picture);
        pmui::Pic(img, "was", 1, {0, 0, ir.w, ir.h}, name, kWhite, 10).Opacity(static_cast<float>(1 - u));
    }
    // (several frames (cut from Lvcid's screenshots of the theme): each held a
    // few seconds, the next cross-fading in over it)
    const int frames = std::max(1, kThemes[theme].frames);
    constexpr double kHold = 3.6, kFade = 0.7;
    const double cycle = std::fmod(pmui::Now(), kHold * frames);
    const int f0 = static_cast<int>(cycle / kHold) % frames, f1 = (f0 + 1) % frames;
    const double into = std::fmod(cycle, kHold), next = frames > 1 ? Smooth(std::clamp((into - (kHold - kFade)) / kFade, 0.0, 1.0)) : 0;
    const float show = static_cast<float>(gPreviewWas >= 0 ? u : 1.0);
    std::snprintf(name, sizeof name, "pm-theme-%s-%d.png", kThemes[theme].picture, f0 + 1);
    pmui::Pic(img, "now", 2, {0, 0, ir.w, ir.h}, name, kWhite, 10).Opacity(show);
    if (frames > 1) {
        std::snprintf(name, sizeof name, "pm-theme-%s-%d.png", kThemes[theme].picture, f1 + 1);
        pmui::Pic(img, "next", 3, {0, 0, ir.w, ir.h}, name, kWhite, 10).Opacity(static_cast<float>(show * next));
    }
    if (theme == CurrentTheme()) {
        // (in use: a solid dark pill with a lime edge in the picture's bottom right, clear of the screenshots' own lime
        // and pink at their top left: Lvcid)
        const double tw = Measured(pane, "pv.use.t", "in use", 14), cw = tw + 26 + 12;
        const Rect c{ir.R() - 12 - cw, ir.B() - 12 - 28, cw, 28};
        Glow(pane, "pv.use.glow", 3, c, kBlack, 0.6);
        pmui::Box(pane, "pv.use.b", 4, c, A(kBlack, 0.88), 14).Outline(1.5f, A(kLime, 0.85));
        pmui::Box(pane, "pv.use.dot", 5, {c.x + 12, c.Cy() - 3, 6, 6}, kLime, 3);
        pmui::Text(pane, "pv.use.t", 5, "in use", 14, "Bold", kLime, c.x + 24, c.Cy());
    }
}

// The theme's dropdown at r (its list drawn over everything, under it; hovered, or moved through with the keys or a pad,
// the preview shows that theme; picked, it switches at once; a check by the one in use).
void ThemeRow(Layer& pane, Layer& overlay, const Rect& r) {
    const bool open = gS.choiceOpen == "theme";
    if (open) pmui::Level(kDropLevel);
    const double h = EaseTo("theme.h", pmui::Over(pane, r) ? 1.0 : open ? kOpenLit : 0.0);
    pmui::Box(pane, "theme.box", 3, r, kField, 10).Outline(2, Mix(kAbEdge, kLime, h));
    const int cur = CurrentTheme();
    pmui::Text(pane, "theme.t", 4, kThemes[cur].name, 17, "Regular", kRowLabel, r.x + 16, r.Cy());
    pmui::Pic(pane, "theme.chev", 4, {r.R() - 32, r.Cy() - 10, 20, 20}, "pm-chev.png", kSub).Transform(1, open ? 180 : 0);
    if (pmui::Click(pane, r, "theme.box")) {
        gS.choiceOpen = open ? "" : "theme";
        gThemeHover = cur;
    }
    pmui::Level(kBarLevel);
    const Rect at = pmui::OnScreen(pane, r).Moved(-overlay.ox, -overlay.oy);
    gChoices.push_back({"theme", at, {}, cur, nullptr, true});
    if (!open) {
        gThemeHover = -1;
        return;
    }
    // the keys and a pad move through it; Enter, Space or A picks (not while a popup asks something over it)
    const bool keys = gS.popup.empty();
    if (keys && (input::Pressed(VK_DOWN) || input::Pressed(input::kPadDown) || input::Pressed(input::kPadStickDown))) gThemeHover = (std::max(gThemeHover, 0) + 1) % kThemeCount;
    if (keys && (input::Pressed(VK_UP) || input::Pressed(input::kPadUp) || input::Pressed(input::kPadStickUp))) gThemeHover = (std::max(gThemeHover, 0) + kThemeCount - 1) % kThemeCount;
    pmui::Level(kDropLevel);
    Rect list{at.x, at.B() + 6, at.w, 12 + 42.0 * kThemeCount};
    if (pmui::OnScreen(overlay, list).B() > overlay.clip.B() - 8) list.y = at.y - 6 - list.h;     // (no room under it: above)
    Glow(overlay, "theme.shadow", 600, list, kBlack, 0.8);
    pmui::Box(overlay, "theme.list", 601, list, kPopup, 12).Outline(2, kPopupRing);
    for (int n = 0; n < kThemeCount; ++n) {
        const Rect o{list.x + 6, list.y + 6 + 42.0 * n, list.w - 12, 42};
        const std::string k = "theme.o" + std::to_string(n);
        if (pmui::Over(overlay, o)) gThemeHover = n;
        const double oh = Ease(k, gThemeHover == n);
        pmui::Box(overlay, k + ".b", 602, o, A(kWhite, 0.08 * oh), 8);      // (hovered: a faint lift, as the sort's list: Lvcid)
        if (n == cur) pmui::Pic(overlay, k + ".ck", 603, {o.x + 12, o.Cy() - 9, 18, 18}, "check.png", kLime);
        pmui::Text(overlay, k + ".t", 603, kThemes[n].name, 17, "Regular", n == cur ? kWhite : Mix(kRowLabel, kWhite, oh), o.x + 40, o.Cy());
        if (pmui::Click(overlay, o, k)) {
            gS.choiceOpen.clear();
            PickTheme(n);
        }
    }
    if (keys && (input::Pressed(VK_RETURN) || input::Pressed(VK_SPACE) || input::Pressed(input::kPadA))) {
        gS.choiceOpen.clear();
        if (gThemeHover >= 0) PickTheme(gThemeHover);
    }
    if (pmui::PressedOutside(overlay, list) && pmui::PressedOutside(overlay, at)) gS.choiceOpen.clear();
    pmui::Level(kBarLevel);
}

// A settings page's reset to default, by its title on its right (Lvcid, 2026-10-11): a pill; it asks first (`popup`)
// and says what it resets (`what`: this page's settings only).
void PageButtons(Layer& pane, const std::string& key, double w, const char* popup, const char* what) {
    constexpr double kH = 52;
    const double right = std::min(832.0, w + 20);         // (its right edge 832 in, or the pane's)
    const double rw = PillWidth(pane, key + ".reset.btn", "reset to default", 18, nullptr, 22);
    const Rect reset{right - rw, 34 - kH / 2, rw, kH};
    // (hovered, as the top bar's pages in Ballest Night: a black bubble inside a thin white edge, the words white,
    // growing a little from its middle; Lvcid)
    const double h = HoverCurve(key + ".reset.h", pmui::Over(pane, reset), kHoverIn, kHoverOut);
    const Rect g = reset.Grow(3 * h);
    pmui::Box(pane, key + ".reset.b", 3, g, A(kChip, 1 - h), g.h / 2)
        .Outline(BallestLight() ? 1.3f : 0.0f, A(kBlack, 0.4 * (1 - h)));      // (Ballest Light: a slight edge at rest: Lvcid)
    if (h > 0.01) {
        pmui::Bubble(pane, key + ".reset.bb", 3, g.Grow(kBubbleOut), g.h / 2 + kBubbleOut, h, 2);
        EdgeOutline(pane, key + ".reset.ring", 3, g, g.h / 2, 2, A(kWhite, 0.85 * h));
    }
    pmui::Text(pane, key + ".reset.t", 4, "reset to default", 18, "Regular", Mix(kTabHover, kWhite, h), g.Cx(), g.Cy(), 0.5);
    if (pmui::Entry* e = pmui::Find(pane, key + ".reset.t")) e->node.Transform(1 + 0.04 * h, 0);      // (grows with it: a scale, no twitch)
    if (pmui::Click(pane, reset, key + ".reset.btn")) gS.popup = popup;
    Tip(pane, key + ".reset", reset, std::string("resets this page only: ") + what);
}

// A theme's words beside its preview (Lvcid): a line, then four short ones centred, their colour words in their
// colours (null: the plain grey). Its colours are read each time (the limes follow the theme).
struct ThemePart {
    const char* text;
    const Color* colour;
};
struct ThemeWords {
    std::vector<ThemePart> line;                     // its one line (a colour word in its colour)
    std::vector<std::vector<ThemePart>> verse;       // and the short verse under it (none: just the line)
};
const ThemeWords& WordsOf(int theme) {
    static const ThemeWords kNight{{{"Ballest's own colors; after dark.", nullptr}},
                                   {{{"Leaderboard ", nullptr}, {"pink", &kPink}, {" and ", nullptr}, {"cyan", &kBallestCyan}, {",", nullptr}},
                                    {{"energizing hints of ", nullptr}, {"lime", &kNightLime}, {",", nullptr}},
                                    // ("bytes", not "bites", on purpose (Lvcid): a bite of apple red, the few small dots it
                                    // shows as, and memory, as the errors it marks)
                                    {{"reserved bytes of ", nullptr}, {"red", &kErrAccent}, {",", nullptr}},
                                    {{"& a touch of ", nullptr}, {"gold", &kBallestGold}, {".", nullptr}}}};
    static const ThemeWords kLucid{{{"Nightvision ", nullptr}, {"lime", &kLucidLime}, {" styled dark mode.", nullptr}}, {}};     // (Lvcid)
    static const ThemeWords kClassic{{{"The old plugins menu.", nullptr}},
                                     {{{"The menu from before:", nullptr}},
                                      {{"plain lists and buttons;", nullptr}},
                                      {{"its cog still opens", nullptr}},
                                      {{"these same settings.", nullptr}}}};
    static const ThemeWords kLight{{{"Ballest's own colors; in daylight.", nullptr}}, kNight.verse};
    return theme == 0 ? kNight : theme == 1 ? kLight : theme == 2 ? kLucid : kClassic;
}

// One line of parts, centred on cx: the plain words in `plain` (grey), the colour words bold in their colours.
void PartsLine(Layer& l, const std::string& key, int z, const std::vector<ThemePart>& parts, double px, double cx, double cy,
               const Color& plain = kSub) {
    std::vector<double> widths;
    double total = 0;
    for (const ThemePart& part : parts) {
        widths.push_back(pmui::Width(part.text, px, part.colour ? "Bold" : "Semilight"));
        total += widths.back();
    }
    double x = cx - total / 2;
    for (size_t i = 0; i < parts.size(); ++i) {
        pmui::Text(l, key + "." + std::to_string(i), z, parts[i].text, px, parts[i].colour ? "Bold" : "Semilight",
                   parts[i].colour ? *parts[i].colour : plain, x, cy);
        x += widths[i];
    }
}

// General (Lvcid, 2026-10-11): the plugin manager's own settings but advanced, on one page. DISPLAY & SOUND: a
// row each, a toggle on the left where it has one (smaller than elsewhere), its name, its slider and value; then
// THEME: the preview on the left, its dropdown on the right, the theme's words under the dropdown.
double General(Layer& pane, Layer& overlay, double w) {
    gNoteTips = true;
    double y = Header(pane, "general", "the plugin manager's window", w);
    PageButtons(pane, "ap", w, "reset.appearance", "window size, window scale, screen dim, hover sounds and the theme");
    y = GroupHead(pane, "ap.g", y, "DISPLAY & SOUND");
    constexpr double kTogX = 26, kLabX = 94, kSlX = 258, kSlW = 290, kValGap = 60, kRow = 44, kTogK = 0.85;
    // (Ballest Night and Light (Lvcid): the scale lime, the screen dim pink, the hover sounds cyan; Lucid Lime all lime)
    const bool ballest = BallestNight();
    const Color* pink = ballest ? &kPink : nullptr;
    const Color* cyan = ballest ? &kBallestCyan : nullptr;
    auto row = [&](const std::string& key, const char* label, const std::string& tip) {
        const double cy = y + kRow / 2;
        pmui::Text(pane, key + ".l", 3, label, 18, "Regular", kRowLabel, kLabX, cy);
        Tip(pane, key, {kTogX - 10, y, kSlX + kSlW + kValGap - kTogX + 10, kRow}, tip);
        return cy;
    };
    auto toggle = [&](const std::string& key, double cy, bool on, const Color* colour) {
        // (Ballest Light: as the cards' toggles, the dark knob and a white edge: Lvcid)
        const bool light = BallestLight();
        return Toggle(pane, key + ".tog", 3, kTogX, cy - 15 * kTogK, on, false, kTogK, colour, ballest && !light, false);
    };
    // window size
    {
        const double cy = row("winsize", "window size", "");
        pmui::Text(pane, "winsize.n", 3, "drag the window's edges or corners", 16, "Semilight", kSub, kSlX, cy);
        y += kRow;
    }
    // window scale: 80..150% in steps of 5 (by default the screen's: 110%, more on small screens); it changes when the slider is let go (the window would
    // move under the pointer while dragging: no test-drag either)
    {
        const double cy = row("scale", "window scale", "for small or very large screens: everything in this window (by default 110%, more on small screens)");
        static double pending = -1;
        const double shown = std::round((pending >= 0 ? pending : gPrefs.scale) / kScaleBase * 20) * 5;
        double st = (shown - 80) / 70;
        if (Slider(pane, "scale", {kSlX, cy - 2, kSlW, 4}, &st, std::to_string(static_cast<int>(shown)) + "%", kSlX + kSlW + kValGap, 1.0 / 14, false))
            pending = (80 + std::round(st * 14) * 5) / 100 * kScaleBase;
        if (pending >= 0 && !pmui::Held("scale.s")) {
            gPrefs.scale = pending;
            gPrefs.scaleChosen = true;
            pending = -1;
            PrefsChanged();
        }
        y += kRow;
    }
    // screen dim: on or off, and how dark (67% by default)
    {
        const double cy = row("dim", "screen dim", "how dark the game gets behind this window (on, 67% by default)");
        if (toggle("dim", cy, gPrefs.dimOn, pink)) {
            gPrefs.dimOn = !gPrefs.dimOn;
            PrefsChanged();
        }
        double t = gPrefs.dim / 0.99;
        if (Slider(pane, "dim", {kSlX, cy - 2, kSlW, 4}, &t, Percent(gPrefs.dim), kSlX + kSlW + kValGap, 0.01, true, pink)) {
            gPrefs.dim = std::round(t * 99) / 100;
            PrefsChanged();
        }
        y += kRow;
    }
    // hover sounds: on or off, and how loud (69% by default); the slider plays the sound as it moves
    {
        const double cy = row("tw.sound", "hover sounds",
                              "a soft click as the pointer comes onto a button or a card, and how loud (on, 69% by default; the footer's buttons follow it too)");
        if (toggle("tw.sound", cy, gPrefs.hoverSound, cyan)) {
            gPrefs.hoverSound = !gPrefs.hoverSound;
            PrefsChanged();
        }
        double vol = gPrefs.hoverVol;
        if (Slider(pane, "tw.vol", {kSlX, cy - 2, kSlW, 4}, &vol, Percent(vol), kSlX + kSlW + kValGap, 0.01, false, cyan)) {
            gPrefs.hoverVol = std::round(vol * 100) / 100;
            PrefsChanged();
        }
        // (its sound at each 1% it moves, at the new volume (Lvcid); at most every 45 ms, so a fast drag can't flood the
        // game's audio: a skipped step sounds on the next frame it can)
        static int lastPct = -1;
        static double lastAt = -100;
        const int pct = static_cast<int>(std::lround(gPrefs.hoverVol * 100));
        if (lastPct < 0) lastPct = pct;
        if (pct != lastPct && game::Seconds() - lastAt >= 0.045) {
            draw::UiSound(kHoverSound, gPrefs.hoverVol, 2.0, 0.11);
            lastAt = game::Seconds();
            lastPct = pct;
        }
        y += kRow;
    }
    gNoteTips = false;
    // the theme: the dropdown on the left, the theme's words under it, the verse's last line ending level with the
    // preview's bottom and the line between the dropdown and the verse; the preview on its right (Lvcid's flip)
    y = GroupHead(pane, "th.g", y + kGroupGap - 2, "THEME");
    const Rect drop{14, y + 6, 280, 38};
    const Rect pv{drop.R() + std::clamp(w - 14 - 280 - 425 - 16, 9.0, 56.0), y + 6, 425, std::floor(425.0 * 9 / 16) + 24};      // (less of a gap on a narrow window)
    const int shownTheme = gThemeHover >= 0 ? gThemeHover : CurrentTheme();
    ThemePreview(pane, pv, shownTheme);
    const double cx = drop.Cx();
    const ThemeWords& words = WordsOf(shownTheme);
    constexpr double kVerse = 23, kWordPx = 16;
    // (the line where it is with Night's four-line verse, verse or not: it doesn't move as the theme changes)
    constexpr double kVerseLines = 4;
    const double lastY = pv.B() - 22, firstY = lastY - kVerse * static_cast<double>(std::max<size_t>(words.verse.size(), 1) - 1);
    PartsLine(pane, "th.line", 3, words.line, kWordPx, cx, (drop.B() + lastY - kVerse * (kVerseLines - 1) - 12) / 2, kRowLabel);
    for (size_t i = 0; i < words.verse.size(); ++i)
        PartsLine(pane, "th.v" + std::to_string(i), 3, words.verse[i], kWordPx, cx, firstY + kVerse * static_cast<double>(i));
    ThemeRow(pane, overlay, drop);           // (after the words: its open list drops over them)
    return pv.B() + 20;
}

void ResetAppearance() {
    hostlog::Info("manager: general settings reset");
    gPrefs.dim = kDefaultDim;
    gPrefs.dimOn = true;
    gPrefs.scaleChosen = false;
    gPrefs.scale = ScreenDefaultScale();
    gPrefs.winW = gPrefs.winH = 0;
    gPrefs.hoverSound = true;
    gPrefs.hoverVol = kDefaultHoverVol;
    PrefsChanged();
    PickTheme(0);                           // (Ballest Night, the default)
}


// A popup's button: `back` at rest, white when hovered (its words then in `back`).
bool PopButton(Layer& l, const std::string& key, const Rect& r, const std::string& label, const Color& back, const Color& ink) {
    const double h = Ease(key, pmui::Over(l, r));
    pmui::Box(l, key + ".b", 3, r, Mix(back, kWhite, h), 13);
    pmui::Text(l, key + ".t", 4, label, 18, "Bold", Mix(ink, back.r + back.g + back.b < 0.2f ? kBlack : back, h), r.Cx(), r.Cy(), 0.5);
    return pmui::Click(l, r, key);
}

// "Reset?" (Lvcid): red to reset, grey to cancel; both white when hovered.
void ResetPopup(Layer& wl, double W, double H) {
    pmui::Level(kPopupLevel);
    const double t = PopupIn();
    pmui::PreferFocus("pop.no");            // (a reset starts on cancel)
    const Rect b{std::floor(W / 2 - 300), std::floor(H / 2 - 125), 600, 250};
    {
        ux::RawColors raw;                      // (a dark veil in every theme)
        pmui::Box(wl, "pop.dim", 900, {0, 0, W, H}, A(kBlack, kFocusDim * t), 24);
    }
    Glow(wl, "pop.shadow", 901, b, kBlack, 0.8 * t);
    Layer& pop = pmui::Group(wl, "pop", 902, b, false, t);
    pmui::Box(pop, "ring", 0, {-2, -2, b.w + 4, b.h + 4}, kPopupRing, 24);
    pmui::Box(pop, "bg", 1, {0, 0, b.w, b.h}, kPopup, 22).Outline(2, kBlack);
    // (this page's settings only, said exactly; Lvcid)
    pmui::Text(pop, "title", 2, "reset general?", 28, "Regular", kWhite, 28, 46);
    pmui::Para(pop, "text", 2,
               "The window size, window scale (110%, more on small screens), screen dim (on, 67%), hover sounds (on, 69%) and the theme (Ballest Night) go "
               "back to their defaults.",
               17, "Semilight", kPopupText, {28, 76, b.w - 56, 70});
    pmui::Text(pop, "text2", 2, "Nothing on the other pages changes.", 15, "Semilight", kSub, 28, 150);
    const double rw = PillWidth(pop, "pop.yes", "Reset", 18, nullptr, 24) + 10, cw = PillWidth(pop, "pop.no", "Cancel", 18, nullptr, 24);
    const bool yes = PopButton(pop, "pop.yes", {b.w - 28 - rw, 172, rw, 50}, "Reset", kRemoveRed, kWhite);
    const bool no = PopButton(pop, "pop.no", {b.w - 28 - rw - 12 - cw, 172, cw, 50}, "Cancel", ux::Hex("#2A2C31"), kRowLabel);
    if (yes) {
        ClosePopup();
        ResetAppearance();
    } else if (no || pmui::PressedOutside(pop, {0, 0, b.w, b.h})) {
        ClosePopup();
    }
}

// --- advanced -------------------------------------------------------------------------------------------------------------
// The disk: what the game, the plugin manager, the plugins and the cosmetics take, measured off the game's thread.
struct DiskNode {
    std::string name;
    uint64_t bytes = 0;
    std::vector<DiskNode> kids;
};
std::mutex gDiskLock;
std::vector<DiskNode> gDisk;                // (under gDiskLock)
std::atomic<int> gDiskState{0};             // 0 not measured, 1 measuring, 2 ready
std::set<std::string> gDiskOpen = {"plugins folder"};

uint64_t FolderBytes(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += FolderBytes(dir + L"\\" + n);
        else total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}
// Each folder in `dir` as a node (largest first).
std::vector<DiskNode> Subfolders(const std::wstring& dir) {
    std::vector<DiskNode> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        out.push_back({eng::Narrow(n.c_str(), static_cast<int>(n.size())), FolderBytes(dir + L"\\" + n), {}});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const DiskNode& a, const DiskNode& b) { return a.bytes > b.bytes; });
    return out;
}
uint64_t FileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

void MeasureDisk() {
    if (gDiskState == 1) return;
    gDiskState = 1;
    const std::wstring pluginsDir = plugins::Dir(), win64 = pluginsDir.substr(0, pluginsDir.find_last_of(L'\\'));
    const std::wstring data = hostlog::DataDir(), cosmeticPlugins = plugins::CosmeticsDir();
    std::thread([win64, pluginsDir, data, cosmeticPlugins] {
        std::wstring game = win64;
        for (int i = 0; i < 3; ++i) game = game.substr(0, game.find_last_of(L'\\'));     // (Win64 -> Binaries -> Ballest -> the game)
        std::vector<DiskNode> out;
        out.push_back({"game", FolderBytes(game), {}});
        DiskNode pm{"plugin manager", FileBytes(win64 + L"\\version.dll"), {}};
        pm.kids.push_back({"version.dll", pm.bytes, {}});
        uint64_t own = 0;
        for (DiskNode& n : Subfolders(data)) own += n.bytes, pm.kids.push_back(n);     // (with cosmetics: loadouts, settings)
        pm.bytes += own;
        out.push_back(std::move(pm));
        DiskNode folder{"plugins folder", 0, {}};
        DiskNode pl{"plugins", 0, {}};
        for (DiskNode& n : Subfolders(pluginsDir))
            if (n.name != "cosmetics") pl.bytes += n.bytes, pl.kids.push_back(std::move(n));
        DiskNode cos{"cosmetics", 0, Subfolders(cosmeticPlugins)};      // the cosmetic plugins (plugins\cosmetics)
        for (const DiskNode& n : cos.kids) cos.bytes += n.bytes;
        folder.bytes = pl.bytes + cos.bytes;
        folder.kids.push_back(std::move(pl));
        folder.kids.push_back(std::move(cos));
        out.push_back(std::move(folder));
        {
            std::lock_guard<std::mutex> g(gDiskLock);
            gDisk = std::move(out);
        }
        gDiskState = 2;
    }).detach();
}

std::string Bytes(uint64_t b) {
    char buf[32];
    const double mb = static_cast<double>(b) / (1024.0 * 1024.0);
    if (mb >= 1024) std::snprintf(buf, sizeof buf, "%.2f GB", mb / 1024);
    else if (mb >= 10) std::snprintf(buf, sizeof buf, "%.0f MB", mb);
    else std::snprintf(buf, sizeof buf, "%.1f MB", mb);
    return buf;
}

// The disk's list: its own scrolling box (the wheel on it scrolls it alone, not the page: Lvcid); a folder with more in
// it opens and closes with a click; its height dragged by the grip on its bottom edge (kept).
double DiskBox(Layer& pane, double y, double w) {
    if (gDiskState == 0) MeasureDisk();
    // (tight: from the names' edge to the controls' right edge, as general's rows: Lvcid)
    (void)w;
    // (its height: as dragged, else just tall enough for what it shows: no scroll bar of its own then; Lvcid)
    const double fit = std::clamp(gContentH.count("disk") ? gContentH["disk"] + 8 : 260.0, 120.0, 900.0);
    const double bw = TightRight() - (kTightLabelX - 16), bh = gPrefs.diskH > 0 ? gPrefs.diskH : fit;
    const Rect box{kTightLabelX - 16, y, bw, bh};
    pmui::Box(pane, "disk.bg", 1, box, kCard, 18).Outline(2, kTrim);
    // the grip: a short bar on the bottom edge; dragged, the box grows or shrinks (120..900; never dragged: it fits what it shows); drawn last, so its press is
    // its own and not the list's
    auto grip = [&] {
        static double startY = 0, startH = 0;
        const Rect gr{box.Cx() - 60, box.B() - 9, 120, 18};
        const bool held = pmui::Held("disk.grip");
        const double gh = Ease("disk.grip", pmui::Over(pane, gr) || held);
        pmui::Box(pane, "disk.grip.b", 4, {box.Cx() - 24, box.B() - 4, 48, 4}, Mix(ux::Hex("#3A3D44"), kLime, gh), 2);
        if (pmui::Over(pane, gr) && pmui::Mouse().pressed) {
            startY = pmui::Mouse().y;
            startH = bh;
        }
        pmui::Click(pane, gr, "disk.grip", false);
        if (held) {
            gPrefs.diskH = std::clamp(startH + pmui::Mouse().y - startY, 120.0, 900.0);
            PrefsChanged();
        }
    };
    if (gDiskState != 2) {
        pmui::Text(pane, "disk.wait", 3, "measuring...", 16, "Semilight", kSub, box.Cx(), box.Cy(), 0.5);
        grip();
        return y + bh;
    }
    std::vector<DiskNode> disk;
    {
        std::lock_guard<std::mutex> g(gDiskLock);
        disk = gDisk;
    }
    Layer& in = pmui::Group(pane, "disk.in", 2, box.Grow(-4), true);
    // (the wheel is the box's only while it has more than it shows: fitted, the page scrolls under the pointer)
    if (gContentH["disk"] > box.h - 8 + 1) pmui::WheelZone(in, {0, 0, box.w - 8, box.h - 8});
    const double scroll = Scrolled("disk", in, {0, 0, box.w - 8, box.h - 8}, gContentH["disk"]);
    double ly = 8 - scroll;
    uint64_t biggest = 1;
    for (const DiskNode& n : disk) biggest = std::max(biggest, n.bytes);
    std::function<void(const DiskNode&, int, const std::string&)> line = [&](const DiskNode& n, int depth, const std::string& path) {
        const std::string key = "disk." + path;
        const Rect r{8, ly, box.w - 24, 32};
        const bool kids = !n.kids.empty(), open = kids && gDiskOpen.count(path);
        const double h = Ease(key, pmui::Over(in, r));
        pmui::Box(in, key + ".h", 1, r, A(ux::Hex("#1C1D21"), h), 8);
        const double x = r.x + 10 + depth * 22;
        if (kids) pmui::Pic(in, key + ".c", 2, {x, r.Cy() - 7, 14, 14}, "pm-chev.png", kSub).Transform(1, open ? 0 : -90);
        const double nx = x + 22;
        pmui::Text(in, key + ".n", 2, n.name, depth == 0 ? 17 : 15.5, depth == 0 ? "Regular" : "Semilight", depth == 0 ? kText : kRowLabel, nx, r.Cy());
        const double bx = r.x + r.w * 0.5, bl = r.w * 0.5 - 110;
        pmui::Box(in, key + ".bar", 2, {bx, r.Cy() - 3, bl, 6}, kChip, 3);
        pmui::Box(in, key + ".fill", 3, {bx, r.Cy() - 3, std::max(3.0, bl * static_cast<double>(n.bytes) / static_cast<double>(biggest)), 6}, A(kNuLime, 0.75), 3);
        pmui::Text(in, key + ".b", 2, Bytes(n.bytes), 15, "Semilight", kSub, r.R() - 8, r.Cy(), 1);
        if (kids && pmui::Click(in, r, key)) {
            if (open) gDiskOpen.erase(path);
            else gDiskOpen.insert(path);
        }
        ly += 34;
        if (open)
            for (const DiskNode& k : n.kids) line(k, depth + 1, path + "/" + k.name);
    };
    for (const DiskNode& n : disk) line(n, 0, n.name);
    gContentH["disk"] = ly + scroll + 8;
    ScrollBar(in, "disk", 20, {box.w - 16, 6, 4, box.h - 20}, box.h - 8, kLime);
    grip();
    return y + bh;
}

// A debug test (Lvcid): its toggle first, then its name; the whole line hovered shows what it does.
// (tight, as general's rows: Lvcid; its toggle, then its name, no box; the whole line clicks it; hovered, the name
// white; what it does a tooltip)
bool TestRow(Layer& pane, const std::string& key, double y, double w, const std::string& label, const std::string& tip, bool on) {
    (void)w;
    const Rect r{kTightLabelX - 16, y, TightRight() - kTightLabelX + 16, kTightRowH};
    const double h = Ease(key + ".h", pmui::Over(pane, r));
    bool clicked = pmui::Click(pane, r, key);
    clicked = Toggle(pane, key + ".tog", 3, kTightLabelX, r.Cy() - 15, on, false) || clicked;
    pmui::Text(pane, key + ".l", 3, label, 18, "Regular", Mix(kRowLabel, kWhite, h), kTightLabelX + 54 + 16, r.Cy());
    Tip(pane, key, r, tip);
    gRowH = kTightRowH;
    return clicked;
}

// One line of the stats: what, and its value.
// (tight, as general's rows: Lvcid; its name where the names are, its value in the control column, no lines)
double Stat(Layer& pane, const std::string& key, double y, double w, const std::string& what, const std::string& value) {
    (void)w;
    pmui::Text(pane, key + ".k", 3, what, 16, "Semilight", kSub, kTightLabelX, y + 15);
    pmui::Text(pane, key + ".v", 3, value, 16, "Regular", kText, TightCol(), y + 15);
    return y + 30;
}

std::string GameBuild() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(exe, GetFileExInfoStandard, &a)) return "?";
    SYSTEMTIME st;
    FileTimeToSystemTime(&a.ftLastWriteTime, &st);
    char buf[48];
    std::snprintf(buf, sizeof buf, "build of %04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}

// A group's heading on the settings pages that folds (Lvcid, 2026-10-11: advanced's sections): a click on it folds the
// group (and opens it again), kept with the lists' folds (manager.folded, "settings:<heading>"); hovered, the lists'
// faint bevel and the chevron; folded, the chevron points on. True while it's open; `y` moves past it either way.
// `startFolded`: folded until it's opened (kept as "settings:open:<heading>" then).
bool FoldGroup(Layer& pane, const std::string& key, double& y, double w, const std::string& title, const std::string& chip = "",
               bool startFolded = false) {
    if (!gFoldedRead) ReadFolded();
    const std::string f = startFolded ? "settings:open:" + title : "settings:" + title;
    const bool folded = (gFolded.count(f) > 0) != startFolded;
    const Rect hr{-6, y - 5, std::min(w, kTightCtlX + kTightCtlW) + 12, 30};
    const double fh = Ease(key + ".fold", pmui::Over(pane, hr), 0.08);
    if (fh > 0.01) {
        pmui::Box(pane, key + ".bevel", 1, hr, A(kWhite, 0.022 * fh), 10).Outline(1, A(kWhite, 0.05 * fh));
        pmui::Box(pane, key + ".bevel.t", 1, {hr.x + 10, hr.y + 1, hr.w - 20, 1}, A(kWhite, 0.05 * fh), 0.5);
    }
    const double next = GroupHead(pane, key, y, title, chip);
    if (folded || fh > 0.01)
        pmui::Pic(pane, key + ".chev", 3, {hr.R() - 28, y + 4, 12, 12}, "pm-chev.png", A(kHead, folded ? 0.55 + 0.45 * fh : 0.55 * fh))
            .Transform(1, folded ? -90 : 0);
    if (pmui::Click(pane, hr, key + ".fold")) {
        if (!gFolded.erase(f)) gFolded.insert(f);
        SaveFolded();
        hostlog::Info("manager: " + std::string(folded ? "opened " : "folded ") + f);
    }
    y = folded ? next + 4 : next;
    return !folded;
}

double Advanced(Layer& pane, Layer& overlay, double w) {
    gNoteTips = true;
    double y = Header(pane, "advanced", "for the curious, plugin makers and testing", w);
    // (the control column from every name on the page, folded ones too: it doesn't jump when DEBUG is opened)
    for (const char* name : {"measure again", "console", "logs folder", "all tests off", "its update", "plugins that need it", "restart notice"}) {
        double& col = gTightCol[gTightPage];
        col = std::max(col, kTightLabelX + pmui::Width(name, 18, "Regular") + 24);
    }
    if (FoldGroup(pane, "ad.stats", y, w, "STATS FOR NERDS")) {
        int running = 0, cosmetics = 0;
        for (const Plug& p : gInstalled) running += p.enabled && p.status == "running";
        cosmetics = static_cast<int>(packages::Items().size());
        const int up = static_cast<int>(game::Seconds());
        double sw = 1920, sh = 1080;
        ux::ScreenSize(&sw, &sh);
        const std::string rs = registry::State();
        y = Stat(pane, "st.game", y, w, "game", GameBuild());
        y = Stat(pane, "st.pm", y, w, "plugin manager", plugins::kHostVersion);
        y = Stat(pane, "st.plugins", y, w, "plugins", std::to_string(gInstalled.size()) + " installed  \xC2\xB7  " + std::to_string(running) + " running");
        y = Stat(pane, "st.cos", y, w, "cosmetics", std::to_string(cosmetics));
        y = Stat(pane, "st.reg", y, w, "registry", (rs.empty() ? "not loaded" : rs) + "  \xC2\xB7  " + std::to_string(registry::Entries().size()) + " plugins listed");
        y = Stat(pane, "st.script", y, w, "script engine", std::string("AngelScript ") + ANGELSCRIPT_VERSION_STRING);
        y = Stat(pane, "st.screen", y, w, "screen", std::to_string(static_cast<int>(sw)) + " x " + std::to_string(static_cast<int>(sh)));
        y = Stat(pane, "st.up", y, w, "game running for", std::to_string(up / 3600) + " h " + std::to_string(up / 60 % 60) + " min");
        y += kGroupGap;
    }
    if (FoldGroup(pane, "ad.console", y, w, "CONSOLE & LOGS")) {
        if (ButtonRow(pane, "c.open", y, w, "console", "the log as it's written (what the host and every plugin report), and the host's commands", "open", true))
            OpenOverlay("console");
        y += Step();
        if (ButtonRow(pane, "c.folder", y, w, "logs folder", "host.log (send it with a bug report), the plugin manager's own files, and registry_url.txt for another registry", "open"))
            OpenDir(hostlog::DataDir());
        y += Step() + kGroupGap;
    }
    if (FoldGroup(pane, "ad.disk", y, w, "DISK ALLOCATION")) {
        y = DiskBox(pane, y, w) + 8;
        if (ButtonRow(pane, "ad.measure", y, w, "measure again", "after adding or removing plugins or cosmetics", "measure")) MeasureDisk();
        y += Step() + kGroupGap;
    }
    // DEBUG (Lvcid: tidied, 2026-10-10): pretend states, to check the look (nothing is changed, downloaded or saved),
    // in three small groups; how many are on in its heading, and one button turns them all off.
    const int testsOn = gTestFirstOpen + (gTestHost > 0) + (gTestNeedsHost > 0) + gTestUpdates + gTestUpdate + (gTestError || gTestToggles) + gTestNew;
    if (FoldGroup(pane, "ad.debug", y, w, "DEBUG", testsOn ? std::to_string(testsOn) + " on" : "", true)) {     // (folded at first: Lvcid)
        auto sub = [&](const std::string& key, const std::string& title) {
            pmui::Text(pane, key, 3, title, 12, "Bold", kHead, 16, y + 10);
            y += 28;
        };
        if (ButtonRow(pane, "t.reset", y, w, "all tests off", "Turns every test below off at once.", "reset")) {
            gTestFirstOpen = gTestUpdates = gTestUpdate = gTestError = gTestToggles = gTestNew = false;
            gTestHost = gTestNeedsHost = 0;
            gTestHostAt = -100;
            gS.bannerLater = false;
            gBannerUpdateClicked = false;
            gModelAt = -100;
            hostlog::Info("manager: every DEBUG test off");
        }
        y += Step() + 6;
        sub("t.h.welcome", "WELCOME");
        if (TestRow(pane, "t.first", y, w, "welcome on every open", "Each open counts as the first since an update: the logo's intro plays.", gTestFirstOpen))
            gTestFirstOpen = !gTestFirstOpen;
        y += Step() + 6;
        sub("t.h.host", "PLUGIN MANAGER UPDATE");
        ChoiceRow(pane, overlay, "t.host", y, w, "its update",
                  "A pretend 9.9.9 in each stage: the banner, update all, the locks and the restart notice follow it. Nothing is downloaded.",
                  {"off", "out", "downloading", "installed (restart)", "failed"}, gTestHost, [](int n) {
                      gTestHost = n;
                      gTestHostAt = -100;
                      gS.bannerLater = false;
                      gBannerUpdateClicked = false;
                      gModelAt = -100;
                  });
        y += Step();
        ChoiceRow(pane, overlay, "t.needs", y, w, "plugins that need it",
                  "The first browse plugin, the first update, the first pack not got and a missing dependency need a newer plugin manager: 9.9.9 (within the pretend update) or 99.0.0 (not out yet).",
                  {"off", "within the update", "not out yet"}, gTestNeedsHost, [](int n) { gTestNeedsHost = n; });
        y += Step();
        if (ButtonRow(pane, "t.notice", y, w, "restart notice", "Shows it again the next time the update is installed (or pretended installed).", "again"))
            gRestartNoticed = false;
        y += Step() + 6;
        sub("t.h.rows", "PLUGINS AND COSMETICS");
        if (TestRow(pane, "t.updates", y, w, "pretend updates", "The first three plugins that can update show one (update only says so in the console).", gTestUpdates)) {
            gTestUpdates = !gTestUpdates;
            gModelAt = -100;
        }
        y += Step();
        if (TestRow(pane, "t.update", y, w, "update pill animations", "The Plugin Manager's row offers a pretend update: its pill plays updating, failed and gone.", gTestUpdate))
            gTestUpdate = !gTestUpdate;
        y += Step();
        if (TestRow(pane, "t.errors", y, w, "errors and warnings", "The Plugin Manager's row shows a pretend load error; the first plugin you can switch, a warning (gold); the second, an error (red).", gTestError || gTestToggles)) {
            const bool on = !(gTestError || gTestToggles);
            gTestError = gTestToggles = on;
            gModelAt = -100;
        }
        y += Step();
        if (TestRow(pane, "t.new", y, w, "NEW tags", "Every plugin on browse, and every pack on explore, shows its NEW tag.", gTestNew)) gTestNew = !gTestNew;
        y += Step();
    }
    gNoteTips = false;
    return y;
}

std::string Formatted(const settings::Setting& s, double v) {
    if (s.kind == settings::Kind::Int || s.kind == settings::Kind::UInt) return std::to_string(std::llround(v));
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.3f", v);
    std::string out = buffer;
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
    return out;
}

double PluginSection(Layer& pane, Layer& overlay, double w, const std::string& id) {
    const Plug* p = nullptr;
    for (const Plug& q : gInstalled)
        if (q.id == id) p = &q;
    if (!p) return Header(pane, id, "not installed any more", w);
    // its heading: icon, name, by and version (the icon glows when a row's cog led here)
    const double since = pmui::Now() - gS.flashAt, flash = gS.flash == "header" ? std::max(0.0, 1 - since / 1.4) : 0;
    Glow(pane, "p.tileglow", 2, {0, 6, 60, 60}, kLime, 0.6 * flash);
    Tile(pane, "p.tile", 3, {0, 6, 60, 60}, p->icon, 15, 3, 0, !p->enabled);
    Header(pane, "", "", w);
    pmui::Text(pane, "p.name", 3, p->name, 34, "Regular", kWhite, 78, 28);
    pmui::Text(pane, "p.by", 3, (p->author.empty() ? "" : "by " + p->author + "  \xC2\xB7  ") + p->version + (p->enabled ? "" : "  \xC2\xB7  off"), 17,
               "Semilight", kSub, 78, 60);
    double y = 112;
    bool any = false;
    if (ui::HasMovable(id)) {
        any = true;
        if (ButtonRow(pane, "p.position", y, w, "window position", "drag its windows anywhere while the cursor shows", "reset")) ui::ResetPositions(id);
        y += Step();
    }
    const std::vector<settings::Setting>& list = settings::List();
    for (size_t i = 0; i < list.size(); ++i) {
        const settings::Setting& s = list[i];
        if (s.pluginId != id || s.hidden) continue;
        any = true;
        const std::string key = "set." + s.variable, label = s.name.empty() ? s.variable : s.name, value = settings::Get(i);
        if (s.kind == settings::Kind::Bool) {
            if (ToggleRow(pane, key, y, w, label, s.description, value == "true")) settings::Set(i, value == "true" ? "false" : "true");
        } else if (!s.choices.empty()) {
            const auto at = std::find(s.choices.begin(), s.choices.end(), value);
            const std::vector<std::string> choices = s.choices;
            ChoiceRow(pane, overlay, key, y, w, label, s.description, choices, at == s.choices.end() ? -1 : static_cast<int>(at - s.choices.begin()),
                      [i, choices](int n) { settings::Set(i, choices[static_cast<size_t>(n)]); });
        } else if (s.kind != settings::Kind::String && s.hasRange && s.max > s.min) {
            double t = std::clamp((std::atof(value.c_str()) - s.min) / (s.max - s.min), 0.0, 1.0);
            // (no test-drag: a plugin's setting is saved as it changes)
            if (SliderRow(pane, key, y, w, label, s.description, &t, value, 0.01, false)) settings::Set(i, Formatted(s, s.min + t * (s.max - s.min)));
        } else {
            TextRow(pane, key, y, w, label, s.description, value, [i, label](const std::string& typed) {
                if (!settings::Set(i, typed)) hostlog::Warn("manager: not a value for " + label + ": " + typed);
            });
        }
        if (!settings::IsDefault(i)) {
            if (IconButton(pane, key + ".reset", 4, {LabelW(w) - 46, y + 11, 34, 34}, "pm-refresh.png", 0, 10)) settings::Reset(i);
        }
        y += Step();
    }
    if (!any) pmui::Text(pane, "p.none", 3, "This plugin has no settings.", 17, "Semilight", kSub, 0, y + 20);
    return y + 40;
}

// One line of text clipped to r. A longer one scrolls quickly (eased) to show its end while `hover`, and back after;
// at rest its end fades into `mask` (what is behind it).
void ScrollText(Layer& l, const std::string& key, int z, const std::string& s, double px, const char* face, const Color& c, const Rect& r,
                bool hover, const Color& mask) {
    const double over = std::max(0.0, pmui::Width(s, px, face) - r.w + 6);
    Anim& a = gAnim[key + ".scroll"];
    a.value = ux::Approach(a.value, hover && over > 0 ? 1.0 : 0.0, pmui::Dt(), 0.12 + over / 900);
    Layer& g = pmui::Group(l, key, z, r, true);
    pmui::Text(g, "t", 0, s, px, face, c, -over * Smooth(a.value), r.h / 2);
    if (over > 0) pmui::Pic(l, key + ".mask", z + 1, {r.R() - 36, r.y, 36, r.h}, "pm-fade-right.png", A(mask, 1 - a.value));
}


// A nav item, a pill (Lvcid, 2026-10-11): the lime bubble when open; hovered, as the top bar's pages in Ballest Night
// (both themes): a black bubble inside a thin white edge, the words white; and as a card of the compact list, its left
// end lit in lime with the glow and smoke drifting in, hanging out over the window (SideAccent on gFx).
bool NavItem(Layer& nav, Layer& /*o*/, const std::string& key, double y, const std::string& label, const char* icon, Obj texture, bool on, bool dim,
             bool dot) {
    const Rect r{22, y, 330 - 22 - 16, 46};
    const double round = r.h / 2;
    const bool over = pmui::Over(nav, r);
    const double h = EaseUpDown(key, over && !on, kEase * 0.6, 0.12);
    if (on) Glow(nav, key + ".glow", 1, r, kLime, 0.45);
    // (two keys: a bubble's material is fixed when it's first made, so the open one and the hover's black are apart)
    if (on) pmui::Bubble(nav, key + ".on", 2, r, round, 1);
    else if (h > 0.01) {
        // (Ballest Light, on the blue: a white bubble with dark bubbles inside a black edge, its words black; Lvcid)
        const bool light = BallestLight();
        pmui::Bubble(nav, key + (light ? ".ink" : ".bub"), 2, r.Grow(kBubbleOut), round + kBubbleOut, h, light ? 4 : 2);
        EdgeOutline(nav, key + ".ring", 3, r, round, 2, A(light ? kBlack : kWhite, 0.85 * h));
    }
    if (!on && gFx) SideAccent(nav, key, r, over, round, *gFx);
    const Rect ir{r.x + 12, r.Cy() - 12, 24, 24};
    // (Ballest Light, on the blue: the names white, as the game's main menu, a turned-off plugin's softer; black on the
    // white hover bubble. Night: grey, white hovered)
    const Color ink = on ? kBlack
                    : BallestLight() ? Mix(A(kWhite, dim ? 0.55 : 1.0), kBlack, h)
                    : CurrentTheme() == 0 ? Mix(A(kWhite, dim ? 0.5 : 0.82), kWhite, h)      // (a touch dimmed at rest: the hover's white stands out)      // (Ballest Night, on its halftone: white, Lvcid)
                                     : Mix(dim ? kHead : kSub, kWhite, h);
    if (icon) pmui::Pic(nav, key + ".i", 4, ir, icon, ink);
    else pmui::Texture(nav, key + ".i", 4, ir, texture, dim ? Color{0.4f, 0.4f, 0.4f, 1} : kWhite, 5);
    // a name too long for the item scrolls to its end while hovered
    // (Ballest Light: the white names with a black drop shadow, as the main menu's; gone as the hover turns them black)
    std::optional<pmui::TextShadowScope> shade;
    if (BallestLight() && !on) shade.emplace(Color{0, 0, 0, static_cast<float>(0.75 * (1 - h))});
    ScrollText(nav, key + ".name", 4, label, 19, "Regular", ink, {r.x + 48, r.y, r.w - 48 - (dot ? 30 : 12), r.h}, over,
               on ? ux::Hex("#B9EE10") : BallestLight() ? Mix(kBallestBlue, ux::Hex("#F4F4F5"), h) : Mix(CurrentTheme() == 0 ? kNightNavBg : kOverlay, kBlack, h));      // (its end fading into what's behind it)
    if (dot && DotShown(key + ".dot@" + std::to_string(gUpdates), over)) pmui::Box(nav, key + ".dot", 5, {r.R() - 22, r.Cy() - 4, 8, 8}, kRed, 4);
    return pmui::Click(nav, r, key);
}

// The settings menu's headings: grey, or (Ballest Light, on the blue) a soft white.
Color NavHead() { return BallestLight() ? Mix(kWhite, kBallestBlue, 0.3) : kHead; }

void SideNav(Layer& o, double height) {
    const Rect view{0, 0, 330, height};
    Layer& nav = pmui::Group(o, "nav", 2, view, true);
    const double scroll = Scrolled("nav", nav, view, gContentH["nav"]);
    double y = 22 - scroll;
    Tick(nav, "h1.bar", 1, {24, y + 1, 3, 14});      // (the lime line, as the list's headings: Lvcid)
    pmui::Text(nav, "h1", 1, "PLUGIN MANAGER", 13, "Bold", NavHead(), 36, y + 8);
    y += 26;
    struct Own {
        const char *section, *label, *icon;
    };
    static const Own kOwn[] = {{"general", "general", "pm-sliders.png"}, {"advanced", "advanced", "pm-shield.png"}};
    for (const Own& own : kOwn) {
        const std::string section = own.section;
        const bool dot = false;
        if (NavItem(nav, o, "nav." + section, y, own.label, own.icon, nullptr, gS.section == section, false, dot)) OpenOverlay("settings", section);
        y += 50;
    }
    y += 12;
    Rule(nav, "sep", 1, {30, y, 330 - 60, 2}, kPopupRing);
    y += 22;
    Tick(nav, "h2.bar", 1, {24, y + 1, 3, 14});
    pmui::Text(nav, "h2", 1, "PLUGIN SETTINGS", 13, "Bold", NavHead(), 36, y + 8);
    y += 26;
    // the plugins with settings: those turned on first, then those turned off
    std::vector<const Plug*> withSettings;
    for (const Plug& p : gInstalled)
        if (p.settings && p.id != kSelf) withSettings.push_back(&p);   // (the plugin manager: its own category above)
    std::stable_partition(withSettings.begin(), withSettings.end(), [](const Plug* p) { return p->enabled; });
    for (const Plug* pp : withSettings) {
        const Plug& p = *pp;
        Obj texture = p.icon.empty() ? nullptr : ux::LoadPicture(eng::Widen(p.icon));
        if (!texture) texture = ux::LoadPicture(eng::Widen(registry::DefaultIcon()));
        if (NavItem(nav, o, "nav.p." + p.id, y, p.name, nullptr, texture, gS.section == "plugin:" + p.id, !p.enabled, false))
            OpenOverlay("settings", "plugin:" + p.id);
        y += 50;
    }
    gContentH["nav"] = y + scroll + 20;
    ScrollBar(nav, "nav", 20, {330 - 10, 8, 4, height - 16}, height);
}

// An overlay's foot: its corners as round as the window's edge (pm-edge-ring.png), and drawn a little in from the
// window's sides and bottom, under the edge (its corners poked out past the edge's curve: Lvcid).
constexpr double kFootRound = 40, kFootIn = 1.5;      // (30 still poked out at the bottom left: Lvcid; more inset showed a sliver of the window by the blue)
Rect FootRect(const Rect& area) { return {kFootIn, 0, area.w - 2 * kFootIn, area.h - kFootIn}; }
void SettingsOverlay(Layer& wl, const Rect& area, double t) {
    Layer& o = pmui::Group(wl, "settings", 500, area, true, t);
    // (its foot's corners as round as the window's edge picture's (24 came out squarer, poking past it: Lvcid))
    pmui::BoxCorners(o, "bg", -1, FootRect(area), kOverlay, 0, 0, kFootRound, kFootRound);
    if (BallestLight()) {
        // (Ballest Light: the page's side over a 20% wash of the light halftone, on its own colour (Lvcid); in from
        // the edges, its square corners inside the window's curve)
        const Rect side{334, 0, area.w - 334 - 8, area.h - 8};
        pmui::MaterialOf(o, "bg.halftone", 0, side, ux::HalftoneMaterial(1.25f), 16, 0.2);
    } else if (CurrentTheme() == 0) {
        // (Ballest Night (Lvcid's pick): the menu on a soft step lighter than the window (shows about
        // #16171A), its names white; the page's side on the dark halftone, fully (its box the window's colour, so its
        // edges melt into it)
        pmui::BoxCorners(o, "nav.bg", 0, {kFootIn, 0, 330 - kFootIn, area.h - kFootIn}, kNightNavBg, 0, 0, 0, kFootRound);
        ux::Node& ht = pmui::MaterialOf(o, "side.halftone", 0, {334, 0, area.w - 334 - 8, area.h - 8}, ux::HalftoneMaterial(1.25f, true), 16, 1.0);
        // (upside down: its big dots in the bottom right, the page's empty space under its last rows, away from its
        // title and its reset button: Lvcid)
        // (set once, when its widget is made: it keeps its transform; no animation needs it again)
        static eng::Weak flipped;
        if (Obj img = ht.Get(); img && img != eng::Get(flipped)) {
            eng::Call(img, "SetRenderTransformPivot", w::Vec2{0.5, 0.5});
            eng::Call(img, "SetRenderScale", w::Vec2{1, -1});
            flipped = eng::MakeWeak(img);
        }
    }
    Rule(o, "edge", 1, {330, 0, 2, area.h}, kSideEdge);
    if (BallestLight()) {
        // (Ballest Light: the settings menu on ballest blue, its items as Night draws them over it: white words, the
        // black bubble hover, the lime open one; Lvcid)
        ux::RawColors raw;
        pmui::BoxCorners(o, "nav.blue", 0, {kFootIn, 0, 330 - kFootIn, area.h - kFootIn}, kBallestBlue, 0, 0, 0, kFootRound);
        SideNav(o, area.h);
    } else {
        SideNav(o, area.h);
    }
    gChoices.clear();
    // (any other: general; "appearance", from before the theme had a page of its own, is general now)
    // (its own settings, and the old personalize page: general, where the theme is now: Lvcid)
    if (gS.section == "plugin:" + std::string(kSelf)) gS.section = "general";
    if (gS.section != "advanced" && gS.section.rfind("plugin:", 0) != 0) gS.section = "general";
    const double px = 360, pw = area.w - px - 84;
    const Rect view{px, 0, pw + 40, area.h};
    Layer& pv = pmui::Group(o, "pane", 3, view, true);
    const std::string key = "pane." + gS.section;
    const double scroll = Scrolled("pane", pv, {0, 0, view.w, view.h}, gContentH[key]);
    const double fade = Smooth((pmui::Now() - gS.sectionAt) / 0.18);
    // (no back button above the heading any more: the input legend at the overlay's foot says how, Lvcid)
    gBackOff = 0;
    Layer& pane = pmui::Group(pv, "c." + gS.section, 0, {10, 26 - scroll + (1 - fade) * 8, pw, std::max(view.h, gContentH[key])}, false, fade);
    double end = 0;
    if (gS.section == "general") end = General(pane, o, pw);
    else if (gS.section == "advanced") end = Advanced(pane, o, pw);
    else if (gS.section.rfind("plugin:", 0) == 0) end = PluginSection(pane, o, pw, gS.section.substr(7));
    gContentH[key] = end + 60 + gBackOff;
    // (at the window's right edge as the lists' (it sat 34 in, inside the page's column: Lvcid), as wide as theirs)
    ScrollBar(o, "pane", 20, {area.w - 20, 8, 8, view.h - 16}, view.h);
    // the open dropdown's list, over everything
    bool drawn = false;
    for (ChoiceList& c : gChoices) {
        if (c.key != gS.choiceOpen) continue;
        drawn = true;
        if (c.custom) continue;
        pmui::Level(kDropLevel);
        Rect list{c.r.x, c.r.B() + 6, c.r.w, 12 + 40.0 * static_cast<double>(c.options.size())};
        if (pmui::OnScreen(o, list).B() > o.clip.B() - 8) list.y = c.r.y - 6 - list.h;     // (no room under it: above)
        Glow(o, "choice.shadow", 600, list, kBlack, 0.8);
        pmui::Box(o, "choice.b", 601, list, kPopup, 12).Outline(2, kPopupRing);
        for (size_t n = 0; n < c.options.size(); ++n) {
            const Rect r{list.x + 6, list.y + 6 + 40.0 * static_cast<double>(n), list.w - 12, 40};
            const std::string k = "choice." + std::to_string(n);
            const double h = Ease(k, pmui::Over(o, r));
            const bool current = static_cast<int>(n) == c.current;
            pmui::Box(o, k + ".b", 602, r, A(kWhite, 0.08 * h), 8);      // (hovered: a faint lift, as the sort's list: Lvcid)
            pmui::Text(o, k + ".t", 603, c.options[n], 17, "Regular", current ? kLime : Mix(kRowLabel, kWhite, h), r.x + 12, r.Cy());
            if (pmui::Click(o, r, k)) {
                if (c.pick) c.pick(static_cast<int>(n));
                gS.choiceOpen.clear();
            }
        }
        if (pmui::PressedOutside(o, list) && pmui::PressedOutside(o, c.r)) gS.choiceOpen.clear();
        pmui::Level(kBarLevel);
    }
    if (!drawn) gS.choiceOpen.clear();
}

// --- the console --------------------------------------------------------------------------------------------------------------
// A log line in its parts, each in its colour (the concept's console): the time grey, the level (warnings yellow,
// errors red), the source tag (the host lime, plugins a cool baby blue, what you typed "[you]" in lime), the message.
struct LogLine {
    std::string text;                       // the whole line (copy)
    std::string time, level, source, message;
    Color levelColor, sourceColor, messageColor;
    double height = 0, heightFor = 0;       // its message's wrapped height, measured, at that width
};
const Color kLogTime = ux::Hex("#4E5156"), kLogInfo = ux::Hex("#6E7176"), kLogHost = ux::Hex("#8DBA1F"), kLogText = ux::Hex("#B9BCC1");
struct LogView {
    std::vector<LogLine> lines;
    size_t count = 0, clearedAt = 0;
    std::string filter;
    int warnings = 0;
} gLog;

// "[time] [level] [source] message"
void Parse(const std::string& line, std::string* level, std::string* source, std::string* message) {
    const size_t a = line.find("] ["), b = a == std::string::npos ? a : line.find("] [", a + 3);
    const size_t c = b == std::string::npos ? b : line.find("] ", b + 3);
    *level = a == std::string::npos || b == std::string::npos ? "" : line.substr(a + 3, b - a - 3);
    *source = c == std::string::npos ? "" : line.substr(b + 3, c - b - 3);
    *message = c == std::string::npos ? line : line.substr(c + 2);
}

bool CosmeticsLine(const std::string& message) {
    for (const char* prefix : {"cosmetics:", "customize:", "browser:", "models:", "packages:", "ux:"})
        if (message.rfind(prefix, 0) == 0) return true;
    return false;
}

// The log lines the filter shows (the newest 1000 kept), built again when a line is written or the filter changes.
void RefreshLog() {
    const size_t count = hostlog::LineCount();
    if (count == gLog.count && gLog.filter == gS.filter && gLog.clearedAt == gS.clearedAt) return;
    // (the wrapped heights already measured are kept, by line)
    std::unordered_map<std::string, std::pair<double, double>> heights;
    for (const LogLine& l : gLog.lines)
        if (l.height > 0) heights[l.text] = {l.height, l.heightFor};
    gLog = {};
    gLog.count = count;
    gLog.filter = gS.filter;
    gLog.clearedAt = gS.clearedAt;
    for (size_t i = std::max(gS.clearedAt, count > 1000 ? count - 1000 : 0); i < count; ++i) {
        const std::string line = hostlog::Line(i);
        if (line.empty()) continue;
        std::string level, source, message;
        Parse(line, &level, &source, &message);
        const bool warn = level == "warn" || level == "error";
        const bool mine = (source == kSelf && message.rfind("> ", 0) == 0) || (source == "host" && message.rfind("test: ", 0) == 0);
        if (warn) ++gLog.warnings;
        const std::string& f = gS.filter;
        const bool shown = f == "everything" || (f == "host" && source == "host") || (f == "plugins" && source != "host" && !source.empty()) ||
                           (f == "warnings" && warn) || (f == "cosmetics" && source == "host" && CosmeticsLine(message)) || (f == "mine" && mine);
        if (!shown) continue;
        LogLine l;
        l.text = line;
        // "[06:17:13.330]": a time only when it looks like one (a line in another form is shown whole)
        const size_t close = line.find("] ");
        l.time = !line.empty() && line[0] == '[' && close != std::string::npos && close <= 16 && !source.empty() ? line.substr(0, close + 1) : "";
        l.level = level.empty() ? "" : "[" + level + "]";
        const bool typed = source == kSelf && message.rfind("> ", 0) == 0;
        l.source = source.empty() ? "" : typed ? "[you]" : "[" + source + "]";
        l.message = l.time.empty() ? line : message;
        l.levelColor = level == "warn" ? kWarn : level == "error" ? kErr : kLogInfo;
        l.sourceColor = typed ? kLime : source == "host" ? kLogHost : kBlue;
        l.messageColor = level == "error" ? Mix(kErr, kLogText, 0.4) : kLogText;
        if (const auto it = heights.find(line); it != heights.end()) std::tie(l.height, l.heightFor) = it->second;
        gLog.lines.push_back(std::move(l));
    }
}

void RunCommand(Obj field) {
    const std::string command = field ? w::ReadText(field) : "";
    if (Bare(command).empty()) return;
    hostlog::Write("info", kSelf, "> " + command);
    testchannel::Enqueue(command);
    if (field) w::SetText(field, "");
    gS.stick = true;
}

void ConsoleOverlay(Layer& wl, const Rect& area, double t) {
    Layer& o = pmui::Group(wl, "console", 500, area, true, t);
    // (its foot's corners as round as the window's edge picture's (24 came out squarer, poking past it: Lvcid))
    pmui::BoxCorners(o, "bg", 0, FootRect(area), kOverlay, 0, 0, kFootRound, kFootRound);
    RefreshLog();
    // what to show
    struct Filter {
        const char *filter, *label;
    };
    static const Filter kFilters[] = {{"everything", "everything"}, {"host", "host"},           {"plugins", "plugins"},
                                      {"warnings", "warnings"},     {"cosmetics", "cosmetics"}, {"mine", "my commands"}};
    double x = 22;
    for (const Filter& f : kFilters) {
        const std::string filter = f.filter, key = "chip." + filter;
        const bool on = gS.filter == filter;
        const std::string count = filter == "warnings" && gLog.warnings > 0 ? std::to_string(gLog.warnings) : "";
        const double tw = Measured(o, key, f.label, 16), cw = count.empty() ? 0 : Measured(o, key + ".n", count, 15) + 6;
        const Rect r{x, 22, tw + cw + 32, 40};
        const double h = Ease(key, pmui::Over(o, r));
        if (on) Glow(o, key + ".glow", 1, r, kLime, 0.3);
        pmui::Box(o, key + ".b", 2, r, on ? kLime : kField, 11).Outline(2, on ? kBlack : Mix(kFieldEdge, kAbEdge, h));
        pmui::Text(o, key, 3, f.label, 16, "Regular", on ? kBlack : Mix(kSub, kWhite, h), r.x + 16, r.Cy());
        if (!count.empty()) pmui::Text(o, key + ".n", 3, count, 15, "Bold", on ? kBlack : kWarn, r.x + 16 + tw + 6, r.Cy());
        if (pmui::Click(o, r, key)) {
            gS.filter = filter;
            gS.stick = true;
        }
        x += r.w + 8;
    }
    double rx = area.w - 22;
    const double clw = PillWidth(o, "clear", "clear", 17, nullptr);
    if (Pill(o, "clear", 2, {rx - clw, 22, clw, 40}, "clear", 17, nullptr, PillKind::Ghost, false, 11)) gS.clearedAt = hostlog::LineCount();
    rx -= clw + 8;
    const double cpw = PillWidth(o, "copy", "copy", 17, nullptr);
    if (Pill(o, "copy", 2, {rx - cpw, 22, cpw, 40}, "copy", 17, nullptr, PillKind::Ghost, false, 11)) {
        std::string all;
        for (const LogLine& l : gLog.lines) all += l.text + "\r\n";
        CopyText(all);
        hostlog::Info("manager: copied " + std::to_string(gLog.lines.size()) + " log lines");
    }
    rx -= cpw + 8;
    if (RoundButton(o, "logfolder", 2, {rx - 40, 22, 40, 40}, "pm-folder.png", 18)) OpenDir(hostlog::DataDir());
    // the log: follows new lines unless scrolled up
    const Rect box{22, 80, area.w - 44, area.h - 80 - 22 - 58 - 14};
    pmui::Box(o, "log.b", 1, box, kConsole, 16).Outline(2, kSideEdge);
    const Rect view{box.x + 2, box.y + 2, box.w - 4, box.h - 4};
    Layer& lg = pmui::Group(o, "log", 2, view, true);
    // Columns: the time, the level, the source (its tag shortened to fit), then the message in all the room left,
    // wrapped onto more lines where it needs them (each line as tall as its message, once laid out).
    constexpr double kLineH = 26.5, kWrapLineH = 20.5, kTopPad = 14;
    constexpr double kTimeX = 20, kLevelX = kTimeX + 122, kSourceX = kLevelX + 64, kMessageX = kSourceX + 196;
    const double messageW = std::max(200.0, view.w - kMessageX - 30);
    auto heightOf = [&](LogLine& l) {
        if (l.height <= 0 || std::fabs(l.heightFor - messageW) > 0.5) {
            const double lines = std::max(1.0, std::ceil(pmui::Width(l.message, 15, "Semilight") / messageW));
            return lines <= 1 ? kLineH : kLineH + (lines - 1) * kWrapLineH;
        }
        return std::max(kLineH, l.height + 6);
    };
    double content = 2 * kTopPad;
    for (LogLine& l : gLog.lines) content += heightOf(l);
    Scroll& s = gS.scrolls["log"];
    if (gS.stick) s.target = std::max(0.0, content - view.h);
    const double before = s.target;
    const double scroll = Scrolled("log", lg, {0, 0, view.w, view.h}, content);
    if (s.target < before - 0.5) gS.stick = false;
    else if (s.target >= s.max - 1) gS.stick = true;
    double y = kTopPad - scroll;
    int slot = 0;
    for (size_t i = 0; i < gLog.lines.size(); ++i) {
        LogLine& l = gLog.lines[i];
        const double lh = heightOf(l);
        if (y + lh < -20 || y > view.h + 20) {
            y += lh;
            continue;
        }
        const std::string key = "l" + std::to_string(slot++);
        const double cy = y + kLineH / 2;
        if (!l.time.empty()) {
            pmui::Text(lg, key + ".t", 1, l.time, 15, "Semilight", kLogTime, kTimeX, cy);
            pmui::Text(lg, key + ".v", 1, l.level, 15, "Semilight", l.levelColor, kLevelX, cy);
            const double sw = pmui::Width(l.source, 15, "Semilight");
            pmui::Text(lg, key + ".s", 1, pmui::Fit(l.source, sw, kMessageX - kSourceX - 12), 15, "Semilight", l.sourceColor, kSourceX, cy);
        }
        const double mx = l.time.empty() ? kTimeX : kMessageX;
        const double mh = pmui::Para(lg, key + ".m", 1, l.message, 15, "Semilight", l.messageColor, {mx, y + 3, view.w - mx - 30, lh});
        if (mh > 0 && pmui::Find(lg, key + ".m")->measured == l.message + "|" + std::to_string(static_cast<int>(view.w - mx - 30))) {
            l.height = mh;
            l.heightFor = messageW;
        }
        y += lh;
    }
    ScrollBar(lg, "log", 20, {view.w - 14, 10, 5, view.h - 20}, view.h);
    // the prompt
    const Rect pr{22, area.h - 22 - 58, area.w - 44, 58};
    Obj field = pmui::Field(o, "prompt.field", 4, {pr.x + 52, pr.y + 10, pr.w - 52 - 110, 38},
                            "find PlayerController, props <Class>, functions <Class>, ...", 17, kWhite);
    if (gS.focusPrompt && field) {
        eng::Call(field, "SetKeyboardFocus");
        gS.focusPrompt = false;
    }
    const bool focused = Focused(field);
    const double f = Ease("prompt", focused);
    pmui::Box(o, "prompt.b", 2, pr, kConsole, 16).Outline(2, Mix(kAbEdge, kLime, f));
    pmui::Pic(o, "prompt.i", 3, {pr.x + 16, pr.Cy() - 12, 24, 24}, "pm-term.png", kLime);
    const bool run = Pill(o, "run", 3, {pr.R() - 12 - 80, pr.Cy() - 21, 80, 42}, "run", 18, nullptr, PillKind::Lime, false, 11);
    if (run || (focused && input::Pressed(VK_RETURN))) RunCommand(field);
}

// --- popups ---------------------------------------------------------------------------------------------------------------------
void ClosePopup() {
    gS.popup.clear();
    gPopupShown.clear();
    gAnim["popup"].value = 0;
}

// "Restart the game to finish" (Lvcid): once a session, as soon as the plugin manager's update is installed (or at the
// next open); "got it", Esc or a click outside dismiss it; the banner stays as the reminder.
void RestartPopup(Layer& wl, double W, double H) {
    pmui::Level(kPopupLevel);
    const double t = PopupIn();
    pmui::PreferFocus("ok");
    const Rect b{std::floor(W / 2 - 300), std::floor(H / 2 - 120), 600, 240};
    {
        ux::RawColors raw;                      // (a dark veil in every theme)
        pmui::Box(wl, "pop.dim", 900, {0, 0, W, H}, A(kBlack, kFocusDim * t), 24);
    }
    Glow(wl, "pop.shadow", 901, b, kBlack, 0.8 * t);
    Layer& pop = pmui::Group(wl, "pop", 902, b, false, t);
    pmui::Box(pop, "ring", 0, {-2, -2, b.w + 4, b.h + 4}, kPopupRing, 24);
    pmui::Box(pop, "bg", 1, {0, 0, b.w, b.h}, kPopup, 22).Outline(2, kBlack);
    pmui::Box(pop, "ico", 2, {28, 26, 44, 44}, kBlack, 22);
    pmui::Pic(pop, "ico.i", 3, {39, 37, 22, 22}, "pm-refresh.png", kLime);
    pmui::Text(pop, "title", 2, "restart the game to finish", 28, "Regular", kWhite, 88, 48);
    pmui::Para(pop, "text", 2,
               "The plugin manager is updated. Restart the game for it to take effect. Until then everything works as before, "
               "except getting or updating what needs the new version.",
               16.5, "Semilight", kPopupText, {28, 92, b.w - 56, 80});
    const double gw = PillWidth(pop, "ok", "got it", 18, nullptr, 26);
    if (Pill(pop, "ok", 3, {b.w - 28 - gw, b.h - 24 - 48, gw, 48}, "got it", 18, nullptr, PillKind::Lime, false, 13) ||
        pmui::PressedOutside(pop, {0, 0, b.w, b.h}))
        ClosePopup();
}

void Popup(Layer& wl, double W, double H) {
    // (Frame draws popups only once the welcome is over)
    if (gS.popup.empty() && !gRestartNoticed && Host().stage == HostStage::Installed) {
        gRestartNoticed = true;
        gS.popup = "restart.notice";
    }
    if (gS.popup.empty()) return;
    if (gS.popup == "restart.notice") {
        RestartPopup(wl, W, H);
        return;
    }
    if (gS.popup == "reset.appearance") {
        ResetPopup(wl, W, H);
        return;
    }
    const Plug* p = FindPlug(gS.popupId);
    if (!p) {
        ClosePopup();
        return;
    }
    pmui::PreferFocus(gS.popup == "remove" ? "no" : "yes");     // (a removal starts on cancel)
    pmui::Level(kPopupLevel);
    const double t = PopupIn();
    const bool remove = gS.popup == "remove";
    std::vector<const Plug*> deps;
    for (const std::string& n : p->needs) {
        const Plug* d = nullptr;
        for (const Plug& q : gInstalled)
            if (q.id == n) d = &q;
        if (!d)
            for (const Plug& q : gBrowse)
                if (q.id == n) d = &q;
        if (d) deps.push_back(d);
    }
    const std::string text = remove ? "Its files are deleted from the plugins folder. You can get it again from browse any time."
                                    : "It needs these to work. They come with it:";
    double ph = 52;
    if (wl.groups.count("pop"))
        if (const pmui::Entry* e = pmui::Find(*wl.groups["pop"], "text"); e && e->height > 0) ph = e->height;
    const double listH = remove ? 0 : static_cast<double>(deps.size()) * 56;
    const double bodyH = 26 + 44 + 14 + ph + 16 + listH + (remove ? 36 : 0) + 18 + 48 + 24;
    const Rect b{std::floor(W / 2 - 310), std::floor(H / 2 - bodyH / 2), 620, bodyH};
    {
        ux::RawColors raw;                      // (a dark veil in every theme)
        pmui::Box(wl, "pop.dim", 900, {0, 0, W, H}, A(kBlack, kFocusDim * t), 24);
    }
    Glow(wl, "pop.shadow", 901, b, kBlack, 0.8 * t);
    Layer& pop = pmui::Group(wl, "pop", 902, b, false, t);
    pmui::Box(pop, "ring", 0, {-2, -2, b.w + 4, b.h + 4}, kPopupRing, 24);
    pmui::Box(pop, "bg", 1, {0, 0, b.w, b.h}, kPopup, 22).Outline(2, kBlack);
    Tile(pop, "tile", 2, {28, 26, 44, 44}, p->icon, 11, 2, 0, false);
    pmui::Text(pop, "title", 2, (remove ? "remove " : "get ") + p->name + (remove ? "?" : ""), 28, "Regular", kWhite, 88, 48);
    pmui::Para(pop, "text", 2, text, 17, "Semilight", kPopupText, {28, 84, b.w - 56, 60});
    double y = 84 + ph + 16;
    if (!remove) {
        for (size_t i = 0; i < deps.size(); ++i) {
            const std::string key = "dep" + std::to_string(i);
            const Rect r{28, y, b.w - 56, 48};
            pmui::Box(pop, key + ".b", 2, r, kChip, 12);
            Tile(pop, key + ".tile", 3, {r.x + 8, r.y + 8, 32, 32}, deps[i]->icon, 8, 2, 0, false);
            pmui::Text(pop, key + ".t", 3, deps[i]->name, 17, "Regular", kText, r.x + 52, r.Cy());
            const std::string chip = deps[i]->installed ? "installed" : "comes with it";
            const double cw = Measured(pop, key + ".chip.t", chip, 13) + 18 + (deps[i]->installed ? 11 : 0);
            Chip(pop, key + ".chip", 3, r.R() - 12 - cw, r.Cy(), chip, deps[i]->installed ? ChipKind::Run : ChipKind::Update, 0);
            y += 56;
        }
    } else {
        const Rect box{28, y + 6, 22, 22};
        const Rect hit{24, y, 300, 34};
        const double h = Ease("pop.forget", pmui::Over(pop, hit));
        pmui::Box(pop, "forget.b", 2, box, gS.forget ? kLime : kPopup, 6).Outline(2, gS.forget ? kBlack : Mix(ux::Hex("#3A3C42"), kTabHover, h));
        if (gS.forget) pmui::Pic(pop, "forget.i", 3, box.Grow(-3), "check.png", kBlack);
        pmui::Text(pop, "forget.t", 2, "also forget its settings", 15, "Semilight", Mix(kPopupText, kWhite, h), 62, box.Cy());
        if (pmui::Click(pop, hit, "pop.forget")) gS.forget = !gS.forget;
        y += 36;
    }
    y += 18;
    const size_t toGet = 1 + static_cast<size_t>(std::count_if(deps.begin(), deps.end(), [](const Plug* d) { return !d->installed; }));
    const std::string yes = remove ? "remove" : toGet == 1 ? "get" : "get all " + std::to_string(toGet);
    const char* yesIcon = remove ? nullptr : "pm-down.png";
    const double yw = PillWidth(pop, "yes", yes, 18, yesIcon, 24);
    const double nw = PillWidth(pop, "no", "cancel", 18, nullptr, 24);
    const bool confirm = Pill(pop, "yes", 3, {b.w - 28 - yw, y, yw, 48}, yes, 18, yesIcon, PillKind::Lime, false, 13);
    const bool cancel = Pill(pop, "no", 3, {b.w - 28 - yw - 12 - nw, y, nw, 48}, "cancel", 18, nullptr, PillKind::Ghost, false, 13);
    if (confirm) {
        if (remove) {
            hostlog::Info("manager: remove " + p->id + (gS.forget ? " (and forget its settings)" : ""));
            registry::Remove(p->id);
            // (forgotten only if the removal was taken: a refused one (still installing) left the wish queued, and a
            // later removal of it by any way forgot its settings unasked)
            if (gS.forget && registry::Pending(p->id) == "removing") gForgetAfterRemove.push_back(p->id);
            if (gS.expanded == p->id) gS.expanded.clear();
        } else {
            hostlog::Info("manager: get " + p->id + " with what it needs");
            registry::Install(p->id);
        }
        gModelAt = -100;
        ClosePopup();
    } else if (cancel || pmui::PressedOutside(pop, {0, 0, b.w, b.h})) {
        ClosePopup();
    }
}

#include "manager_logo.inc"

// --- back and forward (Lvcid) ----------------------------------------------------------------------------------------------
// The mouse's side buttons (4: back, 5: forward) step through the places visited, as a browser's do; nothing is drawn for
// it. A place: the page and its tab, settings (and its section) or the console.
// Taken as it changes, however it changed; a new place after going back drops the forward ones. This session only.
struct Place {
    std::string page, tab, overlay, section;
    bool operator==(const Place&) const = default;
};
std::vector<Place> gHistory;
size_t gHere = 0;

Place PlaceNow() { return {gS.page, gS.tab, gS.overlay, gS.overlay == "settings" ? gS.section : ""}; }     // (the tab: both pages')

void TrackPlace() {
    const Place now = PlaceNow();
    if (!gHistory.empty() && gHistory[gHere] == now) return;
    if (!gHistory.empty()) gHistory.resize(gHere + 1);
    gHistory.push_back(now);
    if (gHistory.size() > 100) gHistory.erase(gHistory.begin());
    gHere = gHistory.size() - 1;
}

bool gResync = false;                       // a back / forward step was taken: what it reached is kept as that step
void GoTo(const Place& p) {
    gResync = true;
    if (p.page != gS.page) SetPage(p.page);
    if (!p.tab.empty() && p.tab != gS.tab) {
        gSlideFrom = gSlideNow;             // (as a click on the tab: the lime box slides)
        gS.swapFrom = gS.tab;
        gS.swapAt = pmui::Now();
        gS.tab = p.tab;
        gS.expanded.clear();
    }
    if (p.overlay != gS.overlay || (p.overlay == "settings" && p.section != gS.section)) OpenOverlay(p.overlay, p.section);
}

// The first open since an update (or DEBUG's pretend one) starts afresh, as a new session does (Lvcid): plugins /
// installed, nothing open, no history.
void FreshStart() {
    gS = State{};
    if (gInstalledField) w::SetText(gInstalledField, "");
    gHistory.clear();
    gHere = 0;
    gSlideFrom = gSlideNow = {};
}

// --- keys and pads (Lvcid, 2026-10-11) --------------------------------------------------------------------------------
// Keyboard and mouse: Esc back / close, the mouse's back and forward buttons step through where you've been (never
// closing), Space / Enter select, arrows and WASD move, Tab / Shift+Tab move in order. A pad: the D-pad or stick moves,
// A selects, B back / close, Start closes the window, Y the page's search, X turns the focused plugin on or off, LT / RT
// settings / plugins / cosmetics, LB / RB the page's two tabs (installed / browse, owned / explore), View settings.

bool gPadMode = false;                      // the last thing used was a pad (the legend shows its buttons), else keys / mouse

void TrackInputMode() {
    const int k = input::AnyPressed();
    if (k >= input::kPadA) gPadMode = true;
    else if (k > 0 || pmui::MouseMoved()) gPadMode = false;
}

// What Esc / B does now: closes the window (true) or steps back (false).
bool BackCloses() {
    if (!gS.popup.empty() || !gS.choiceOpen.empty() || gS.sortOpen || gTypedLastFrame) return false;
    if (gS.page == "cosmetics" && gS.overlay.empty() && !gCosRowsState.expanded.empty())
        return false;
    return gS.overlay.empty();
}

void NavKeys() {
    using namespace input;
    auto held = [](std::initializer_list<int> keys) {
        for (int k : keys)
            if (Down(k)) return true;
        return false;
    };
    int dx = 0, dy = 0;
    if (held({VK_UP, 'W', kPadUp, kPadStickUp})) dy = -1;
    else if (held({VK_DOWN, 'S', kPadDown, kPadStickDown})) dy = 1;
    else if (held({VK_LEFT, 'A', kPadLeft, kPadStickLeft})) dx = -1;
    else if (held({VK_RIGHT, 'D', kPadRight, kPadStickRight})) dx = 1;
    static int heldDir = 0;
    static double nextAt = 0;
    const int dir = dy * 2 + dx;
    const double now = pmui::Now();
    gNavHeld = dir != 0;
    // (a slider with the focus: left / right move it, a step at a time (kept; X held: a test, Slider), not the focus)
    auto go = [&] {
        if (dx != 0 && dy == 0 && !gSliderFocused.empty() && gSliderFocused == pmui::Focus() && pmui::Navigating()) gSliderStep = dx;
        else pmui::Nav(dx, dy);
    };
    if (dir == 0) {
        heldDir = 0;
    } else if (dir != heldDir) {
        heldDir = dir;
        nextAt = now + 0.38;                // (held: it repeats after a moment, then quickly)
        go();
    } else if (now >= nextAt) {
        nextAt = now + 0.075;
        go();
    }
    if (Pressed(VK_TAB)) pmui::NavOrder(Down(VK_SHIFT) ? -1 : 1);
    if (Pressed(kPadA) || Pressed(VK_RETURN) || Pressed(VK_SPACE)) {
        if (pmui::Navigating()) pmui::Press(pmui::Focus());
        else pmui::Nav(0, 0);               // (the first press only shows where the focus is)
    }
    if (!gS.popup.empty()) return;          // (a popup: only moving and selecting)
    // X: the focused plugin on or off (its row, or anything on it: "row.i.<id>", "i.<id>.<part>"; cosmetics' "ci.")
    if (Pressed(kPadX)) {
        std::string f = pmui::Focus();
        if (f.rfind("row.", 0) == 0) f = f.substr(4);
        const size_t a = f.find('.');
        if (!pmui::Navigating()) pmui::Nav(0, 0);
        else if (a != std::string::npos && (f.compare(0, a, "i") == 0 || f.compare(0, a, "ci") == 0))
            pmui::Press(f.substr(0, f.find('.', a + 1)) + ".tog");
    }
    // Y: the page's search
    if (Pressed(kPadY) && gS.overlay.empty()) {
        if (Obj field = gInstalledField) {
            eng::Call(field, "SetKeyboardFocus");
            pmui::NavOff();
        }
    }
    // LT / RT: settings, plugins, cosmetics (in that order, round)
    if (const int trig = (Pressed(kPadRT) ? 1 : 0) - (Pressed(kPadLT) ? 1 : 0); trig != 0) {
        std::vector<std::string> places = {"settings", "plugins"};
        if (packages::KitOn()) places.push_back("cosmetics");
        const std::string here = gS.overlay == "settings" ? std::string("settings") : gS.page;
        const auto at = std::find(places.begin(), places.end(), here);
        const int i = at == places.end() ? 1 : static_cast<int>(at - places.begin()), n = static_cast<int>(places.size());
        const std::string& to = places[static_cast<size_t>(((i + trig) % n + n) % n)];
        if (to == "settings") OpenOverlay("settings");
        else SetPage(to);
    }
    // LB / RB: the page's two tabs (as a click on the strip: the lime box glides over)
    if (const int bump = (Pressed(kPadRB) ? 1 : 0) - (Pressed(kPadLB) ? 1 : 0); bump != 0 && gS.overlay.empty()) {
        const std::string to = bump > 0 ? "browse" : "installed";
        if (gS.tab != to) {
            gSlideFrom = gSlideNow;
            gS.swapFrom = gS.tab;
            gS.swapAt = pmui::Now();
            gS.tab = to;
            gS.expanded.clear();
        }
    }
    if (Pressed(kPadView)) OpenOverlay(gS.overlay == "settings" ? "" : "settings");
}

// The legend (Lvcid): the basics for the input used last, just off the window's bottom right, on a small dark pill.
// A pad: (A) select (B) back / close; keys and mouse: [Esc] back / close. One word for B / Esc: what it will do.
void Legend(Layer& root, const Rect& win, const Rect& dim, double alpha) {
    if (alpha < 0.01) return;
    struct Item {
        std::string key, label;
        bool round;
    };
    const std::string back = BackCloses() ? "close" : "back";
    std::vector<Item> items;
    if (gPadMode) items = {{"A", "select", true}, {"B", back, true}};
    else items = {{"Esc", back, false}};
    const Color ink = A(kTabHover, alpha), letter = A(kWhite, alpha), chip = A(kChip, alpha), ring = A(ux::Hex("#3A3C42"), alpha);
    constexpr double kLegGap = 8, kLegBetween = 18, kLegPad = 14, kLegH = 34;
    double total = 0;
    std::vector<double> widths;
    for (size_t i = 0; i < items.size(); ++i) {
        const double kw = items[i].round ? 24 : Measured(root, "leg.k" + std::to_string(i), items[i].key, 12) + 16;
        const double lw = Measured(root, "leg.l" + std::to_string(i), items[i].label, 15);
        widths.push_back(kw);
        total += kw + kLegGap + lw + (i + 1 < items.size() ? kLegBetween : 0);
    }
    // off the window, under its right edge; no room down there (a tall window): inside its corner, left of the grip
    const double room = dim.B() - win.B();
    const bool under = room >= kLegH + 4;
    // (under it: close by its bottom right, a small gap below the window and a little in from its right (Lvcid: it felt
    // far off in the middle of the room down there))
    const double right = under ? win.R() - 10 : win.R() - 30, cy = under ? win.B() + std::clamp(room - kLegH - 2, 2.0, 10.0) + kLegH / 2 : win.B() - 10 - kLegH / 2;
    const Rect pill{std::floor(right - total - 2 * kLegPad), std::floor(cy - kLegH / 2), std::floor(total + 2 * kLegPad), kLegH};
    pmui::Box(root, "leg.bg", 3000, pill, A(kWindow, 0.82 * alpha), kLegH / 2).Outline(1, A(ux::Hex("#2A2C31"), alpha));
    double x = pill.x + kLegPad;
    for (size_t i = 0; i < items.size(); ++i) {
        const std::string k = "leg." + std::to_string(i);
        const Rect kr{x, std::floor(cy - 12), widths[i], 24};
        pmui::Box(root, k + ".b", 3001, kr, chip, items[i].round ? 12 : 7).Outline(2, ring);
        pmui::Text(root, "leg.k" + std::to_string(i), 3002, items[i].key, 12, "Bold", letter, kr.Cx(), cy, 0.5);
        x = kr.R() + kLegGap;
        x += pmui::Text(root, "leg.l" + std::to_string(i), 3002, items[i].label, 15, "Bold", ink, x, cy) + kLegBetween;
    }
}

// --- keys -----------------------------------------------------------------------------------------------------------------------
// Esc goes back one step: a popup, an open dropdown, a text box, an open card, an overlay, then the window itself. The
// welcome's intro takes no keys (it plays through).
void Keys() {
    TrackInputMode();
    if (gWelcome == Welcome::Intro) return;     // (the intro plays through: it's short; Lvcid)
    // Back (Esc, or a pad's B): one step each press, the topmost thing first; the window itself only when nothing else
    // is open (Lvcid: a back never closes more than one level). Each step is logged.
    if (input::Pressed(VK_ESCAPE) || input::Pressed(input::kPadB)) {
        std::string step;
        if (!gS.popup.empty()) ClosePopup(), step = "the popup";
        else if (!gS.choiceOpen.empty()) gS.choiceOpen.clear(), step = "the list";
        else if (gS.sortOpen) gS.sortOpen = false, step = "the sort list";
        else if (gTypedLastFrame) Unfocus(), step = "the text box (its text kept)";      // typing: out of the box, not the window
        else if (gS.page == "cosmetics" && gS.overlay.empty() && !gCosRowsState.expanded.empty()) gCosRowsState.expanded.clear(), step = "the open card";
        else if (!gS.overlay.empty() && gS.overlay == "settings" && gS.backFor == gS.section) {
            const std::string overlay = gS.backOverlay, section = gS.backSection;      // (a jump in: back where it came from)
            OpenOverlay(overlay, overlay == "settings" ? section : "");
            step = "back to " + (overlay.empty() ? std::string("the page") : overlay);
        } else if (!gS.overlay.empty()) {
            step = gS.overlay;
            OpenOverlay("");                // settings or the console: back to the page it was on (Lvcid)
        } else {
            step = "the window";
            Close();                        // the whole window (Lvcid): it opens again just as it was
        }
        hostlog::Info("manager: back: closed " + step);
        return;
    }
    // Start: out, all the way, from any layer (its state kept)
    if (input::Pressed(input::kPadMenu)) {
        hostlog::Info("manager: closed by Start");
        Close();
        return;
    }
    // back / forward (the mouse's side buttons); not while a popup asks something
    if (gResync && !gHistory.empty()) gHistory[gHere] = PlaceNow();     // (where the step really landed)
    gResync = false;
    if (NewLook()) TrackPlace();             // (Classic: the window is only settings, nothing to step through)
    if (const bool back = input::Pressed(VK_XBUTTON1), fwd = input::Pressed(VK_XBUTTON2); (back || fwd) && gS.popup.empty()) {
        if (back ? gHere > 0 : gHere + 1 < gHistory.size()) {
            gS.choiceOpen.clear();
            gS.sortOpen = false;
            gHere = back ? gHere - 1 : gHere + 1;
            const Place& p = gHistory[gHere];
            GoTo(p);
            hostlog::Info(std::string("manager: ") + (back ? "back" : "forward") + " to " + p.page +
                          (p.tab.empty() ? "" : " / " + p.tab) + (p.overlay.empty() ? "" : ", " + p.overlay) +
                          (p.section.empty() ? "" : " / " + p.section));
        }
        return;
    }
    if (!gTypedLastFrame && gS.choiceOpen != "theme") NavKeys();
}

// Closing lets go of input a moment later (only the click blocker stays up meanwhile), so the click or key that closed
// it doesn't land in the game too.
constexpr double kReleaseDelay = 0.12, kDimOut = 0.5, kDimIn = 0.22, kWindowIn = 0.12;
constexpr double kWindowOut = 0.12;         // closed: the window fades out (as it was, taking nothing), the dim with it
double gReleaseAt = -1, gOpenedAt = -100;
Rect gLastDim;                              // where the dim was, to ease it out after closing

// The dim, `since` seconds after closing (Lvcid): the game brightens back smoothly, starting gently (the window is
// dark; a level full of white pieces shouldn't come back as a flash), quick enough not to feel slow.
double DimNow() { return gPrefs.dimOn ? gPrefs.dim : 0.0; }       // (its toggle off: no dim)
double ClosingDim(double since) { return DimNow() * (1 - Smooth(std::clamp(since / kDimOut, 0.0, 1.0))); }

// Input back to the game (a moment after closing); the dim may still be easing out, drawn but taking no clicks.
bool gReleased = true;
void Release() {
    game::SetModalWidget(nullptr);
    game::RequestCursor(kCursorOwner, false);
    for (ux::Node& b : gBlocker) b.Show(false);
    gReleased = true;
}
void Hide() {
    if (!gReleased) Release();
    if (gScreen.Ready()) gScreen.Show(false);
    gReleaseAt = -1;
}

// Resizing: dragging an edge or a corner resizes the window about its middle (it stays centred, both sides move).
struct Resize {
    std::string edge;                       // the handle held, or ""
    double startW = 0, startH = 0, startX = 0, startY = 0;
} gResize;

}  // namespace

// The footer's plugins button opens on the page it was left on the last time it was opened from there (plugins at
// first); the Customize page's globe opens cosmetics without changing that (Lvcid). This session only.
// (plugins run after the window each frame: the Esc or click that closed it would reach them too, the same frame)
long long gFrameNo = 0, gClosedFrameNo = -1;
std::string gFooterPage = "plugins";
bool gFromFooter = false;

// A run being raced, not paused and not finished: the window doesn't open over it, and closes if one starts under it (the
// timer runs on and the ball can't be driven meanwhile: AnythingGoes' review).
bool LiveRun() { return race::Active() && !race::Complete() && !race::Paused(); }

void Open(const std::string& page_) {
    if (!gS.open && LiveRun()) {
        hostlog::Info("manager: not opened: a run is being raced (pause it first)");
        return;
    }
    // (the Classic theme: this window is only its settings, opened by the old menu's cog)
    const std::string page = page_.empty() && !NewLook() ? "settings" : page_;
    const bool first = !gS.open && FirstOpenNow();
    if (first) {
        FreshStart();                       // (a new version: nothing kept from before; Lvcid)
        gFooterPage = "plugins";
    }
    if (!gS.open) {
        gFromFooter = page.empty();
        HoverReset("top.");
    }
    if (page.empty() && !gS.open && gS.page != gFooterPage) SetPage(gFooterPage);
    if (page == "settings" || page == "console") OpenOverlay(page);
    else if (page == "plugins" || page == "cosmetics") SetPage(page);
    if (!gS.open) hostlog::Info("manager: opened" + (page.empty() ? std::string() : " on " + page));
    if (!gS.open) {
        gFirstOpen = first;
        if (gFirstOpen) {
            hostlog::Info(std::string("manager: first open since ") + plugins::kHostVersion + " was installed" +
                          (gTestFirstOpen ? " (pretend, from DEBUG)" : "") + ": the welcome plays");
            StartWelcome();
        }
    }
    if (!gS.open) gHole.until = game::Seconds() + 0.5;     // (the footer's button measured again as it opens)
    if (!gS.open && gReleaseAt < 0) gOpenedAt = game::Seconds();     // (reopened while its dim goes: it comes back from there)
    gS.open = true;
    gClosedByX = false;
    gReleaseAt = -1;
    gReleased = false;
    for (ux::Node& b : gBlocker) b.Show(true);
    gModelAt = -100;
    game::RequestCursor(kCursorOwner, true);
}

void Close() {
    if (!gS.open) return;
    if (gWelcome == Welcome::Intro) {
        hostlog::Info("manager: not closed: the intro is playing");
        return;
    }
    if (gFromFooter) gFooterPage = gS.page;
    gS.open = false;
    gClosedFrameNo = gFrameNo;
    // the corner logo's replay is cut off where it is. (The intro can't be closed: Close refuses it above.)
    gWelcome = Welcome::None;
    gWelcomeAt = -100;
    gReplayAt = -100;
    gReplayNext = -1;
    ClosePopup();
    gS.choiceOpen.clear();
    gS.sortOpen = false;
    gResize.edge.clear();
    Unfocus();
    if (gPrefsDirtyAt >= 0 && gSliderTest.key.empty()) SavePrefs();
    gReleaseAt = game::Seconds() + kReleaseDelay;
    hostlog::Info("manager: closed");
}

bool IsOpen() { return gS.open; }

double HoverSoundLevel() {
    if (!gPrefsRead) ReadPrefs();
    return gPrefs.hoverSound ? gPrefs.hoverVol / kDefaultHoverVol : 0.0;
}

bool TookInputThisFrame() { return gS.open || gClosedFrameNo == gFrameNo; }

bool NewLook() {
    const int nl = NewLookIndex();
    return nl >= 0 && settings::Get(static_cast<size_t>(nl)) != "Classic";
}

// The logo's click (Lvcid): everything checked again: the registry (what's out, every update, the plugin manager's own,
// icons), the installed and browse lists, the cosmetics, and where the footer's plugins button is. (Plugins put in the
// folder by hand still load at the next start: the host reads the folder once.)
double gRefreshAt = -100;
constexpr double kRefreshSpin = 1.1;       // the refresh symbol's two turns, in seconds
void RefreshAll() {
    hostlog::Info("manager: refresh (the logo)");
    registry::Refresh();
    gModelAt = gCosAt = -100;
    gHole.until = game::Seconds() + 0.5;
    gRefreshAt = pmui::Now();
}

void ThemeLimes() {
    const bool night = BallestNight();
    kLime = night ? kNightLime : kLucidLime;
    kNuLime = night ? kNightLime : kLucidNuLime;
    kTopLime = night ? kNightLime : kLucidTopLime;
}

void Frame() {
    ++gFrameNo;
    ux::SetThemeMap(nullptr);               // (a theme's colour map is only ever on while the window draws)
    ux::SetOutlineFade(1);
    gThemeNow = -1;                         // (the theme read again this frame: the classic menu or a setting may have changed it)
    gFx = nullptr;                          // (set again when the window is drawn: a new screen drops the old layer)
    ThemeLimes();
    // a slider's test-drag whose slider isn't drawn any more (settings closed mid-test): what it changed goes back
    if (!gSliderTest.key.empty() && game::Seconds() - gSliderTest.seen > 0.2) {
        gPrefs.dim = gSliderTest.prefs.dim;
        gPrefs.scale = gSliderTest.prefs.scale;
        gSliderTest.key.clear();
        SavePrefs();
        hostlog::Info("manager: test-drag ended (its slider went away): put back");
    }
    // the footer's plugins button: lit (a dimmer white than hovered) while the window is open (Lvcid)
    for (const auto& b : ui::footer::Buttons())
        if (plugins::IdAt(b->owner) == kSelf) b->active = gS.open;
    // Removed plugins whose settings are to be forgotten, once they are gone.
    for (auto it = gForgetAfterRemove.begin(); it != gForgetAfterRemove.end();) {
        plugins::Info info;
        if (!plugins::Find(*it, &info) && registry::Pending(*it).empty()) {
            storage::Forget(*it);
            hostlog::Info("manager: forgot the settings of " + *it);
            it = gForgetAfterRemove.erase(it);
        } else {
            ++it;
        }
    }
    // closed: for a moment the window fades out as it was (taking nothing); its dim eases out meanwhile and longer
    bool closing = false;
    if (gS.open && LiveRun() && gWelcome != Welcome::Intro) {
        hostlog::Info("manager: closed: a run started");
        Close();
    }
    if (!gS.open) {
        if (gReleaseAt < 0) return;
        if (!gReleased && game::Seconds() >= gReleaseAt) Release();
        const double since = game::Seconds() - (gReleaseAt - kReleaseDelay);
        if (since >= kDimOut || !gScreen.Ready()) {
            Hide();
            return;
        }
        if (since >= kWindowOut) {
            Layer& root = pmui::Begin(gScreen, gPrefs.scale);
            pmui::Box(root, "dim", 0, gLastDim, A(kBlack, ClosingDim(since)));
            pmui::End();
            return;
        }
        closing = true;
    }
    if (const int nl = NewLookIndex(); !closing && gWelcome == Welcome::None && nl >= 0 && settings::Get(static_cast<size_t>(nl)) == "Classic" && gS.overlay.empty()) {
        Close();                            // switched back to the classic menu
        return;
    }
    if (!gScreen.Ready()) {
        gScreen.zOrder = 500;               // as the classic menu: in front of plugin windows, behind the footer
        if (!gScreen.Make()) return;
        // It takes the keyboard and pads while open (game::SetModalWidget): focusable, so they go to it.
        if (Obj host = eng::Get(gScreen.host)) {
            eng::WriteBool(host, "bIsFocusable", true);
            if (eng::FindFunction(eng::ClassOf(host), "SetIsFocusable")) eng::Call(host, "SetIsFocusable", uint8_t{1});
        }
    }
    gScreen.Show(true);
    if (!closing) game::SetModalWidget(eng::Get(gScreen.host));
    if (!gPrefsRead) ReadPrefs();
    if (!gPrefs.scaleChosen) gPrefs.scale = ScreenDefaultScale();     // (a screen change: its own default)
    const double k = gPrefs.scale;
    gScreen.Scale(k);
    pmui::OnHoverStart(HoverSound);
    Layer& root = pmui::Begin(gScreen, k);
    // (saved on release (Lvcid): once the button is up, the pad's direction let go and no test-drag is on)
    if (gPrefsDirtyAt >= 0 && !pmui::Mouse().down && !gNavHeld && gSliderTest.key.empty()) SavePrefs();
    gTypingField = nullptr;
    if (pmui::Now() - gModelAt > 0.25) BuildModel();
    if (!closing) Keys();
    closing = closing || !gS.open;          // (closed by a key just now: it fades from this frame)
    // the welcome: its intro shows only the logo (nothing answers); once the logo has gone, the window is the pages
    if (gWelcome == Welcome::Intro && IntroTime() >= kWindowAt) EndWelcome();
    if (closing || gWelcome == Welcome::Intro) pmui::SetModal(1000);      // fading out: nothing answers
    else pmui::SetModal(!gS.popup.empty() ? kPopupLevel : (!gS.choiceOpen.empty() || gS.sortOpen) ? kDropLevel : !gS.overlay.empty() ? kBarLevel : 0);
    // Where it goes: centred between the game's top bar and its footer, drawn at the size setting about the screen's
    // middle. Its size on screen: the one dragged to (else 1184x780 at 1080), never smaller than its pages
    // fit, never bigger than the room between the bars. The dim leaves both bars as they are; clicks are taken everywhere.
    double sw = 1920, sh = 1080;
    ux::ScreenSize(&sw, &sh);
    const double footer = ui::footer::Height();
    const double top = 38, bottom = footer > 1 ? footer : 40;
    const double cx = sw / 2, cy = sh / 2;
    const Rect screen{cx - cx / k, cy - cy / k, sw / k, sh / k};
    const Rect dim{screen.x, cy + (top - cy) / k, screen.w, (sh - bottom - top) / k};
    const double roomW = sw - 24, roomH = sh - top - bottom - 12;
    const double minW = std::min(roomW, 1100 * k), minH = std::min(roomH, 620 * k);
    double ws = gPrefs.winW > 0 ? gPrefs.winW : std::min(kWinW, sw - 64);
    double hs = gPrefs.winH > 0 ? gPrefs.winH : std::min(kWinH, sh - top - bottom - 48);
    if (!gResize.edge.empty()) {
        if (pmui::Held("resize." + gResize.edge)) {
            const std::string& e = gResize.edge;
            const double sx = e.find('l') != std::string::npos ? -1 : e.find('r') != std::string::npos ? 1 : 0;
            const double sy = e.find('t') != std::string::npos ? -1 : e.find('b') != std::string::npos ? 1 : 0;
            if (sx != 0) ws = gResize.startW + sx * 2 * (pmui::Mouse().x - gResize.startX) * k;
            if (sy != 0) hs = gResize.startH + sy * 2 * (pmui::Mouse().y - gResize.startY) * k;
            gPrefs.winW = std::clamp(ws, minW, roomW);
            gPrefs.winH = std::clamp(hs, minH, roomH);
        } else {
            gResize.edge.clear();
            PrefsChanged();
        }
    }
    ws = std::clamp(ws, minW, roomW);
    hs = std::clamp(hs, minH, roomH);
    const double W = std::floor(ws / k), H = std::floor(hs / k);
    const double wcy = cy + ((top + sh - bottom) / 2 - cy) / k;
    const Rect win{std::floor(cx - W / 2), std::floor(wcy - H / 2), W, H};
    pmui::Level(0);
    gLastDim = dim;
    const double closedFor = closing ? game::Seconds() - (gReleaseAt - kReleaseDelay) : 0;
    pmui::Box(root, "dim", 0, dim,
              A(kBlack, closing ? std::min(ClosingDim(closedFor), DimNow() * EaseOut((pmui::Now() - gOpenedAt) / kDimIn))
                                : DimNow() * EaseOut((pmui::Now() - gOpenedAt) / kDimIn)));
    if (eng::Get(gBlockerCanvas) != gScreen.Canvas() || !gBlocker[0].Get()) {
        for (ux::Node& b : gBlocker) b = ux::MakeHit(gScreen.Tree(), gScreen.Canvas(), 0);
        gBlockerCanvas = eng::MakeWeak(gScreen.Canvas());
    }
    {
        // the footer's plugins button (other plugins' buttons move it), measured again only when the footer changed;
        // none during the intro
        Obj mine = nullptr;
        std::string key = std::to_string(static_cast<int>(sw)) + "x" + std::to_string(static_cast<int>(sh));
        for (const auto& b : ui::footer::Buttons()) {
            key += "|" + b->label;
            if (plugins::IdAt(b->owner) == kSelf) mine = eng::Get(b->button);
        }
        key += "|" + std::to_string(reinterpret_cast<uintptr_t>(mine));
        const double now = game::Seconds();
        if (key != gHole.key) {
            gHole.key = key;
            gHole.until = now + 0.5;
        }
        if (now < gHole.until && mine)
            if (const ux::Rect vb = ux::Bounds(mine); vb.ok) gHole.at = vb;
        Rect hole{screen.x, screen.B(), 0, 0};
        if (gWelcome != Welcome::Intro && !closing && gHole.at.ok && mine)
            hole = {cx + (gHole.at.x - cx) / k, cy + (gHole.at.y - cy) / k, gHole.at.w / k, gHole.at.h / k};
        gBlocker[0].Box(screen.x, screen.y, screen.w, std::max(0.0, hole.y - screen.y));                     // above it
        gBlocker[1].Box(screen.x, hole.B(), screen.w, std::max(0.0, screen.B() - hole.B()));                 // below it
        gBlocker[2].Box(screen.x, hole.y, std::max(0.0, hole.x - screen.x), hole.h);                         // left of it
        gBlocker[3].Box(hole.R(), hole.y, std::max(0.0, screen.R() - hole.R()), hole.h);                     // right of it
    }
    // A click outside the window (on the dim, the game under it) closes it from whatever is open (a popup, a list,
    // settings), as it is (Lvcid); not while the welcome's intro plays. Pressed and let go out there.
    bool outside = false;
    {
        static bool pressedOut = false;
        const pmui::Pointer& m = pmui::Mouse();
        const bool out = dim.Has(m.x, m.y) && !win.Grow(12).Has(m.x, m.y);        // (12: the resize edges, the logo's overhang)
        if (m.pressed) pressedOut = out && !closing && gWelcome != Welcome::Intro;
        outside = pressedOut && m.released && out;
        if (m.released) pressedOut = false;
    }
    // The resize handles (Lvcid: found exactly, at any resolution): bands on the window's edges, mostly outside it, and its
    // corners (not the top left: the logo is there, so the top and left bands start after it). The one under the pointer
    // is the only thing that answers there (pmui::Shield), so nothing under it lights up or takes the press; while one is
    // dragged nothing else answers at all. Drawn last (below).
    struct Handle {
        const char* edge;
        Rect r;
    };
    constexpr double bandOut = 10, bandIn = 5, corner = 22, logoEnd = 82;
    const Handle handles[] = {
        {"br", {win.R() - corner, win.B() - corner, corner + bandOut, corner + bandOut}},
        {"tr", {win.R() - corner, win.y - bandOut, corner + bandOut, corner + bandOut}},
        {"bl", {win.x - bandOut, win.B() - corner, corner + bandOut, corner + bandOut}},
        {"l", {win.x - bandOut, win.y + logoEnd, bandOut + bandIn, win.h - logoEnd - corner}},
        {"r", {win.R() - bandIn, win.y + corner, bandOut + bandIn, win.h - 2 * corner}},
        {"t", {win.x + logoEnd, win.y - bandOut, win.w - logoEnd - corner, bandOut + bandIn}},
        {"b", {win.x + corner, win.B() - bandIn, win.w - 2 * corner, bandOut + bandIn}}};
    std::string edgeHere;
    if (gWelcome == Welcome::None && !closing && pmui::Modal() <= kBarLevel && gResize.edge.empty())
        for (const Handle& hd : handles)
            if (hd.r.Has(pmui::Mouse().x, pmui::Mouse().y)) {
                edgeHere = hd.edge;
                pmui::Shield(root, hd.r);
                break;
            }
    if (!gResize.edge.empty()) pmui::Shield(root, screen.Grow(1e5));
    pmui::Click(root, win, "window", false);
    double shown = EaseOut((pmui::Now() - gOpenedAt) / kWindowIn);               // it fades in, quickly
    if (closing) shown = std::min(shown, 1 - EaseOut(closedFor / kWindowOut));   // and out, as quickly
    // the welcome's intro (Lvcid): no window while the logo plays in the middle (not even its edge); once the logo has
    // gone, the window comes in sharply, on plugins / installed
    if (gWelcomeAt > -50 && IntroTime() < kWindowAt + kSharpIn) shown *= EaseOut(logo::Clamp01((IntroTime() - kWindowAt) / kSharpIn), 2);
    // (the window's outlines fade with it: they don't follow the window's opacity by themselves)
    ux::SetOutlineFade(shown);
    // (the theme's colour map on from here even on the first, unshown frame: things first made unmapped kept that)
    ux::SetThemeMap(BallestLight() ? LightMap : nullptr);
    if (shown > 0.001) {
        Glow(root, "shadow", 1, win, kBlack, 0.75 * shown);
        ux::SetThemeMap(BallestLight() ? LightMap : nullptr);     // (from the window on: the dim under it as it is)
        // (Ballest Light: the fill as the edge picture's own shape (its corners came out squarer than the edge's, poking
        // out past it, light on the game: Lvcid); the box stays for its opacity, clear)
        // (every theme: Ballest Night's corners poked out past the edge too)
        ux::Node& bg = pmui::Box(root, "win", 2, win, A(kWindow, 0), 24);
        {
            const Color fill = ux::Themed(kWindow, 0);
            ux::RawColors raw;
            pmui::Sliced(root, "win.fill", 2, win, "pm-edge-fill.png", 28.0 / 96, 96, A(fill, shown));
        }
        // (Ballest Light: the game's own light panel behind everything, its halftone: MI_Halftone_White, as the
        // Customize page's background)
        if (BallestLight())
            // (its own box past the edges; drawn 8 in, so its square corners sit inside the window's curve: Lvcid)
            pmui::MaterialOf(root, "win.halftone", 2, win.Grow(-8), ux::HalftoneMaterial(1.25f), 16, shown);
        bg.Outline(2, A(kBlack, 0));      // (its faint grey edge, not black on black: win.edge, below)
        bg.Opacity(static_cast<float>(shown));
        // (its edge over the window's insides (z 4: the settings overlay's backing covered it) and under the logo; a hint
        // of metal; Lvcid)
        pmui::Sliced(root, "win.edge", 4, win, "pm-edge-ring.png", 28.0 / 96, 96, A(ux::Hex(BallestLight() ? "#2E3034" : "#2A2C31"), shown));     // (Ballest Light: dark, as the logo: Lvcid)     // (a picture: smooth corners, Lvcid)
    }
    Layer& wl = pmui::Group(root, "w", 3, win, true, shown);
    gFx = &pmui::Group(root, "fx", 5, screen, false, shown);
    gFxScreen = {0, 0, screen.w, screen.h};      // (the screen itself: a group as big as root's endless clip drew nothing)
    const bool welcome = gWelcome != Welcome::None;
    if (!welcome && CurrentTheme() == 0)
        // (Ballest Night: the dark halftone faintly behind the top bar, its big dots toward the top right, as the welcome's;
        // past the logo, in from the window's top and right (its square corners inside the curve): Lvcid)
        pmui::MaterialOf(wl, "top.halftone", 1, {96, 8, W - 96 - 8, kTopH - 10}, ux::HalftoneMaterial(1.25f, true), 0, 0.35);
    if (!welcome) TopBar(wl, W);
    // top left, over the window's corner (Lvcid's sheet): the plugin manager's logo in a round frame, 86 across. In the
    // welcome's intro it plays big in the screen's middle first, flying here as it settles; while an update banner
    // offers an update it twinkles here every 10 s (manager_logo.inc).
    {
        const Rect lr{win.x - 7, win.y - 8, 86, 86};
        pmui::Level(kBarLevel);
        const double now = pmui::Now();
        // While an update banner offers an update: its replay (glint, twinkle, sway) every 10 s (the first 2 s after it
        // shows). Otherwise its own quiet animation, the twinkle alone, every 9 s (Lvcid, 2026-10-11).
        static double twinkleAt = -100, twinkleNext = -1;
        if (gBannerLive && !welcome && !Animating() && !closing) {
            if (gReplayNext < 0) gReplayNext = now + 2;
            if (now - gReplayAt >= logo::kReplayLen && now >= gReplayNext) {
                gReplayAt = now;
                gReplayNext = now + 10;
            }
        } else if (now - gReplayAt >= logo::kReplayLen) {
            gReplayNext = -1;                   // (the banner gone: the one playing finishes, no more start)
        }
        const bool replaying = now - gReplayAt < logo::kReplayLen;
        if (!gBannerLive && !welcome && !Animating() && !closing && !replaying) {
            if (twinkleNext < 0) twinkleNext = now + 9;
            if (now >= twinkleNext && now - twinkleAt >= logo::kNudgeLen) {
                twinkleAt = now;
                twinkleNext = now + 9;
            }
        } else if (gBannerLive || welcome || Animating()) {
            twinkleNext = -1;                   // (from 9 s after it's quiet again)
        }
        logo::Look look;
        Rect at = lr;
        double opacity = shown;
        if (Animating()) {
            // big in the screen's middle; gone after its twinkle (its corner's comes in with the window)
            look = logo::Welcome(IntroTime());
            at = {std::floor(screen.Cx() - kBigLogo / 2), std::floor(screen.Cy() - kBigLogo / 2), kBigLogo, kBigLogo};
            opacity = closing ? 0.0 : IntroOpacity();
        } else if (replaying) {
            look = logo::Replay(now - gReplayAt);
        } else if (now - twinkleAt < logo::kNudgeLen) {
            look = logo::Nudge(now - twinkleAt);       // (the twinkle alone: Lvcid)
        }
        // hovered (the welcome over): the lime square dissolves into a lime refresh symbol, it is the refresh button
        // (Lvcid); clicked, the symbol rolls twice round while everything is checked again, easing in and out (Lvcid,
        // 2026-10-11: twice, eased in)
        if (!welcome && !Animating()) {
            const double spin = logo::Clamp01((now - gRefreshAt) / kRefreshSpin);
            look.refresh = Ease("logo.refresh", (pmui::Over(root, lr) && !closing) || spin < 1, 0.07);
            look.spin = 720 * (spin < 0.5 ? 4 * spin * spin * spin : 1 - std::pow(-2 * spin + 2, 3) / 2);
            // clicked: the symbol goes white and fades back to lime, on the pages' hover curve and speeds (Lvcid)
            look.flash = HoverCurve("logo.flash", now - gRefreshAt < kHoverIn, kHoverIn, kHoverOut);
        }
        // hovered (in its corner, the welcome over): it's the refresh button (below)
        const double hov = Ease("logo", pmui::Over(root, lr) && !closing && !welcome);
        if (!Animating()) {
            // (in its corner: the window's edge goes on round it, a thin ring just outside it in the edge's colour, and
            // a soft shadow under it, so the edge meets it and it sits on the window: Lvcid)
            ux::RawColors raw;
            Glow(root, "logo.shadow", 5, at.Moved(0, 3).Grow(-2), kBlack, 0.6 * opacity * (1 - 0.7 * hov));
            const Rect ring = at.Grow(2.5);
            // (hovered, the edge's ring gives way to the hover's own: it covered the bubble's band round the logo, Lvcid)
            pmui::Box(root, "logo.edge", 5, ring, A(kBlack, 0), ring.w / 2)
                .Outline(2, A(ux::Hex(BallestLight() ? "#2E3034" : "#2A2C31"), opacity * (1 - hov)));
        }
        logo::Draw(root, "logo", 6, at, look, opacity);
        if (hov > 0.01 && BallestNight()) {
            // Ballest Night (Lvcid): the black bubble's swirl round its frame (under the logo: only a band shows past its
            // edge) and a soft white glow on the edge in place of a line
            pmui::Bubble(root, "logo.bb", 5, lr.Grow(5), lr.w / 2 + 5, hov * shown, 2);
            // (and a ring of ballest gold round it, as the game's goal ring (Lvcid): the chrome ring's shading in gold, its
            // light turning slowly, a soft gold glow about it)
            pmui::Pic(root, "logo.gglow", 11, lr.Grow(7), "pm-ring-glow.png", A(kBallestGold, 0.75 * hov)).Opacity(static_cast<float>(shown));
            ChromeRing(root, "logo.gold", 11, lr.Grow(2), A(Mix(kBallestGold, kWhite, 0.15), hov * shown), kChromeTurn * 0.6);      // (brighter: Lvcid)
        } else if (hov > 0.01) {
            pmui::Box(root, "logo.b", 11, lr, A(kTile, 0), lr.w / 2).Outline(3, A(kNuLime, 0.85 * hov));
            pmui::Pic(root, "logo.ring", 11, lr.Grow(-2), "pm-ring-glow.png", A(kNuLime, 0.55 * hov)).Opacity(static_cast<float>(shown));
        }
        if (!closing && !welcome && pmui::Click(root, lr, "logo") && now - gRefreshAt >= kRefreshSpin) RefreshAll();     // (not mid-spin)
    }
    if (!gS.open && !closing) {             // closed by its x just now: the rest is drawn, fading from this frame
        closing = true;
        pmui::SetModal(1000);
    }
    if (welcome) {                          // (the intro: the window hidden, there to fade in)
        gBannerLive = false;
        ux::SetThemeMap(nullptr);
        ux::SetOutlineFade(1);
        pmui::End();
        return;
    }
    double y = kTopH + 3;
    y += Banner(wl, W, y);
    gS.overlayT = ux::Approach(gS.overlayT, gS.overlay.empty() ? 0.0 : 1.0, pmui::Dt(), 0.05);
    if (gS.overlay.empty() && gS.overlayT <= 0.001) gS.overlayShown.clear();
    if (gS.overlayT < 0.999) {
        const Rect page{0, y, W, H - y};
        if (gS.page == "cosmetics") CosmeticsPage(wl, page);
        else PluginsPage(wl, page);
    }
    if (!gS.overlayShown.empty()) {
        pmui::Level(kBarLevel);
        const Rect area{0, kTopH + 3, W, H - kTopH - 3};
        if (gS.overlayShown == "settings") SettingsOverlay(wl, area, Smooth(gS.overlayT));
        else ConsoleOverlay(wl, area, Smooth(gS.overlayT));
    }
    Popup(wl, W, H);
    // The resize handles (above): a thin line shows the edge under the pointer (or dragged), a grip marks the bottom
    // right corner.
    pmui::Level(kBarLevel);
    pmui::Pierce(true);
    for (const Handle& hd : handles) {
        const std::string id = std::string("resize.") + hd.edge, e = hd.edge;
        const double h = Ease(id, gResize.edge == e || edgeHere == e, 0.05);
        if (edgeHere == e) pmui::Click(root, hd.r, id, false);
        if (pmui::Held(id) && gResize.edge.empty()) gResize = {e, ws, hs, pmui::Mouse().x, pmui::Mouse().y};
        if (h < 0.01) continue;
        const Color line = A(kLime, 0.35 * h);      // (muted: the lime was a bit much along the edge, Lvcid)
        if (e.find('l') != std::string::npos) pmui::Box(root, id + ".l", 4, {win.x, win.y + 24, 2, win.h - 48}, line, 1);
        if (e.find('r') != std::string::npos) pmui::Box(root, id + ".r", 4, {win.R() - 2, win.y + 24, 2, win.h - 48}, line, 1);
        if (e.find('t') != std::string::npos) pmui::Box(root, id + ".t", 4, {win.x + 24, win.y, win.w - 48, 2}, line, 1);
        if (e.find('b') != std::string::npos) pmui::Box(root, id + ".b", 4, {win.x + 24, win.B() - 2, win.w - 48, 2}, line, 1);
    }
    pmui::Pierce(false);
    const double grip = Ease("resize.grip", gResize.edge == "br" || edgeHere == "br", 0.05);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j <= i; ++j)
            pmui::Box(root, "grip" + std::to_string(i) + std::to_string(j), 4, {win.R() - 12 - 5.0 * (i - j), win.B() - 12 - 5.0 * j, 2.5, 2.5},
                      Mix(ux::Hex("#3A3D42"), kLime, grip), 1.25);
    ux::SetThemeMap(nullptr);               // (the key legend, on the game: as it is)
    ux::SetOutlineFade(1);
    Legend(root, win, dim, closing ? shown : EaseOut((pmui::Now() - gOpenedAt) / kDimIn));
    pmui::End();
    if (outside && gResize.edge.empty()) {
        hostlog::Info("manager: closed by a click outside the window");
        Close();
    }
    gTypedLastFrame = gTypingField != nullptr;
    if (gTypingField && !closing) game::SetTypingWidget(gTypingField);
}

}  // namespace manager
