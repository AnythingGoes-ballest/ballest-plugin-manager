#include <algorithm>
#include "tracks.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "engine.hpp"
#include "game.hpp"
#include "cosmetics.hpp"
#include "log.hpp"

namespace tracks {
namespace {

using eng::Obj;

std::string gSearchState = "idle", gOpenState = "idle";
std::vector<steam::Item> gResults;
int gTotal = 0, gSearchSerial = 0;
uint64_t gOpening = 0;
double gOpeningSince = 0;
constexpr double kDownloadTimeout = 120;

Obj GameInstance() {
    Obj controller = game::PlayerController();
    return controller ? eng::Call(eng::FindCdo("GameplayStatics"), "GetGameInstance", controller).ReturnObj() : nullptr;
}

std::string FString(const uint8_t* at) { return eng::ReadFString(at); }

std::string MakeFString(const std::string& text, std::wstring* keep, eng::FString* out) {
    *keep = eng::Widen(text);
    *out = eng::FString{keep->c_str(), static_cast<int32_t>(keep->size() + 1), static_cast<int32_t>(keep->size() + 1)};
    return text;
}

// The level data the game keeps for one of its tracks (its master level library).
Obj LibraryLevelData(const std::string& key) {
    Obj instance = GameInstance();
    Obj library = instance ? eng::ReadObj(instance, "LevelLibrary") : nullptr;
    Obj fn = library ? eng::FindFunction(eng::ClassOf(library), "Try Get Level Data By String") : nullptr;
    if (!fn) return nullptr;
    eng::Params p(fn);
    std::wstring keep;
    eng::FString name{};
    MakeFString(key, &keep, &name);
    p.Set("LevelName", name);
    if (!eng::Invoke(library, p)) return nullptr;
    return p.GetObj("LevelData");
}

// The level data the game keeps for one of its tracks. Mostly its master level library, keyed by level name; the
// tower trials aren't (read from the game's files): The Tower's level data (DA_Map_Climb01, LevelName Map_TheTower)
// is filed under the key "Map_Climb01", and The Tower CPs' (DA_Map_TheTowerCP) isn't in the library at all but in
// its map's folder, as /Game/Maps/COOKEDMAPS/<level>/DA_<level>.
Obj LevelData(const std::string& level) {
    if (Obj data = LibraryLevelData(level)) return data;
    if (level == "Map_TheTower")
        if (Obj data = LibraryLevelData("Map_Climb01")) return data;
    const std::wstring path = L"/Game/Maps/COOKEDMAPS/" + eng::Widen(level) + L"/DA_" + eng::Widen(level) + L".DA_" + eng::Widen(level);
    return cosmetics::LoadAsset(path);
}

std::string PlaylistLevels(Obj list, std::vector<std::string>* out) {
    uint8_t array[16] = {};
    if (!list || !eng::ReadBytes(list, "LevelNumberLevelNameMap", array, sizeof array)) return "";
    struct {
        const eng::FString* data;
        int32_t num, max;
    } a{};
    std::memcpy(&a, array, sizeof a);
    for (int32_t i = 0; i < a.num && a.data; ++i) out->push_back(eng::ReadFString(reinterpret_cast<const uint8_t*>(&a.data[i])));
    return "";
}

std::vector<std::string> StringArray(const uint8_t* at) {
    struct {
        const eng::FString* data;
        int32_t num, max;
    } array{};
    std::vector<std::string> out;
    if (!at) return out;
    std::memcpy(&array, at, sizeof array);
    for (int32_t i = 0; i < array.num && array.data; ++i) out.push_back(eng::ReadFString(reinterpret_cast<const uint8_t*>(&array.data[i])));
    return out;
}

// As the game's own track preview opens a workshop track (read from WBP_TrackPreview and O_LevelManager): the level
// manager makes level data of the installed item (ProcessDownloadedLevel: finds its .ballmap and registers it), and
// the game instance enters it (TryEnterLevelByString(level data's UGC map path, level data, "Map_LevelEditorMain"),
// which reads the map file, claims it with the track hub and begins the level transition).
bool Open(uint64_t id) {
    Obj instance = GameInstance();
    if (!instance) {
        gOpenState = "error: no game instance";
        return false;
    }
    Obj manager = nullptr;
    if (Obj get = eng::FindFunction(eng::ClassOf(instance), "GetLevelManager")) {
        eng::Params p(get);
        if (eng::Invoke(instance, p)) manager = p.GetObj("LevelManager");
    }
    Obj process = manager ? eng::FindFunction(eng::ClassOf(manager), "ProcessDownloadedLevel") : nullptr;
    if (!process) {
        gOpenState = "error: the game's level manager isn't there";
        return false;
    }
    eng::Params made(process);
    const int64_t fileId = static_cast<int64_t>(id);
    made.Set("PublishedFileId", &fileId, sizeof fileId);            // SIK_PublishedFileId: the id, 8 bytes
    Obj data = eng::Invoke(manager, made) ? made.GetObj("OutLevelData") : nullptr;
    const uint8_t* ok = made.Get("bSucess");
    if (!data || !ok || !*ok) {
        gOpenState = "error: the game couldn't read the track's files";
        hostlog::Warn("tracks: ProcessDownloadedLevel failed for workshop " + std::to_string(id));
        return false;
    }
    uint8_t mapPath[16] = {};
    eng::ReadBytes(data, "? UGCMap Path", mapPath, sizeof mapPath);
    const std::string path = FString(mapPath);
    Obj enter = eng::FindFunction(eng::ClassOf(instance), "TryEnterLevelByString");
    if (!enter) {
        gOpenState = "error: the game can't enter levels here";
        return false;
    }
    eng::Params p(enter);
    std::wstring keepPath, keepOverride;
    eng::FString name{}, override{};
    MakeFString(path, &keepPath, &name);
    MakeFString("Map_LevelEditorMain", &keepOverride, &override);
    p.Set("LevelPathOrName", name);
    p.Set("LevelData", data);
    p.Set("OverrideLevelName", override);
    if (!eng::Invoke(instance, p)) {
        gOpenState = "error: the game didn't open the track";
        return false;
    }
    const uint8_t* entered = p.Get("bSuccessful");
    const uint8_t* message = p.Get("Message");
    const std::string said = message ? FString(message) : "";
    hostlog::Info("tracks: opening workshop " + std::to_string(id) + " (" + path + "): " + (entered && *entered ? "ok" : "refused") +
                  (said.empty() ? "" : " - " + said));
    // "Waiting for Hub Claim Check" (measured): the game checks the track with its hub first and enters it itself
    // when the check comes back, so this is on its way rather than refused.
    const bool claiming = said.find("Hub Claim") != std::string::npos;
    if ((!entered || !*entered) && !claiming) {
        gOpenState = "error: " + (said.empty() ? std::string("the game refused to open it") : said);
        return false;
    }
    gOpenState = "open";
    return true;
}

}  // namespace

void LocalFrame();

void Frame() {
    LocalFrame();
    if (!gOpening) return;
    const uint32_t state = steam::ItemState(gOpening);
    if ((state & steam::kItemInstalled) && !(state & (steam::kItemDownloading | steam::kItemDownloadPending))) {
        const uint64_t id = gOpening;
        gOpening = 0;
        gOpenState = "opening";
        Open(id);
    } else if (game::Seconds() - gOpeningSince > kDownloadTimeout) {
        gOpening = 0;
        gOpenState = "error: the download didn't finish";
    }
}

std::vector<OfficialTrack> OfficialTracks() {
    std::vector<OfficialTrack> all;
    Obj instance = GameInstance();
    Obj seasons = instance ? eng::ReadObj(instance, "CircuitSeasons") : nullptr;
    int number = 0;
    for (Obj list : seasons ? eng::ReadObjArray(seasons, "NumberedSeasons") : std::vector<Obj>{}) {
        std::vector<std::string> levels;
        PlaylistLevels(list, &levels);
        ++number;
        for (const auto& level : levels) all.push_back({level, "season " + std::to_string(number)});
    }
    const std::pair<const wchar_t*, const char*> others[] = {
        {L"/Game/Maps/Playlists/DA_CollabLevels_ShippingList.DA_CollabLevels_ShippingList", "collabs"},
        {L"/Game/Maps/Playlists/DA_TowerLevels_ShippingList.DA_TowerLevels_ShippingList", "trials"}};
    for (const auto& [path, group] : others) {
        std::vector<std::string> levels;
        PlaylistLevels(cosmetics::LoadAsset(path), &levels);
        for (const auto& level : levels) all.push_back({level, group});
    }
    return all;
}

std::vector<std::string> Official() {
    Obj instance = GameInstance();
    Obj fn = instance ? eng::FindFunction(eng::ClassOf(instance), "GetCircuitMapNames") : nullptr;
    if (!fn) return {};
    eng::Params p(fn);
    if (!eng::Invoke(instance, p)) return {};
    return StringArray(p.Get("LevelNumberLevelNameMap"));
}

std::string Title(const std::string& level) {
    Obj data = LevelData(level);
    uint8_t name[16] = {};
    return data && eng::ReadBytes(data, "?OptionalDescriptiveName", name, sizeof name) ? FString(name) : "";
}

std::string Image(const std::string& level) {
    Obj data = LevelData(level);
    // LevelPreviewImage: a soft reference (40 bytes in the type dump): a weak pointer (8 bytes), then the
    // FSoftObjectPath {FTopLevelAssetPath {PackageName, AssetName}, SubPathString}.
    uint8_t soft[40] = {};
    if (!data || !eng::ReadBytes(data, "LevelPreviewImage", soft, sizeof soft)) return "";
    uint32_t package[2], asset[2];
    std::memcpy(package, soft + 8, sizeof package);
    std::memcpy(asset, soft + 16, sizeof asset);
    const std::string p = eng::Name(package[0], static_cast<int32_t>(package[1])), a = eng::Name(asset[0], static_cast<int32_t>(asset[1]));
    return p.empty() || a.empty() || p == "None" ? "" : p + "." + a;
}

bool Open(const std::string& level) {
    Obj instance = GameInstance();
    Obj data = LevelData(level);
    Obj fn = instance ? eng::FindFunction(eng::ClassOf(instance), "TryEnterLevelByString") : nullptr;
    if (!fn || !data) {
        hostlog::Warn("tracks: can't open " + level + (data ? "" : ": the game has no level data for it"));
        return false;
    }
    eng::Params p(fn);
    std::wstring keepLevel, keepOverride;
    eng::FString name{}, override{};
    MakeFString(level, &keepLevel, &name);
    MakeFString("Map_LevelEditorMain", &keepOverride, &override);
    p.Set("LevelPathOrName", name);
    p.Set("LevelData", data);
    p.Set("OverrideLevelName", override);
    if (!eng::Invoke(instance, p)) return false;
    uint8_t ok = 0;
    if (const uint8_t* r = p.Get("bSuccessful")) ok = *r;
    hostlog::Info("tracks: opening " + level + (ok ? "" : " (the game refused)"));
    return ok != 0;
}

bool OpenOfficial(int index) {
    Obj instance = GameInstance();
    Obj fn = instance ? eng::FindFunction(eng::ClassOf(instance), "TryEnterLevelByIndex") : nullptr;
    if (!fn) return false;
    eng::Params p(fn);
    p.Set("Index", static_cast<int32_t>(index));
    return eng::Invoke(instance, p);
}

void Search(const std::string& text) {
    const int serial = ++gSearchSerial;
    gSearchState = "searching";
    gResults.clear();
    gTotal = 0;
    steam::SearchWorkshop(text, 1, [serial](bool ok, std::vector<steam::Item> items, int total) {
        if (serial != gSearchSerial) return;
        gSearchState = ok ? "done" : "error: the workshop search failed";
        gResults = std::move(items);
        gTotal = total;
    });
}

std::string SearchState() { return gSearchState; }

namespace {
std::map<uint64_t, bool> gPreviewAsked;
std::wstring PreviewFile(uint64_t id) { return hostlog::DataDir() + L"\\cache\\previews\\" + std::to_wstring(id) + L".jpg"; }
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
}  // namespace

std::string PreviewImage(uint64_t id, uint64_t preview) {
    if (!id) return "";
    // The game's own cache of workshop previews: <id>_<hash>_<size>.img, JPEG (measured).
    const std::wstring saved = hostlog::DataDir() + L"\\..\\HubPreviewCache\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((saved + std::to_wstring(id) + L"_*.img").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        const std::wstring full = saved + fd.cFileName;
        return eng::Narrow(full.c_str(), static_cast<int>(full.size()));
    }
    const std::wstring mine = PreviewFile(id);
    if (Exists(mine)) return eng::Narrow(mine.c_str(), static_cast<int>(mine.size()));
    if (!preview || gPreviewAsked[id]) return "";
    gPreviewAsked[id] = true;
    steam::DownloadFile(preview, [id](bool ok, std::string bytes) {
        if (!ok || bytes.empty()) return;
        const std::wstring dir = hostlog::DataDir() + L"\\cache";
        CreateDirectoryW(dir.c_str(), nullptr);
        CreateDirectoryW((dir + L"\\previews").c_str(), nullptr);
        if (FILE* f = _wfopen(PreviewFile(id).c_str(), L"wb")) {
            fwrite(bytes.data(), 1, bytes.size(), f);
            fclose(f);
        }
    });
    return "";
}

std::string ResultImage(size_t index) {
    if (index >= gResults.size()) return "";
    return PreviewImage(gResults[index].id, gResults[index].preview);
}
const std::vector<steam::Item>& Results() { return gResults; }
int Total() { return gTotal; }

bool OpenWorkshop(uint64_t id) {
    if (!steam::Available() || !id) {
        gOpenState = "error: Steam isn't available";
        return false;
    }
    const uint32_t state = steam::ItemState(id);
    if ((state & steam::kItemInstalled) && !(state & steam::kItemNeedsUpdate)) {
        gOpenState = "opening";
        return Open(id);
    }
    if (!steam::DownloadItem(id)) {
        gOpenState = "error: Steam wouldn't download it";
        return false;
    }
    gOpening = id;
    gOpeningSince = game::Seconds();
    gOpenState = "downloading";
    hostlog::Info("tracks: downloading workshop " + std::to_string(id));
    return true;
}

std::string OpenState() { return gOpenState; }

namespace {
Obj LevelManager() {
    Obj instance = GameInstance();
    Obj get = instance ? eng::FindFunction(eng::ClassOf(instance), "GetLevelManager") : nullptr;
    if (!get) return nullptr;
    eng::Params p(get);
    return eng::Invoke(instance, p) ? p.GetObj("LevelManager") : nullptr;
}

// The game's folder of the player's own maps (O_LevelManager.GetCustomMapDirectory: Saved + the editor's
// MapsFolderDirectory, "...\Saved\UserSavedMaps/"), or "".
std::string CustomMapDirectory() {
    Obj manager = LevelManager();
    Obj fn = manager ? eng::FindFunction(eng::ClassOf(manager), "GetCustomMapDirectory") : nullptr;
    if (!fn) return "";
    eng::Params p(fn);
    if (!eng::Invoke(manager, p)) return "";
    const uint8_t* r = p.Return();
    return r ? FString(r) : "";
}

// levelData.levelPublishedFileId of a map file: > 0 once it has been published (then it races on the workshop
// map's live leaderboard, so it isn't opened as a local map), -1 (or 0) never published. -2 if the file can't be read
// or doesn't say (no levelPublishedFileId in its head, or not a number): unknown counts as not the player's own
// never-published map (fail closed).
long long PublishedIdOf(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return -2;
    std::string head(65536, '\0');
    head.resize(fread(head.data(), 1, head.size(), f));
    fclose(f);
    const size_t at = head.find("\"levelPublishedFileId\"");
    if (at == std::string::npos) return -2;
    const size_t colon = head.find(':', at);
    if (colon == std::string::npos) return -2;
    char* end = nullptr;
    const long long id = std::strtoll(head.c_str() + colon + 1, &end, 10);
    if (end == head.c_str() + colon + 1) return -2;                 // not a number
    return id;
}
std::map<std::string, eng::Weak> gLocalData;    // map file path -> the level data the game made for it
eng::Weak gCupList, gCupPrevious;               // the plugin's list of local maps while it is the active one
bool gCupActive = false;
bool gCupLeftMenu = false;                     // its first map has loaded (the main menu was left)

// "My Track_AnythingGoes" (or the full file name) -> the full path in the game's maps folder, or "".
std::string LocalPath(const std::string& name) {
    const std::string dir = CustomMapDirectory();
    if (dir.empty()) {
        gOpenState = "error: the game's level manager isn't there";
        return "";
    }
    std::string file = name;
    for (char& c : file)
        if (c == '\\') c = '/';
    if (file.find('/') != std::string::npos || file.find("..") != std::string::npos) {
        gOpenState = "error: only a map file name in the game's own maps folder";
        return "";
    }
    if (file.find('&') == std::string::npos) file = "Map_LevelEditorMain&" + file;
    if (file.size() < 9 || file.compare(file.size() - 9, 9, ".balledit") != 0) file += ".balledit";
    std::string path = dir;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += "/";
    path += file;
    const long long published = PublishedIdOf(eng::Widen(path));
    if (published == -2) {
        gOpenState = "error: no such map, or its file doesn't say it was never published: " + path;
        return "";
    }
    if (published > 0) {
        gOpenState = "error: this map has been published (workshop " + std::to_string(published) + "): open it from the workshop";
        return "";
    }
    return path;
}

// The game's level data for a map file of the player's own: O_LevelManager.AddRuntimeLevelData(path) reads the
// file's worldSettings into new level data (bIsUGC, the map path, UGCDirectory = GetCustomMapDirectory, its
// levelPublishedFileId), files it in the game instance's AllLevelAssets and lists it in the level manager's UGC list.
// Its OutLevelData is never set (it hands back its unset local "Data", measured: always None), so the new entry of
// the game instance's AddedLevelDatas is the one, checked by its map path. Made once per file while it lives.
Obj LocalLevelData(const std::string& path) {
    if (Obj cached = eng::Get(gLocalData[path])) return cached;
    Obj manager = LevelManager();
    Obj add = manager ? eng::FindFunction(eng::ClassOf(manager), "AddRuntimeLevelData") : nullptr;
    Obj instance = GameInstance();
    if (!add || !instance) {
        gOpenState = "error: the game can't open map files here";
        return nullptr;
    }
    eng::Params made(add);
    std::wstring keepPath;
    eng::FString fpath{};
    MakeFString(path, &keepPath, &fpath);
    made.Set("Path", fpath);
    const size_t before = eng::ReadObjArray(instance, "AddedLevelDatas").size();
    Obj data = nullptr;
    if (eng::Invoke(manager, made)) {
        data = made.GetObj("OutLevelData");
        const auto added = eng::ReadObjArray(instance, "AddedLevelDatas");
        if (!data && added.size() > before) {
            uint8_t mapPath[16] = {};
            eng::ReadBytes(added.back(), "? UGCMap Path", mapPath, sizeof mapPath);
            if (FString(mapPath) == path) data = added.back();
            else hostlog::Warn("tracks: new level data is for " + FString(mapPath) + ", not " + path);
        }
    }
    if (!data) {
        gOpenState = "error: the game couldn't read the map file";
        hostlog::Warn("tracks: AddRuntimeLevelData failed for " + path);
        return nullptr;
    }
    int64_t fileId = 0;
    eng::ReadBytes(data, "Level Published File ID", &fileId, sizeof fileId);
    if (fileId > 0) {
        gOpenState = "error: the game reads this map as published (" + std::to_string(fileId) + ")";
        return nullptr;
    }
    gLocalData[path] = eng::MakeWeak(data);
    return data;
}

// The map's levelData.levelDisplayName ("My Track"): the key the game files its level data under
// (AddRuntimeLevelData: AllLevelAssets[LevelDisplayName], read from its bytecode) and the active level's name in its
// race (GameInstance.ActiveLevel, measured), which its playlists are searched by. Not the level data's
// OptionalDescriptiveName, which is the userLevelName ("My Track_AnythingGoes", measured).
std::string DisplayName(const std::string& path) {
    FILE* f = _wfopen(eng::Widen(path).c_str(), L"rb");
    if (!f) return "";
    std::string head(65536, ' ');
    head.resize(fread(head.data(), 1, head.size(), f));
    fclose(f);
    const size_t at = head.find("\"levelDisplayName\"");
    const size_t open = at == std::string::npos ? at : head.find('"', head.find(':', at) + 1);
    const size_t close = open == std::string::npos ? open : head.find('"', open + 1);
    return close == std::string::npos ? "" : head.substr(open + 1, close - open - 1);
}

// GameInstance.TryEnterLevelByString(path, data, "Map_LevelEditorMain"): reads the map (LoadMapFromFile with the part
// after '&'), resolves its baseline level and, for a map never published (id 0 or -1: no hub claim), begins the level
// transition in game mode 1, the workshop race (BeginLevelTransition(level, 1, map JSON, title, data)).
bool EnterLocal(const std::string& path, Obj data) {
    Obj instance = GameInstance();
    Obj enter = instance ? eng::FindFunction(eng::ClassOf(instance), "TryEnterLevelByString") : nullptr;
    if (!enter) {
        gOpenState = "error: the game can't enter levels here";
        return false;
    }
    eng::Params p(enter);
    std::wstring keepPath, keepOverride;
    eng::FString fpath{}, override{};
    MakeFString(path, &keepPath, &fpath);
    MakeFString("Map_LevelEditorMain", &keepOverride, &override);
    p.Set("LevelPathOrName", fpath);
    p.Set("LevelData", data);
    p.Set("OverrideLevelName", override);
    if (!eng::Invoke(instance, p)) {
        gOpenState = "error: the game didn't open the map";
        return false;
    }
    const uint8_t* entered = p.Get("bSuccessful");
    const uint8_t* message = p.Get("Message");
    const std::string said = message ? FString(message) : "";
    hostlog::Info("tracks: opening local map " + path + " as a race: " + (entered && *entered ? "ok" : "refused") +
                  (said.empty() ? "" : " - " + said));
    if (!entered || !*entered) {
        gOpenState = "error: " + (said.empty() ? std::string("the game refused to open it") : said);
        return false;
    }
    gOpenState = "open";
    return true;
}

bool SetActiveList(Obj list, const char* why) {
    Obj instance = GameInstance();
    Obj fn = instance ? eng::FindFunction(eng::ClassOf(instance), "SetActiveLevelList") : nullptr;
    if (!fn) return false;
    eng::Params p(fn);
    std::wstring keep;
    eng::FString message{};
    MakeFString(why, &keep, &message);
    p.Set("?NewList", list);
    p.Set("DebugMessage", message);
    return eng::Invoke(instance, p);
}
}  // namespace

std::string LocalMapsFolder() { return CustomMapDirectory(); }

long long PublishedIdOfFile(const std::string& path) { return PublishedIdOf(eng::Widen(path)); }

long long EditorMapPublishedId(const std::string& mapName) {
    std::string dir = CustomMapDirectory();
    if (dir.empty() || mapName.empty() || mapName.find_first_of("/\\*?") != std::string::npos) return -2;
    if (dir.back() != '/' && dir.back() != '\\') dir += "/";
    const std::wstring pattern = eng::Widen(dir + "Map_LevelEditorMain&" + mapName + "_*.balledit");
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(pattern.c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return -2;
    long long result = -3;
    do {
        const long long id = PublishedIdOf(eng::Widen(dir) + found.cFileName);
        result = result == -3 ? id : (result == id ? id : -2);
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return result == -3 ? -2 : result;
}

static bool EditorMapName(const std::wstring& file) {
    return file.rfind(L"Map_LevelEditorMain&", 0) == 0 && file.size() >= 30 &&
           file.compare(file.size() - 9, 9, L".balledit") == 0;
}

static int InstallLocalMapTo(const std::wstring& source, const std::wstring& file);

// A map file shipped in a plugin's folder, copied into the player's own maps folder (the game opens local maps only
// from there): when it isn't there yet, or the plugin's copy is newer than the one there. A copy the player edited
// since (newer than the plugin's) is kept. Only an editor map file ("Map_LevelEditorMain&<name>_<type>.balledit")
// that was never published. 1 copied, 0 already there (up to date, or the player's newer one), -1 not done
// (OpenState says why; the game's level manager may not be up yet: try again later).
int InstallLocalMap(const std::wstring& source) {
    const size_t slash = source.find_last_of(L"/\\");
    const std::wstring file = slash == std::wstring::npos ? source : source.substr(slash + 1);
    if (!EditorMapName(file)) {
        gOpenState = "error: not an editor map file (Map_LevelEditorMain&<name>_<type>.balledit)";
        return -1;
    }
    return InstallLocalMapTo(source, file);
}

// The same for a plugin file with any name ending in .balledit (a registry plugin's files have plain names only),
// installed as target: an editor map file name, no folders.
int InstallLocalMapAs(const std::wstring& source, const std::wstring& target) {
    if (source.size() < 9 || source.compare(source.size() - 9, 9, L".balledit") != 0) {
        gOpenState = "error: not a map file (.balledit)";
        return -1;
    }
    if (!EditorMapName(target) || target.size() > 200 || target.find(L"..") != std::wstring::npos ||
        target.find_first_of(L"/\\:*?\"<>|") != std::wstring::npos ||
        std::any_of(target.begin(), target.end(), [](wchar_t c) { return c < 32; })) {
        gOpenState = "error: the target isn't an editor map file name (Map_LevelEditorMain&<name>_<type>.balledit)";
        return -1;
    }
    return InstallLocalMapTo(source, target);
}

static int InstallLocalMapTo(const std::wstring& source, const std::wstring& file) {
    const long long published = PublishedIdOf(source);
    if (published == -2) {
        gOpenState = "error: the plugin's map file can't be read or doesn't say it was never published (levelPublishedFileId)";
        return -1;
    }
    if (published > 0) {
        gOpenState = "error: a published map isn't installed as a local map";
        return -1;
    }
    std::string dir = CustomMapDirectory();
    if (dir.empty()) {
        gOpenState = "error: the game's level manager isn't there yet";
        return -1;
    }
    if (dir.back() != '/' && dir.back() != '\\') dir += "/";
    const std::wstring target = eng::Widen(dir) + file;
    WIN32_FILE_ATTRIBUTE_DATA from{}, to{};
    if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &from)) {
        gOpenState = "error: the plugin's map file isn't there";
        return -1;
    }
    if (GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &to) &&
        CompareFileTime(&from.ftLastWriteTime, &to.ftLastWriteTime) <= 0)
        return 0;
    CreateDirectoryW(eng::Widen(dir).c_str(), nullptr);
    if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {          // (keeps the plugin copy's time: no repeat)
        gOpenState = "error: copying into the maps folder failed (" + std::to_string(GetLastError()) + ")";
        return -1;
    }
    hostlog::Info("tracks: map installed into the player's maps folder: " + eng::Narrow(file.c_str(), static_cast<int>(file.size())));
    return 1;
}

bool OpenLocal(const std::string& name) {
    const std::string path = LocalPath(name);
    if (path.empty()) return false;
    Obj data = LocalLevelData(path);
    return data && EnterLocal(path, data);
}

// The maps as the active playlist (a PDA_LevelList_C of their display names, as the game makes its own lists), so the
// game's own "next track" goes through them in order (TryGoToNextLevel: the next name of ActiveLevelPlaylist, its
// level data from AllLevelAssets, entered with TryEnterLevelByString). The list the game had is put back when the
// main menu is up again (LocalFrame), so its own playlists (the circuit's track tiles enter by index in the active
// list) are never left pointing at these maps.
bool OpenLocalList(const std::vector<std::string>& names, int index) {
    if (index < 0 || index >= static_cast<int>(names.size())) {
        gOpenState = "error: no such map in the list";
        return false;
    }
    std::vector<std::string> paths;
    std::vector<Obj> datas;
    for (const auto& n : names) {
        const std::string path = LocalPath(n);
        Obj data = path.empty() ? nullptr : LocalLevelData(path);
        if (!data) return false;
        paths.push_back(path);
        datas.push_back(data);
    }
    Obj instance = GameInstance();
    Obj cls = eng::FindClass("PDA_LevelList_C");
    Obj list = cls && instance ? eng::Call(eng::FindCdo("GameplayStatics"), "SpawnObject", cls, instance).ReturnObj() : nullptr;
    Obj addFn = list ? eng::FindFunction(cls, "AddLevelToPlaylist") : nullptr;
    if (addFn) {
        for (const auto& path : paths) {
            eng::Params p(addFn);
            std::wstring keep;
            eng::FString level{};
            MakeFString(DisplayName(path), &keep, &level);
            p.Set("LevelName", level);
            p.Set("LevelIndexOrAppendType", int32_t{-1});
            eng::Invoke(list, p);
        }
        eng::Call(list, "SetListAsUGCList");
        Obj current = eng::ReadObj(instance, "ActiveLevelPlaylist");
        if (!gCupActive || current != eng::Get(gCupList)) gCupPrevious = eng::MakeWeak(current);
        if (SetActiveList(list, "plugin list of local maps")) {
            gCupList = eng::MakeWeak(list);
            gCupActive = true;
            gCupLeftMenu = false;
            hostlog::Info("tracks: " + std::to_string(datas.size()) + " local maps are the active playlist");
        }
    } else {
        hostlog::Warn("tracks: couldn't make a playlist of the local maps; opening the map alone");
    }
    return EnterLocal(paths[static_cast<size_t>(index)], datas[static_cast<size_t>(index)]);
}

void LocalFrame() {
    if (!gCupActive) return;
    Obj controller = game::PlayerController();
    Obj world = controller ? eng::OuterOf(eng::OuterOf(controller)) : nullptr;
    if (!world) return;
    if (eng::ObjName(world) != "Map_MainMenu") {
        gCupLeftMenu = true;
        return;
    }
    if (!gCupLeftMenu) return;                                  // still on the way out to the first map
    Obj instance = GameInstance();
    Obj current = instance ? eng::ReadObj(instance, "ActiveLevelPlaylist") : nullptr;
    gCupActive = false;
    if (!current || current != eng::Get(gCupList)) return;      // the game has chosen another list itself
    Obj previous = eng::Get(gCupPrevious);
    if (previous && SetActiveList(previous, "the list before the plugin's local maps"))
        hostlog::Info("tracks: the game's playlist is active again");
}

}  // namespace tracks
