#include "packages.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

#include "cosmetics.hpp"
#include "game.hpp"
#include "log.hpp"
#include "plugins.hpp"
#include "registry.hpp"
#include "storage.hpp"

namespace packages {
namespace {

constexpr double kRestoreSeconds = 30;      // a loadout's choice of a cosmetic not added yet is tried this long
constexpr double kSaveDelay = 0.5;
constexpr int kMaxLoadouts = 5;             // loadouts at most (one made the first time; "+" adds one, up to these)

// --- files ----------------------------------------------------------------------------------------------------------
void MakeFolders(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
}

bool ReadAll(const std::wstring& file, std::string* out, size_t limit = 4 * 1024 * 1024) {
    out->clear();
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0 && out->size() < limit) out->append(buf, n);
    CloseHandle(h);
    return true;
}

// Written to a temporary file and moved over the old one, so a failed write never leaves half a file.
bool WriteAll(const std::wstring& file, const std::string& text) {
    const std::wstring temp = file + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// --- settings.txt: the small part of TOML it uses --------------------------------------------------------------------
std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string Unquote(const std::string& v) {
    if (v.size() < 2 || v.front() != '"' || v.back() != '"') return v;
    std::string out;
    for (size_t i = 1; i + 1 < v.size(); ++i) {
        if (v[i] == '\\' && i + 2 < v.size()) {
            const char n = v[++i];
            out.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n);
        } else {
            out.push_back(v[i]);
        }
    }
    return out;
}

using Toml = std::map<std::string, std::string>;      // "table.key" (top level: ".key") -> the raw value

Toml ParseToml(const std::string& text) {
    Toml out;
    std::string table, line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        bool quoted = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && quoted) {
                ++i;
                continue;
            }
            if (line[i] == '"') quoted = !quoted;
            if (line[i] == '#' && !quoted) {
                line.resize(i);
                break;
            }
        }
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') table = Trim(line.substr(1, line.size() - 2));
        else if (const size_t eq = line.find('='); eq != std::string::npos) out[table + "." + Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    return out;
}

std::string Str(const Toml& t, const std::string& key, const std::string& fallback = "") {
    const auto it = t.find(key);
    return it == t.end() ? fallback : Unquote(it->second);
}
double Num(const Toml& t, const std::string& key, double fallback) {
    const auto it = t.find(key);
    if (it == t.end()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end && end != it->second.c_str() ? v : fallback;
}

// --- the slot table --------------------------------------------------------------------------------------------------
const std::vector<Slot> kSlots = {
    {"ball", "balls", true, false}, {"hat", "hats", true, false}, {"bfx", "bfx", true, false},
    {"arms", "arms", false, true}, {"bounce", "bounce", false, true},
};

int KindIndex(const std::string& slot) { return slot == "ball" ? 0 : slot == "hat" ? 1 : slot == "bfx" ? 2 : -1; }

// --- state -----------------------------------------------------------------------------------------------------------
Settings gSettings;
bool gSettingsLoaded = false;
std::vector<Item> gItems;

std::wstring SettingsFile() { return Root() + L"\\settings.txt"; }
std::wstring LoadoutsFile() { return Root() + L"\\loadouts.txt"; }

void LoadSettings() {
    gSettingsLoaded = true;
    std::string text;
    if (!ReadAll(SettingsFile(), &text)) return;
    const Toml t = ParseToml(text);
    const int style = static_cast<int>(Num(t, ".label_style", 2));
    gSettings.labelStyle = style == 1 ? 1 : 2;
    const std::string theme = Str(t, ".panel_theme", "default");
    gSettings.theme = theme == "charcoal" ? 1 : theme == "night" ? 2 : 0;
}

void SaveSettings() {
    MakeFolders(Root());
    std::string text = "# Cosmetic Kit settings (the host writes this file; unknown lines are not kept)\n";
    text += "label_style = " + std::to_string(gSettings.labelStyle) + "        # slot tab names: 1 sideways, 2 horizontal\n";
    static const char* const kThemes[3] = {"default", "charcoal", "night"};
    text += std::string("panel_theme = \"") + kThemes[std::clamp(gSettings.theme, 0, 2)] + "\"   # the Customize panel: default, charcoal, night\n";
    if (!WriteAll(SettingsFile(), text)) hostlog::Warn("cosmetics: settings.txt could not be written");
}

// --- what cosmetic plugins add ----------------------------------------------------------------------------------------
size_t gNoted = 0;                          // cosmetics plugins added (for Status)

// The plugin whose folder a file is in ("" for none): plugins\<id>\... or plugins\cosmetics\<id>\...
std::string PluginOf(const std::wstring& file) {
    for (const plugins::Info& info : plugins::List()) {
        const std::wstring dir = plugins::FolderOf(info.id) + L"\\";
        if (dir.size() > 1 && file.size() > dir.size() && _wcsnicmp(file.c_str(), dir.c_str(), dir.size()) == 0) return info.id;
    }
    return "";
}

// One of a plugin's cosmetics: an item of its plugin's pack (the pack is the plugin's id).
void AddNote(const std::string& category, const std::string& id, const std::string& name, const std::wstring& preview,
             const std::wstring& model, const std::wstring& anyFile) {
    if (Find(category, id)) return;
    Item item;
    item.id = id;
    item.name = name;
    item.category = category;
    item.plugin = PluginOf(anyFile);
    if (item.plugin.empty()) item.plugin = plugins::CallerId();     // a cosmetic with no files of its own
    item.pack = item.plugin;
    item.preview = preview;
    item.model = model;
    plugins::Info info;
    if (plugins::Find(item.plugin, &info)) {
        item.author = info.author;
        item.version = info.version;
    }
    gItems.push_back(item);
    ++gNoted;
}

double gReadyAt = -1;                      // when the game was first ready (Cosmetic Kit on)

// --- loadouts --------------------------------------------------------------------------------------------------------
struct Loadout {
    std::string name;
    std::map<std::string, std::string> values;                  // slot -> custom id, game asset path, or ""
    std::vector<std::pair<std::string, std::string>> other;     // lines kept as they are (gloss, texture, unknown)
};
std::vector<Loadout> gLoadouts;
int gActive = 0;
bool gLoadoutsLoaded = false;
double gSaveAt = -1;
struct PendingChoice {
    std::string slot, value;
};
std::vector<PendingChoice> gPending;        // choices of the active loadout not wearable yet (their cosmetic is not added)
double gRestoreUntil = -1;
double gLastTrack = -100;

void SaveLoadouts() {
    MakeFolders(Root());
    std::string text = "# Cosmetic Kit loadouts: a choice for every slot (a custom id, a game asset path, or empty)\n";
    text += "active = " + std::to_string(gActive + 1) + "\n";
    for (size_t i = 0; i < gLoadouts.size(); ++i) {
        const Loadout& l = gLoadouts[i];
        text += "\n[loadout " + std::to_string(i + 1) + "]\n";
        text += "name = " + l.name + "\n";
        std::set<std::string> written;
        for (const Slot& s : kSlots) {
            const auto it = l.values.find(s.id);
            text += std::string(s.id) + " = " + (it == l.values.end() ? "" : it->second) + "\n";
            written.insert(s.id);
        }
        for (const auto& [slot, value] : l.values)
            if (!written.count(slot)) text += slot + " = " + value + "\n";
        for (const auto& [key, value] : l.other) text += key + " = " + value + "\n";
    }
    if (!WriteAll(LoadoutsFile(), text)) hostlog::Warn("cosmetics: loadouts.txt could not be written");
    gSaveAt = -1;
}

void LoadLoadouts() {
    gLoadoutsLoaded = true;
    std::string text;
    if (ReadAll(LoadoutsFile(), &text) && !text.empty()) {
        std::istringstream in(text);
        int active = 1;
        for (std::string line; std::getline(in, line);) {
            line = Trim(line);
            if (line.empty() || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                gLoadouts.push_back({});
                gLoadouts.back().name = "Loadout " + std::to_string(gLoadouts.size());
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
            if (gLoadouts.empty()) {
                if (key == "active") active = std::atoi(value.c_str());
                continue;
            }
            Loadout& l = gLoadouts.back();
            // Every other key is a slot (one this host doesn't know, a newer host's, is kept and written back as it is).
            if (key == "name") l.name = value;
            // (gloss, texture and texture_on are the loadout's look, not slots: texture_on read as a slot was lost on reload)
            else if (key == "gloss" || key == "texture" || key == "texture_on") l.other.emplace_back(key, value);
            else l.values[key] = value;
        }
        if (gLoadouts.size() > static_cast<size_t>(kMaxLoadouts)) gLoadouts.resize(kMaxLoadouts);     // (an older test's extras)
        gActive = std::clamp(active - 1, 0, std::max(0, static_cast<int>(gLoadouts.size()) - 1));
    }
    if (!gLoadouts.empty()) return;
    // The first time: loadout 1 is what was worn (the local game cosmetics of cosmetics_local.txt, and Cosmetic Kit's
    // saved custom ones, which win as they did); more are added with "+".
    Loadout first;
    first.name = "Loadout 1";
    const char* const kinds[3] = {"ball", "hat", "bfx"};
    for (int k = 0; k < 3; ++k) {
        std::string value = cosmetics::LocalGamePath(static_cast<cosmetics::Kind>(k));
        const std::string custom = storage::Get("cosmetic-kit", kinds[k], "");
        if (!custom.empty()) value = custom;
        first.values[kinds[k]] = value;
    }
    for (const std::string& slot : cosmetics::ExtraSlotNames()) first.values[slot] = cosmetics::EquippedExtra(slot);
    gLoadouts.push_back(first);
    gActive = 0;
    SaveLoadouts();
    hostlog::Info("cosmetics: loadouts made (loadout 1 from what was worn)");
}

bool WearChoice(const std::string& slot, const std::string& value) {
    const int k = KindIndex(slot);
    if (k >= 0) {
        const auto kind = static_cast<cosmetics::Kind>(k);
        if (value.empty()) {
            cosmetics::Equip(kind, "");
            cosmetics::SetLocalGamePath(kind, "");
            return true;
        }
        if (value[0] == '/') {
            cosmetics::Equip(kind, "");
            return cosmetics::SetLocalGamePath(kind, value);
        }
        return cosmetics::Has(kind, value) && cosmetics::Equip(kind, value);
    }
    if (value.empty()) {
        cosmetics::EquipExtra(slot, "");
        return true;
    }
    return cosmetics::EquipExtra(slot, value);
}

// A loadout's gloss and texture lines ("gloss = on", "texture = 40", "texture_on = true"), if it has them.
bool PrefsOf(const Loadout& l, cosmetics::SkinPrefs* out) {
    bool any = false;
    for (const auto& [key, value] : l.other) {
        if (key == "gloss") out->gloss = value != "off";
        else if (key == "texture") out->texture = std::atof(value.c_str());
        else if (key == "texture_on") out->textureOn = value == "true";
        else continue;
        any = true;
    }
    return any;
}

void SetOther(Loadout& l, const std::string& key, const std::string& value) {
    for (auto& [k, v] : l.other)
        if (k == key) {
            v = value;
            return;
        }
    l.other.emplace_back(key, value);
}

void Apply(int index) {
    gPending.clear();
    if (index < 0 || index >= static_cast<int>(gLoadouts.size())) return;
    const Loadout& l = gLoadouts[static_cast<size_t>(index)];
    cosmetics::SkinPrefs prefs;
    const bool hasPrefs = PrefsOf(l, &prefs);
    cosmetics::SetLocalSkinPrefs(prefs, hasPrefs);
    std::set<std::string> slots;
    for (const Slot& s : kSlots) slots.insert(s.id);
    for (const std::string& s : cosmetics::ExtraSlotNames()) slots.insert(s);
    for (const auto& [slot, value] : l.values) slots.insert(slot);
    for (const std::string& slot : slots) {
        const auto it = l.values.find(slot);
        const std::string value = it == l.values.end() ? "" : it->second;
        if (!WearChoice(slot, value)) gPending.push_back({slot, value});
    }
}

std::string WornNow(const std::string& slot) {
    const int k = KindIndex(slot);
    if (k < 0) return cosmetics::EquippedExtra(slot);
    const auto kind = static_cast<cosmetics::Kind>(k);
    const std::string custom = cosmetics::Equipped(kind);
    return custom.empty() ? cosmetics::LocalGamePath(kind) : custom;
}

// What is worn, written into the active loadout when it changes (the Customize page's tiles, a plugin).
void Track() {
    if (gLoadouts.empty() || game::Seconds() - gLastTrack < 0.25) return;
    gLastTrack = game::Seconds();
    Loadout& l = gLoadouts[static_cast<size_t>(gActive)];
    cosmetics::SkinPrefs prefs;
    if (cosmetics::TakeLocalSkinPrefs(&prefs)) {
        char texture[32];
        std::snprintf(texture, sizeof texture, "%g", prefs.texture);
        SetOther(l, "gloss", prefs.gloss ? "on" : "off");
        SetOther(l, "texture", texture);
        SetOther(l, "texture_on", prefs.textureOn ? "true" : "false");
        if (gSaveAt < 0) gSaveAt = game::Seconds() + kSaveDelay;
    }
    // (only the slots shown: one hidden (Cosmetic Kit Plus not running, say) keeps its choice in the loadout instead of
    // having "nothing worn" written over it once its pending wear gives up)
    const std::vector<std::string> slots = ShownSlots(true);
    for (const std::string& slot : slots) {
        if (std::any_of(gPending.begin(), gPending.end(), [&](const PendingChoice& p) { return p.slot == slot; })) continue;
        const std::string now = WornNow(slot);
        const auto it = l.values.find(slot);
        const std::string kept = it == l.values.end() ? "" : it->second;
        if (now == kept) continue;
        l.values[slot] = now;
        if (gSaveAt < 0) gSaveAt = game::Seconds() + kSaveDelay;
    }
}

bool gKitWasOn = false;

}  // namespace

// --- public ----------------------------------------------------------------------------------------------------------
const std::vector<Slot>& Slots() { return kSlots; }

std::vector<std::string> ShownSlots(bool local) {
    std::vector<std::string> out;
    const bool plus = local && PlusOn();
    for (const Slot& s : kSlots)
        if (s.isPublic || (local && (!s.needsMoreSlots || plus))) out.push_back(s.id);
    // (a slot a plugin has filled (Cosmetics::AddExtra) shows whether or not Plus runs, as upstream's host shows it)
    if (local)
        for (const std::string& slot : cosmetics::ExtraSlotNames())
            if (std::find(out.begin(), out.end(), slot) == out.end()) out.push_back(slot);
    return out;
}

std::string LabelOf(const std::string& slot) {
    for (const Slot& s : kSlots)
        if (slot == s.id) return s.label;
    return slot;
}

bool KitOn() {
    plugins::Info info;
    return plugins::Find("cosmetic-kit", &info) && !plugins::IsOff("cosmetic-kit") && info.status == "running";
}

bool PlusOn() {
    plugins::Info info;
    return KitOn() && plugins::Find("cosmetic-kit-plus", &info) && !plugins::IsOff("cosmetic-kit-plus") && info.status == "running";
}

const Settings& Config() {
    if (!gSettingsLoaded) LoadSettings();
    return gSettings;
}
void SetTheme(int theme) {
    Config();
    theme = std::clamp(theme, 0, 2);
    if (gSettings.theme == theme) return;
    gSettings.theme = theme;
    SaveSettings();
}
void SetLabelStyle(int style) {
    Config();
    style = style == 1 ? 1 : 2;
    if (gSettings.labelStyle == style) return;
    gSettings.labelStyle = style;
    SaveSettings();
}

const std::vector<Item>& Items() { return gItems; }

const Item* Find(const std::string& category, const std::string& id) {
    for (const auto& item : gItems)
        if (item.category == category && item.id == id) return &item;
    return nullptr;
}

std::wstring Root() { return hostlog::DataDir() + L"\\cosmetics"; }

void NoteBall(const std::string& id, const std::string& name, const std::wstring& image, const std::wstring& preview,
              const std::wstring& model) {
    AddNote("ball", id, name, preview, model, !model.empty() ? model : !preview.empty() ? preview : image);
}

void NoteHat(const std::string& id, const std::string& name, const std::string& /*mesh*/, double /*scale*/, const std::wstring& preview,
             const std::wstring& model) {
    AddNote("hat", id, name, preview, model, !model.empty() ? model : preview);
}

void NoteBfx(const std::string& id, const std::string& name, const std::string& /*base*/, double /*scale*/, const std::wstring& preview,
             const std::string& /*effect*/, const std::string& /*sound*/) {
    AddNote("bfx", id, name, preview, L"", preview);
}

void NoteExtra(const std::string& slot, const std::string& id, const std::string& name, const std::wstring& preview,
               const std::wstring& model) {
    AddNote(slot, id, name, preview, model, !model.empty() ? model : preview);
}

int LoadoutCount() { return static_cast<int>(gLoadouts.size()); }
int ActiveLoadout() { return gActive; }

void UseLoadout(int index) {
    if (index < 0 || index >= static_cast<int>(gLoadouts.size()) || index == gActive) return;
    if (gSaveAt >= 0) SaveLoadouts();       // the one being left, as it is
    gActive = index;
    Apply(gActive);
    gRestoreUntil = game::Seconds() + 2;    // a choice whose cosmetic is still being added
    gLastTrack = game::Seconds();
    SaveLoadouts();
    hostlog::Info("cosmetics: loadout " + std::to_string(gActive + 1));
}

int MaxLoadouts() { return kMaxLoadouts; }

int AddLoadout() {
    if (gLoadouts.empty() || gLoadouts.size() >= static_cast<size_t>(kMaxLoadouts)) return -1;
    Loadout copy = gLoadouts[static_cast<size_t>(gActive)];
    copy.name = "Loadout " + std::to_string(gLoadouts.size() + 1);
    gLoadouts.push_back(copy);
    gActive = static_cast<int>(gLoadouts.size()) - 1;
    SaveLoadouts();
    hostlog::Info("cosmetics: new loadout " + std::to_string(gActive + 1));
    return gActive;
}

void Frame() {
    const bool on = KitOn();
    if (on != gKitWasOn) {
        gKitWasOn = on;
        hostlog::Info(std::string("cosmetics: Cosmetic Kit ") + (on ? "on: loadouts and the new Customize page" : "off"));
    }
    if (!on) return;
    if (!game::PlayerController()) return;
    if (gReadyAt < 0) {
        gReadyAt = game::Seconds();
        Config();
    }
    if (!gLoadoutsLoaded && game::Seconds() - gReadyAt > 1) {
        LoadLoadouts();
        Apply(gActive);
        gRestoreUntil = game::Seconds() + kRestoreSeconds;
    }
    if (!gPending.empty()) {
        for (size_t i = gPending.size(); i-- > 0;)
            if (WearChoice(gPending[i].slot, gPending[i].value)) gPending.erase(gPending.begin() + static_cast<std::ptrdiff_t>(i));
        if (!gPending.empty() && game::Seconds() > gRestoreUntil) {
            for (const auto& p : gPending) hostlog::Info("cosmetics: loadout " + std::to_string(gActive + 1) + ": " + p.slot + " " + p.value + " is not there");
            gPending.clear();
        }
    }
    if (gLoadoutsLoaded) Track();
    if (gSaveAt >= 0 && game::Seconds() >= gSaveAt) SaveLoadouts();
}

std::string Status() {
    return std::string("packages: kit ") + (KitOn() ? "on" : "off") + ", " + std::to_string(gItems.size()) + " item(s), " +
           std::to_string(gNoted) + " from plugins; loadout " + std::to_string(gActive + 1) + " of " +
           std::to_string(gLoadouts.size()) + "; style " + std::to_string(Config().labelStyle) + ", Cosmetic Kit Plus " +
           (PlusOn() ? "on" : "off");
}

}  // namespace packages
