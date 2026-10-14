// Tracks to open from outside the game's menus: the game's own (its circuit, in order) and workshop tracks found by a
// search, opened the way the game opens them (read from BP_BallgameGameInstance):
//   * the game's own: TryEnterLevelByIndex(index in GetCircuitMapNames), which ends in BeginLevelTransition(level, 0)
//   * a workshop track: downloaded by Steam if it isn't installed (DownloadItem, as the game's browser does), made
//     into level data by the level manager (O_LevelManager.AddRuntimeLevelFromSteam), then read with
//     SKGMLEStatics.LoadMapFromFile(directory, map path) and BallestUGCPlaybackLibrary.ResolveUGCPlaybackBaselineFromJson,
//     and opened with BeginLevelTransition(level, 1, map JSON, title) (TryEnterLevelByIndex's workshop branch)
// Game thread only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "steam.hpp"

namespace tracks {

void Frame();

// The game's own tracks, grouped as its playlists group them (read from the game's files): each circuit season
// (the game instance's CircuitSeasons.NumberedSeasons), then DA_CollabLevels_ShippingList ("collabs") and
// DA_TowerLevels_ShippingList ("trials": The Tower and The Tower CPs).
struct OfficialTrack {
    std::string level, group;           // group: "season 1", "season 2", ..., "collabs", "trials"
};
std::vector<OfficialTrack> OfficialTracks();
std::vector<std::string> Official();                    // level names, in the circuit's order (TryEnterLevelByIndex)
std::string Title(const std::string& level);            // its descriptive name ("01" for the circuit's), or ""
std::string Image(const std::string& level);            // its preview picture (a game texture path), or ""
bool OpenOfficial(int index);
bool Open(const std::string& level);                    // as the game's own play button does

// A workshop search: results arrive later (SearchState "searching" until then).
void Search(const std::string& text);
std::string SearchState();                              // "idle", "searching", "done", "error: ..."
const std::vector<steam::Item>& Results();
int Total();
// A result's preview picture on this computer: the game's own cache of them (Saved\HubPreviewCache\<id>_*.img,
// JPEG), or one downloaded here (Saved\PluginManager\cache\previews\<id>.jpg); "" until there is one (asking for it
// starts the download).
std::string ResultImage(size_t index);
// The same for any workshop item, by id and its preview's file handle (0 if unknown: then only cached pictures).
std::string PreviewImage(uint64_t id, uint64_t preview);

// Opens a workshop track by id, downloading it first if needed; OpenState tells how it is going.
bool OpenWorkshop(uint64_t id);
std::string OpenState();                                // "idle", "downloading", "opening", "open", "error: ..."

// A map file of the player's own (a name in the game's UserSavedMaps folder; never published) opened as a normal
// race, as the game opens a workshop map (OpenState tells how it went). LocalMapsFolder: that folder, or "".
bool OpenLocal(const std::string& fileName);
// The same, for map index of a list of such maps that becomes the game's active playlist while they are raced, so
// the game's own "next track" goes to the next one; the game's own list is put back on the main menu.
bool OpenLocalList(const std::vector<std::string>& fileNames, int index);
std::string LocalMapsFolder();
// A map file shipped with a plugin (full path) copied into the player's maps folder when missing or older there:
// 1 copied, 0 already there (or the player's own newer copy, kept), -1 not done (OpenState: why).
int InstallLocalMap(const std::wstring& source);
// levelData.levelPublishedFileId of a map file (full path): > 0 once published, -1 never published, -2 unreadable.
long long PublishedIdOfFile(const std::string& path);
// The player's own map open in the track editor (its file in LocalMapsFolder, "Map_LevelEditorMain&<name>_*.balledit"),
// as that file's published id: -1 never published, > 0 published, -2 no such file (or several that disagree).
long long EditorMapPublishedId(const std::string& mapName);

}  // namespace tracks
