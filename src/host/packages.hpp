// Cosmetics 2: the slot table, Cosmetic Kit's settings, loadouts, and every cosmetic known. Shared cosmetics are plugins
// (each its own repo, listed in the registry, installed into plugins\cosmetics\<id>); they add theirs through the
// Cosmetics API, and each is noted here as part of its plugin's pack. The player's choices are under
//   %LOCALAPPDATA%\Ballest\Saved\PluginManager\cosmetics\
//     loadouts.txt                any number of loadouts, one active, a choice for every slot each
//     settings.txt                Cosmetic Kit's settings (the slot tabs' label style, the panel theme)
// All of it only while Cosmetic Kit is installed and on: it is the feature set's switch.
// Game thread only.
#pragma once
#include <string>
#include <vector>

namespace packages {

// --- slots ------------------------------------------------------------------------------------------------------------
// One table drives the Customize page's slot tabs, loadouts, the browser's sections and the storage folders.
struct Slot {
    const char* id;                     // also the package category and the folder name
    const char* label;                  // on its tab
    bool isPublic;                      // the game's own slot (shown in public mode; worn through the game's page)
    bool needsMoreSlots;                // an extra slot: shown only while Cosmetic Kit Plus runs (PlusOn)
};
const std::vector<Slot>& Slots();
// The slot tabs the Customize page shows, top to bottom: the table's (the public ones only in public mode, its extra
// ones, empty or not, while Cosmetic Kit Plus runs), then any extra slot a plugin has filled (Cosmetics::AddExtra),
// Plus or not, as upstream's host shows them.
std::vector<std::string> ShownSlots(bool local);
std::string LabelOf(const std::string& slot);

// --- Cosmetic Kit ------------------------------------------------------------------------------------------------------
bool KitOn();                           // Cosmetic Kit is installed and turned on
// Cosmetic Kit Plus is installed, turned on and running: the extra slots (arms, bounce effects, ...) are its.
bool PlusOn();

struct Settings {
    int labelStyle = 2;                 // the slot tabs' names: 1 sideways, 2 horizontal
    int theme = 0;                      // the Customize panel: 0 the game's (light), 1 charcoal, 2 Ballest night
};
const Settings& Config();
void SetLabelStyle(int style);
void SetTheme(int theme);

// --- cosmetics ---------------------------------------------------------------------------------------------------------
struct Item {
    std::string id, name, category, author, version, pack, credits, license, added, plugin;
    std::wstring preview;               // its picture file, or ""
    std::wstring model;                 // its model text file, or ""
};
const std::vector<Item>& Items();       // every cosmetic known: plugins' (in the order they came)
const Item* Find(const std::string& category, const std::string& id);
std::wstring Root();

// What a plugin added through the Cosmetics API, noted as one of its plugin's pack (api.cpp). Paths are full paths in
// the plugins folder; `model` is the model's file as the plugin gave it.
void NoteBall(const std::string& id, const std::string& name, const std::wstring& image, const std::wstring& preview,
              const std::wstring& model);
void NoteHat(const std::string& id, const std::string& name, const std::string& mesh, double scale, const std::wstring& preview,
             const std::wstring& model);
void NoteBfx(const std::string& id, const std::string& name, const std::string& base, double scale, const std::wstring& preview,
             const std::string& effect, const std::string& sound);
void NoteExtra(const std::string& slot, const std::string& id, const std::string& name, const std::wstring& preview,
               const std::wstring& model);

// --- loadouts ----------------------------------------------------------------------------------------------------------
int LoadoutCount();
int ActiveLoadout();                    // 0-based
void UseLoadout(int index);             // wears it at once
int AddLoadout();                       // a copy of the active one, made active; its index (-1 at the cap, 5)
int MaxLoadouts();

void Frame();                           // every frame, before cosmetics::Frame
std::string Status();

}  // namespace packages
