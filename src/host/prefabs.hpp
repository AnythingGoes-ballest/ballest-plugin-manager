// The game's prefabs: groups of editor pieces saved as files in its Saved\SaveGames\Prefabs folder, made, placed,
// listed and deleted by its own console commands (ballest.editor.saveprefab, spawnprefab, listprefabs, deleteprefab;
// EBallestConsoleCommand). Those commands are run here through the game's own console, BallestCommandConsoleSubsystem
// (a LocalPlayerSubsystem) and its ExecuteCommand(Line) -> reply, the way the console box runs a line the player typed,
// so a plugin gets exactly what the player would: the game's own file format, grouping and placement. Plugins never
// pass a command line: each function builds its one command from a checked prefab name.
// Game thread only.
#pragma once
#include <string>
#include <vector>

namespace prefabs {
// The folder the game keeps prefabs in (its Saved\SaveGames\Prefabs, under a test copy's own -userdir), with a
// trailing backslash; "" before the game can say where it saves.
std::string Folder();
// A prefab's name as the game takes it: 1 to 64 letters, digits, '_' and '-' (the file is "<name>.json").
bool ValidName(const std::string& name);
// The prefab files in the folder, by name (without .json), sorted.
std::vector<std::string> List();
// A prefab file's text, or "" if there is none (or it is over kMaxBytes).
std::string Read(const std::string& name);
constexpr size_t kMaxBytes = 4 * 1024 * 1024;
// Writes a prefab file from text (a downloaded prefab): only a BallestPrefab version 1 file with an "items" array of
// 2 to 4096 pieces, at most kMaxBytes, under a valid name. Replaces a file of that name. "" when written, else why not.
std::string Write(const std::string& name, const std::string& text);
// The game's own commands, each answering the game's reply (what its console box shows), or why it couldn't run.
struct Reply {
    bool ran = false;                   // the game's console took the command
    std::string text;                   // its reply, or why it didn't run
};
// The editor's selected pieces saved as the prefab `name`. The game asks before replacing a file (its reply says so,
// and "saveprefab <name> confirm" goes ahead): with `overwrite` that answer is given, else the file is left as it was.
// `saved` is whether the game says it saved (its reply starts "Saved prefab").
Reply SaveSelection(const std::string& name, bool overwrite, bool* saved);
Reply Spawn(const std::string& name, std::vector<int>* made);     // placed in the editor; the new pieces' ids
Reply Delete(const std::string& name);
Reply ListReply();                      // the game's listprefabs reply
// Test channel: one line through the game's console, as typed.
Reply RunLine(const std::string& line);
}  // namespace prefabs
