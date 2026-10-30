#include "prefabs.hpp"

#include <windows.h>

#include <algorithm>

#include "editor.hpp"
#include "engine.hpp"
#include "files.hpp"
#include "game.hpp"
#include "json.hpp"

namespace prefabs {
namespace {

using eng::Obj;

eng::Weak gConsole;

// The game's console: one BallestCommandConsoleSubsystem per local player (measured: one, under the engine's
// LocalPlayer). Kept while it lives.
Obj Console() {
    if (Obj c = eng::Get(gConsole)) return c;
    Obj cls = eng::FindClass("BallestCommandConsoleSubsystem"), found = nullptr;
    if (!cls) return nullptr;
    eng::ForEachObject([&](Obj o) {
        if (!eng::IsA(o, cls) || eng::IsDefaultObject(o)) return true;
        found = o;
        return false;
    });
    gConsole = eng::MakeWeak(found);
    return found;
}

std::string FStringReturn(const eng::Params& p) {
    const uint8_t* r = p.Return();
    return r ? eng::ReadFString(r) : "";
}

// A path from the engine's KismetSystemLibrary, as text.
std::string LibraryPath(const char* function, const std::wstring* argument = nullptr) {
    Obj lib = eng::FindCdo("KismetSystemLibrary");
    eng::Params p(eng::FunctionOn(lib, function));
    if (argument)
        p.SetArg(0, eng::FString{argument->c_str(), static_cast<int32_t>(argument->size() + 1),
                                 static_cast<int32_t>(argument->size() + 1)});
    if (!eng::Invoke(lib, p)) return "";
    return FStringReturn(p);
}

std::wstring PathOf(const std::string& name) { return eng::Widen(Folder() + name + ".json"); }

}  // namespace

std::string Folder() {
    std::string saved = LibraryPath("GetProjectSavedDirectory");
    if (saved.empty()) return "";
    const std::wstring relative = eng::Widen(saved);
    std::string full = LibraryPath("ConvertToAbsolutePath", &relative);
    if (full.empty()) full = saved;
    std::replace(full.begin(), full.end(), '/', '\\');
    if (full.back() != '\\') full += '\\';
    return full + "SaveGames\\Prefabs\\";
}

bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

std::vector<std::string> List() {
    std::vector<std::string> out;
    const std::string folder = Folder();
    if (folder.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(eng::Widen(folder + "*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring file = fd.cFileName;
        const std::string name = eng::Narrow(file.c_str(), static_cast<int>(file.size() - 5));    // without .json
        if (ValidName(name)) out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::string Read(const std::string& name) {
    if (!ValidName(name)) return "";
    HANDLE h = CreateFileW(PathOf(name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && static_cast<unsigned long long>(size.QuadPart) <= kMaxBytes) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) || got != text.size()) text.clear();
    }
    CloseHandle(h);
    return text;
}

std::string Write(const std::string& name, const std::string& text) {
    if (!ValidName(name)) return "not a prefab name (1 to 64 letters, digits, _ and -)";
    if (text.size() > kMaxBytes) return "over 4 MB";
    json::Value v;
    std::string error;
    if (!json::Parse(text, v, error) || v.type != json::Value::Object) return "not JSON" + (error.empty() ? "" : ": " + error);
    const json::Value* version = v.Get("version");
    if (v.Str("format") != "BallestPrefab" || !version || version->type != json::Value::Number || version->number != 1)
        return "not a BallestPrefab version 1 file";
    const json::Value* items = v.Get("items");
    if (!items || items->type != json::Value::Array || items->items.size() < 2 || items->items.size() > 4096)
        return "a prefab has 2 to 4096 pieces";
    const std::string folder = Folder();
    if (folder.empty()) return "the game can't say where it keeps prefabs yet";
    CreateDirectoryW(eng::Widen(folder).c_str(), nullptr);
    return files::WriteWhole(PathOf(name), text) ? "" : "couldn't write the file";
}

Reply RunLine(const std::string& line) {
    Obj console = Console();
    if (!console) return {false, "the game's console isn't there"};
    const std::wstring wide = eng::Widen(line);
    eng::Params p(eng::FunctionOn(console, "ExecuteCommand"));
    p.SetArg(0, eng::FString{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)});
    if (!eng::Invoke(console, p)) return {false, "the game's console didn't take it"};
    return {true, FStringReturn(p)};
}

Reply SaveSelection(const std::string& name, bool overwrite, bool* saved) {
    if (saved) *saved = false;
    if (!ValidName(name)) return {false, "not a prefab name (1 to 64 letters, digits, _ and -)"};
    if (!editor::Open()) return {false, "not in the track editor"};
    if (editor::Selection().empty()) return {false, "nothing is selected"};
    Reply r = RunLine("ballest.editor.saveprefab " + name);
    // measured: "Prefab <name> already exists. To confirm overwriting it with this captured selection, ... type:
    // ballest.editor.saveprefab <name> confirm"
    if (r.ran && overwrite && r.text.find("already exists") != std::string::npos)
        r = RunLine("ballest.editor.saveprefab " + name + " confirm");
    if (saved) *saved = r.ran && r.text.rfind("Saved prefab", 0) == 0;      // measured: "Saved prefab <name> to <path>."
    return r;
}

Reply Spawn(const std::string& name, std::vector<int>* made) {
    if (made) made->clear();
    if (!ValidName(name)) return {false, "not a prefab name (1 to 64 letters, digits, _ and -)"};
    if (!editor::Open()) return {false, "not in the track editor"};
    const std::vector<int> before = editor::Pieces();
    Reply r = RunLine("ballest.editor.spawnprefab " + name);
    if (r.ran && made)
        for (int id : editor::Pieces())
            if (std::find(before.begin(), before.end(), id) == before.end()) made->push_back(id);
    return r;
}

Reply Delete(const std::string& name) {
    if (!ValidName(name)) return {false, "not a prefab name (1 to 64 letters, digits, _ and -)"};
    return RunLine("ballest.editor.deleteprefab " + name);
}

Reply ListReply() { return RunLine("ballest.editor.listprefabs"); }

}  // namespace prefabs
