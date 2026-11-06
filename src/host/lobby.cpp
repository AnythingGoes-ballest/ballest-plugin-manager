#include "lobby.hpp"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "log.hpp"
#include "sandbox.hpp"
#include "steam.hpp"

namespace lobby {
namespace {

std::string gState = "none";
int gGeneration = 0;                    // Create, Join and Leave bump it: a Steam answer for an older request is dropped
std::mutex gCallbackLock;               // what Steam's callbacks touch (they may run on the online subsystem's thread)
int gType = 0;                          // the type this game's own lobby was made with
std::string gId;                        // the lobby this game is in
bool gOwner = false;
std::string gPendingInvite;
bool gCommandLineRead = false;
bool gSearching = false;
std::vector<Found> gResults;
std::string gSearchCode;                // the last Find's code ("" for the lists)

bool UseTest() { return sandbox::On(); }

// --- Steam ------------------------------------------------------------------------------------------------------------
// Callback ids and sizes (Steamworks SDK, 8-byte packing on Windows): LobbyEnter_t 504 (lobby 0, permissions 8, locked
// 12, response 16: 24 bytes), LobbyMatchList_t 510 (count: 4), LobbyCreated_t 513 (EResult 0, lobby 8: 16),
// GameLobbyJoinRequested_t 333 (lobby 0, friend 8: 16), FriendGameInfo_t (game id 0, ip 8, ports 12/14, lobby 16: 24).
constexpr int kLobbyEnter = 504, kLobbyEnterSize = 24, kLobbyMatchList = 510, kLobbyMatchListSize = 4, kLobbyCreated = 513,
              kLobbyCreatedSize = 16, kGameLobbyJoinRequested = 333;
constexpr int kEnterSuccess = 1, kResultOk = 1, kFriendFlagImmediate = 4, kDistanceWorldwide = 3, kComparisonEqual = 0;

struct Steam {
    void* mm = nullptr;
    void* friends = nullptr;
    uint32_t appId = 0;
    uint64_t (*createLobby)(void*, int, int) = nullptr;
    uint64_t (*joinLobby)(void*, uint64_t) = nullptr;
    void (*leaveLobby)(void*, uint64_t) = nullptr;
    uint64_t (*requestList)(void*) = nullptr;
    void (*stringFilter)(void*, const char*, const char*, int) = nullptr;
    void (*distanceFilter)(void*, int) = nullptr;
    void (*countFilter)(void*, int) = nullptr;
    uint64_t (*lobbyByIndex)(void*, int) = nullptr;
    const char* (*getData)(void*, uint64_t, const char*) = nullptr;
    bool (*setData)(void*, uint64_t, const char*, const char*) = nullptr;
    int (*dataCount)(void*, uint64_t) = nullptr;
    bool (*dataByIndex)(void*, uint64_t, int, char*, int, char*, int) = nullptr;
    int (*memberCount)(void*, uint64_t) = nullptr;
    uint64_t (*memberByIndex)(void*, uint64_t, int) = nullptr;
    uint64_t (*owner)(void*, uint64_t) = nullptr;
    int (*memberLimit)(void*, uint64_t) = nullptr;
    bool (*setType)(void*, uint64_t, int) = nullptr;
    bool (*setJoinable)(void*, uint64_t, bool) = nullptr;
    bool (*requestData)(void*, uint64_t) = nullptr;
    bool (*sendChat)(void*, uint64_t, const void*, int) = nullptr;
    int (*chatEntry)(void*, uint64_t, int, uint64_t*, void*, int, int*) = nullptr;
    int (*friendCount)(void*, int) = nullptr;
    uint64_t (*friendByIndex)(void*, int, int) = nullptr;
    bool (*friendGame)(void*, uint64_t, void*) = nullptr;
    void (*inviteDialog)(void*, uint64_t) = nullptr;
    bool ok = false;
};

Steam& S() {
    static Steam s;
    static bool tried = false;
    if (tried) return s;
    tried = true;
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    if (!dll) return s;
    bool all = true;
    auto get = [&](const char* name) {
        void* p = reinterpret_cast<void*>(GetProcAddress(dll, name));
        if (!p) {
            hostlog::Warn(std::string("lobby: steam_api64.dll has no ") + name);
            all = false;
        }
        return p;
    };
    auto open = [&](const char* name) {
        auto accessor = reinterpret_cast<void* (*)()>(get(name));
        return accessor ? accessor() : nullptr;
    };
#define BIND(field, name) s.field = reinterpret_cast<decltype(s.field)>(get("SteamAPI_ISteamMatchmaking_" name))
    s.mm = open("SteamAPI_SteamMatchmaking_v009");
    BIND(createLobby, "CreateLobby");
    BIND(joinLobby, "JoinLobby");
    BIND(leaveLobby, "LeaveLobby");
    BIND(requestList, "RequestLobbyList");
    BIND(stringFilter, "AddRequestLobbyListStringFilter");
    BIND(distanceFilter, "AddRequestLobbyListDistanceFilter");
    BIND(countFilter, "AddRequestLobbyListResultCountFilter");
    BIND(lobbyByIndex, "GetLobbyByIndex");
    BIND(getData, "GetLobbyData");
    BIND(setData, "SetLobbyData");
    BIND(dataCount, "GetLobbyDataCount");
    BIND(dataByIndex, "GetLobbyDataByIndex");
    BIND(memberCount, "GetNumLobbyMembers");
    BIND(memberByIndex, "GetLobbyMemberByIndex");
    BIND(owner, "GetLobbyOwner");
    BIND(memberLimit, "GetLobbyMemberLimit");
    BIND(setType, "SetLobbyType");
    BIND(setJoinable, "SetLobbyJoinable");
    BIND(requestData, "RequestLobbyData");
    BIND(sendChat, "SendLobbyChatMsg");
    BIND(chatEntry, "GetLobbyChatEntry");
#undef BIND
    s.friends = steam::FriendsInterface();
    s.friendCount = reinterpret_cast<decltype(s.friendCount)>(get("SteamAPI_ISteamFriends_GetFriendCount"));
    s.friendByIndex = reinterpret_cast<decltype(s.friendByIndex)>(get("SteamAPI_ISteamFriends_GetFriendByIndex"));
    s.friendGame = reinterpret_cast<decltype(s.friendGame)>(get("SteamAPI_ISteamFriends_GetFriendGamePlayed"));
    s.inviteDialog = reinterpret_cast<decltype(s.inviteDialog)>(get("SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog"));
    void* utils = open("SteamAPI_SteamUtils_v010");
    auto appId = reinterpret_cast<uint32_t (*)(void*)>(get("SteamAPI_ISteamUtils_GetAppID"));
    if (utils && appId) s.appId = appId(utils);
    s.ok = all && s.mm && s.friends && s.appId;
    return s;
}

uint64_t Parse64(const std::string& id) { return std::strtoull(id.c_str(), nullptr, 10); }

std::map<std::string, std::string> SteamData(uint64_t lobby) {
    std::map<std::string, std::string> out;
    Steam& s = S();
    const int n = s.dataCount(s.mm, lobby);
    for (int i = 0; i < n && i < 64; ++i) {
        char key[256] = {}, value[8192] = {};
        if (s.dataByIndex(s.mm, lobby, i, key, sizeof key, value, sizeof value)) out[key] = value;
    }
    return out;
}

// GameLobbyJoinRequested_t: the player accepted an invite (or chose to join a friend's lobby) while the game runs.
struct CallbackObject {
    void* const* table;
    uint8_t flags;
    int32_t callback;
};
void JoinRequested(void*, void* param) {
    if (!param) return;
    uint64_t lobbyId = 0;
    std::memcpy(&lobbyId, param, sizeof lobbyId);
    std::lock_guard<std::mutex> lock(gCallbackLock);
    if (lobbyId) gPendingInvite = std::to_string(lobbyId);
}
void JoinRequestedWithCall(void* self, void* param, bool, uint64_t) { JoinRequested(self, param); }
int JoinRequestedSize(void*) { return 16; }
void* const gJoinTable[] = {reinterpret_cast<void*>(&JoinRequestedWithCall), reinterpret_cast<void*>(&JoinRequested),
                            reinterpret_cast<void*>(&JoinRequestedSize)};
CallbackObject gJoinCallback{gJoinTable, 0, 0};
bool gJoinRegistered = false;

// LobbyChatMsg_t (507): lobby 0, user 8, chat entry type 16 (uint8), chat id 20. The message itself is read with
// GetLobbyChatEntry; only k_EChatEntryTypeChatMsg (1) entries are messages.
constexpr int kLobbyChatMsg = 507, kChatEntryMessage = 1;
std::vector<Message> gInbox;
void ChatArrived(void*, void* param) {
    if (!param || !S().ok) return;
    uint64_t lobbyId = 0;
    int32_t chatId = 0;
    std::memcpy(&lobbyId, param, 8);
    std::memcpy(&chatId, static_cast<const uint8_t*>(param) + 20, 4);
    std::lock_guard<std::mutex> lock(gCallbackLock);
    if (std::to_string(lobbyId) != gId) return;
    uint64_t user = 0;
    int type = 0;
    char data[1025] = {};
    const int n = S().chatEntry(S().mm, lobbyId, chatId, &user, data, 1024, &type);
    if (n <= 0 || type != kChatEntryMessage || gInbox.size() > 256) return;
    gInbox.push_back({std::to_string(user), std::string(data, static_cast<size_t>(n))});
}
void ChatArrivedWithCall(void* self, void* param, bool, uint64_t) { ChatArrived(self, param); }
int ChatSize(void*) { return 24; }
void* const gChatTable[] = {reinterpret_cast<void*>(&ChatArrivedWithCall), reinterpret_cast<void*>(&ChatArrived),
                            reinterpret_cast<void*>(&ChatSize)};
CallbackObject gChatCallback{gChatTable, 0, 0};

void RegisterInvites() {
    if (gJoinRegistered || UseTest() || !S().ok) return;
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    using RegisterFn = void (*)(void*, int);
    auto reg = dll ? reinterpret_cast<RegisterFn>(reinterpret_cast<void*>(GetProcAddress(dll, "SteamAPI_RegisterCallback"))) : nullptr;
    if (!reg) return;
    reg(&gJoinCallback, kGameLobbyJoinRequested);
    reg(&gChatCallback, kLobbyChatMsg);
    gJoinRegistered = true;
}

// --- the test copies' shared folder -------------------------------------------------------------------------------------
constexpr ULONGLONG kStaleMs = 20000, kBeatMs = 1000;

std::wstring TestDir() {
    static std::wstring dir;
    if (!dir.empty()) return dir;
    wchar_t base[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, base))) return L"";
    dir = std::wstring(base) + L"\\BallestTogetherTest";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
std::string OwnTestId() { return "t" + std::to_string(GetCurrentProcessId()); }

std::string ReadText(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string text;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0 && text.size() < 256 * 1024) text.append(buf, n);
    CloseHandle(h);
    return text;
}
bool WriteText(const std::wstring& path, const std::string& text) {
    const std::wstring temp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr) && n == text.size();
    CloseHandle(h);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
// How long ago a file was written, in ms (max if it isn't there).
ULONGLONG AgeMs(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return ~0ull;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG t = (static_cast<ULONGLONG>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    const ULONGLONG n = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return n > t ? (n - t) / 10000 : 0;
}
std::map<std::string, std::string> ParseLines(const std::string& text) {
    std::map<std::string, std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        const size_t eq = line.find('=');
        if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
        at = end + 1;
    }
    return out;
}
bool Digits(const std::string& id) { return !id.empty() && id.size() < 24 && id.find_first_not_of("0123456789") == std::string::npos; }
std::wstring LobbyFile(const std::string& id) { return TestDir() + L"\\" + std::wstring(id.begin(), id.end()) + L".lobby"; }
std::wstring MemberFile(const std::string& id, const std::string& member) {
    return TestDir() + L"\\" + std::wstring(id.begin(), id.end()) + L".m." + std::wstring(member.begin(), member.end());
}
std::wstring ChatFile(const std::string& id) { return TestDir() + L"\\" + std::wstring(id.begin(), id.end()) + L".chat"; }
long long gChatRead = 0;                // how far this copy has read its lobby's chat file
std::map<std::string, std::string> gTestData;   // the owner's copy of its lobby
ULONGLONG gLastBeat = 0;

bool WriteTestLobby() {
    std::string text;
    for (const auto& [k, v] : gTestData) {
        std::string value = v;
        std::replace(value.begin(), value.end(), '\n', ' ');
        text += k + "=" + value + "\n";
    }
    return WriteText(LobbyFile(gId), text);
}
std::map<std::string, std::string> TestLobby(const std::string& id, bool* fresh) {
    const ULONGLONG age = AgeMs(LobbyFile(id));
    if (fresh) *fresh = age < kStaleMs;
    return age == ~0ull ? std::map<std::string, std::string>{} : ParseLines(ReadText(LobbyFile(id)));
}
std::vector<Member> TestMembers(const std::string& id) {
    std::vector<Member> out;
    const auto data = TestLobby(id, nullptr);
    auto owner = data.find("_owner"), ownerName = data.find("_ownername");
    if (owner != data.end()) out.push_back({owner->second, ownerName != data.end() ? ownerName->second : owner->second, true});
    WIN32_FIND_DATAW f;
    const std::wstring pattern = TestDir() + L"\\" + std::wstring(id.begin(), id.end()) + L".m.*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring name = f.cFileName;
        const std::wstring path = TestDir() + L"\\" + name;
        if (name.find(L".tmp") != std::wstring::npos || AgeMs(path) >= kStaleMs) continue;
        const size_t dot = name.rfind(L'.');
        const std::string member(name.begin() + static_cast<std::ptrdiff_t>(dot + 1), name.end());
        out.push_back({member, ReadText(path), false});
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return out;
}
// Member and chat files left by a test copy that was closed without leaving (its lobby long gone), deleted.
void CleanOrphans() {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((TestDir() + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = f.cFileName;
        const size_t dot = name.find(L'.');
        if (dot == std::wstring::npos || name.find(L".lobby") != std::wstring::npos) continue;
        const std::string id(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(dot));
        const std::wstring path = TestDir() + L"\\" + name;
        if (AgeMs(LobbyFile(id)) == ~0ull && AgeMs(path) > 60000) DeleteFileW(path.c_str());
    } while (FindNextFileW(h, &f));
    FindClose(h);
}
std::vector<std::string> TestLobbyIds() {
    std::vector<std::string> ids;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW((TestDir() + L"\\*.lobby").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return ids;
    do {
        std::wstring name = f.cFileName;
        name.resize(name.size() - 6);
        const std::string id(name.begin(), name.end());
        if (AgeMs(LobbyFile(id)) < kStaleMs) ids.push_back(id);
        else if (AgeMs(LobbyFile(id)) > 60000) DeleteFileW(LobbyFile(id).c_str());     // long gone
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return ids;
}

std::string SlotSuffix() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    const std::wstring v(buf, n);
    const size_t at = v.find(L"BallestTest");
    return at == std::wstring::npos ? "" : " (slot " + std::string(v.begin() + static_cast<std::ptrdiff_t>(at + 11), v.end()) + ")";
}

}  // namespace

std::string Backend() { return UseTest() ? "test" : S().ok ? "steam" : "none"; }

std::string OwnName() {
    std::string name = steam::PersonaName(steam::OwnSteamId());
    if (name.empty()) name = "player";
    return UseTest() ? name + SlotSuffix() : name;
}

void Frame() {
    if (!gCommandLineRead) {
        gCommandLineRead = true;
        // Steam starts the game with "+connect_lobby <id>" for an invite accepted while it wasn't running.
        const std::string line = GetCommandLineA();
        const size_t at = line.find("+connect_lobby");
        if (at != std::string::npos) {
            const std::string id = std::to_string(std::strtoull(line.c_str() + at + 14, nullptr, 10));
            if (id != "0") gPendingInvite = id;
        }
    }
    RegisterInvites();
    if (!UseTest() || gId.empty() || gState != "in lobby") return;
    if (GetTickCount64() - gLastBeat < kBeatMs) return;
    gLastBeat = GetTickCount64();
    if (gOwner) {
        WriteTestLobby();
    } else {
        bool fresh = false;
        TestLobby(gId, &fresh);
        if (!fresh) {
            DeleteFileW(MemberFile(gId, OwnTestId()).c_str());
            gId.clear();
            gState = "error: the lobby closed";
            return;
        }
        WriteText(MemberFile(gId, OwnTestId()), OwnName());
    }
}

std::string State() { return gState; }
std::string Id() { return gId; }
bool IsOwner() { return !gId.empty() && gOwner; }

void Leave() {
    ++gGeneration;                      // an answer still on its way is for a request that's over
    if (!gId.empty()) {
        if (UseTest()) {
            if (gOwner) {
                DeleteFileW(LobbyFile(gId).c_str());
                DeleteFileW(ChatFile(gId).c_str());
            } else {
                DeleteFileW(MemberFile(gId, OwnTestId()).c_str());
            }
        } else if (S().ok) {
            const uint64_t id = Parse64(gId);
            if (gOwner) {
                // Steam gives the lobby to another member when its owner leaves: unlisted and emptied first, so it is
                // never found (or joined) after the host has gone
                S().setJoinable(S().mm, id, false);
                for (const char* key : {"ct", "ch", "map", "host", "ver", "vis"}) S().setData(S().mm, id, key, "");
            }
            S().leaveLobby(S().mm, id);
        }
        hostlog::Info("lobby: left " + gId);
    }
    std::lock_guard<std::mutex> lock(gCallbackLock);
    gId.clear();
    gOwner = false;
    gTestData.clear();
    gInbox.clear();
    gChatRead = 0;
    gState = "none";
}

void Create(int type, int maxMembers) {
    Leave();
    type = std::clamp(type, 0, 3);
    maxMembers = std::clamp(maxMembers, 2, 16);
    gType = type;
    const int generation = gGeneration;
    if (UseTest()) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        gId = std::to_string(9000000000ull + (static_cast<uint64_t>(t.QuadPart) ^ GetCurrentProcessId()) % 999999999ull);
        gOwner = true;
        gTestData = {{"_owner", OwnTestId()}, {"_ownername", OwnName()}, {"_type", std::to_string(type)},
                     {"_max", std::to_string(maxMembers)}, {"_joinable", "1"}};
        gState = WriteTestLobby() ? "in lobby" : "error: the test lobby folder can't be written";
        hostlog::Info("lobby: test lobby " + gId + " made");
        return;
    }
    Steam& s = S();
    if (!s.ok) {
        gState = "error: Steam lobbies are not available";
        return;
    }
    gState = "creating";
    steam::Await(s.createLobby(s.mm, type, maxMembers), kLobbyCreated, kLobbyCreatedSize, [generation](const uint8_t* r, bool failed) {
        int32_t result = 0;
        uint64_t id = 0;
        if (r) std::memcpy(&result, r, 4), std::memcpy(&id, r + 8, 8);
        if (generation != gGeneration) {            // left (or something else started) while Steam made it
            if (id) S().leaveLobby(S().mm, id);
            return;
        }
        if (failed || result != kResultOk || !id) {
            gState = "error: Steam couldn't make the lobby (" + std::to_string(result) + ")";
            return;
        }
        gId = std::to_string(id);
        gOwner = true;
        gState = "in lobby";
        hostlog::Info("lobby: Steam lobby " + gId + " made");
    });
}

void Join(const std::string& id) {
    Leave();
    if (id.empty()) return;
    if (!Digits(id)) {
        gState = "error: not a lobby";
        return;
    }
    const int generation = gGeneration;
    if (UseTest()) {
        bool fresh = false;
        const auto data = TestLobby(id, &fresh);
        if (!fresh) {
            gState = "error: no such lobby";
            return;
        }
        const int max = std::atoi(data.count("_max") ? data.at("_max").c_str() : "8");
        if (data.count("_joinable") && data.at("_joinable") == "0") {
            gState = "error: the lobby is locked";
            return;
        }
        if (static_cast<int>(TestMembers(id).size()) >= max) {
            gState = "error: the lobby is full";
            return;
        }
        gId = id;
        gOwner = false;
        WriteText(MemberFile(gId, OwnTestId()), OwnName());
        gState = "in lobby";
        hostlog::Info("lobby: joined test lobby " + gId);
        return;
    }
    Steam& s = S();
    if (!s.ok) {
        gState = "error: Steam lobbies are not available";
        return;
    }
    gState = "joining";
    const uint64_t wanted = Parse64(id);
    steam::Await(s.joinLobby(s.mm, wanted), kLobbyEnter, kLobbyEnterSize, [wanted, generation](const uint8_t* r, bool failed) {
        uint32_t response = 0;
        if (r) std::memcpy(&response, r + 16, 4);
        if (generation != gGeneration) {
            if (r && response == kEnterSuccess) S().leaveLobby(S().mm, wanted);
            return;
        }
        if (failed || response != kEnterSuccess) {
            gState = "error: couldn't join (Steam answered " + std::to_string(response) + ")";
            return;
        }
        gId = std::to_string(wanted);
        gOwner = S().owner(S().mm, wanted) == steam::OwnSteamId();
        gState = "in lobby";
        hostlog::Info("lobby: joined Steam lobby " + gId);
    });
}

bool SetData(const std::string& key, const std::string& value) {
    if (!IsOwner() || key.empty() || key[0] == '_' || key.find_first_of("=\r\n") != std::string::npos ||
        value.find_first_of("\r\n") != std::string::npos || key.size() > 255 || value.size() > 8000)
        return false;
    if (UseTest()) {
        gTestData[key] = value;
        return WriteTestLobby();
    }
    return S().ok && S().setData(S().mm, Parse64(gId), key.c_str(), value.c_str());
}

std::string Data(const std::string& key) {
    if (gId.empty()) return "";
    if (UseTest()) {
        const auto data = gOwner ? gTestData : TestLobby(gId, nullptr);
        auto it = data.find(key);
        return it == data.end() ? "" : it->second;
    }
    if (!S().ok) return "";
    const char* v = S().getData(S().mm, Parse64(gId), key.c_str());
    return v ? v : "";
}

bool SetType(int type) {
    if (!IsOwner()) return false;
    type = std::clamp(type, 0, 3);
    if (UseTest()) {
        gTestData["_type"] = std::to_string(type);
        return WriteTestLobby();
    }
    return S().ok && S().setType(S().mm, Parse64(gId), type);
}

bool SetJoinable(bool joinable) {
    if (!IsOwner()) return false;
    if (UseTest()) {
        gTestData["_joinable"] = joinable ? "1" : "0";
        return WriteTestLobby();
    }
    return S().ok && S().setJoinable(S().mm, Parse64(gId), joinable);
}

int MaxMembers() {
    if (gId.empty()) return 0;
    if (UseTest()) return std::atoi(Data("_max").empty() ? (gOwner ? gTestData["_max"].c_str() : "0") : Data("_max").c_str());
    return S().ok ? S().memberLimit(S().mm, Parse64(gId)) : 0;
}

std::vector<Member> Members() {
    std::vector<Member> out;
    if (gId.empty()) return out;
    if (UseTest()) return TestMembers(gId);
    Steam& s = S();
    if (!s.ok) return out;
    const uint64_t id = Parse64(gId), owner = s.owner(s.mm, id);
    const int n = s.memberCount(s.mm, id);
    for (int i = 0; i < n && i < 64; ++i) {
        const uint64_t m = s.memberByIndex(s.mm, id, i);
        std::string name = steam::PersonaName(m);
        if (name.empty()) {
            steam::RequestPersona(m);
            name = "...";
        }
        out.push_back({std::to_string(m), name, m == owner});
    }
    std::stable_partition(out.begin(), out.end(), [](const Member& m) { return m.owner; });     // the owner first
    return out;
}

void Find(const std::string& code) {
    gResults.clear();
    gSearchCode = code;
    if (UseTest()) {
        CleanOrphans();
        for (const std::string& id : TestLobbyIds()) {
            bool fresh = false;
            const auto data = TestLobby(id, &fresh);
            if (!fresh || !data.count("ct")) continue;
            const int type = std::atoi(data.count("_type") ? data.at("_type").c_str() : "0");
            const bool codeMatch = !code.empty() && data.count("ch") && data.at("ch") == code;
            if (code.empty() ? !(type == kPublic || type == kFriends) : !(codeMatch && type != kInviteOnly)) continue;
            Found f;
            f.id = id;
            for (const auto& [k, v] : data)
                if (k[0] != '_') f.data[k] = v;
            f.members = static_cast<int>(TestMembers(id).size());
            f.max = std::atoi(data.count("_max") ? data.at("_max").c_str() : "0");
            f.friends = type == kFriends;      // in test copies every copy is the same account: a friend of itself
            gResults.push_back(f);
        }
        gSearching = false;
        return;
    }
    Steam& s = S();
    if (!s.ok) return;
    gSearching = true;
    s.stringFilter(s.mm, "ct", "1", kComparisonEqual);
    if (!code.empty()) s.stringFilter(s.mm, "ch", code.c_str(), kComparisonEqual);
    s.distanceFilter(s.mm, kDistanceWorldwide);
    s.countFilter(s.mm, 50);
    const bool withFriends = code.empty();
    steam::Await(s.requestList(s.mm), kLobbyMatchList, kLobbyMatchListSize, [withFriends](const uint8_t* r, bool failed) {
        Steam& st = S();
        uint32_t count = 0;
        if (r && !failed) std::memcpy(&count, r, 4);
        std::set<std::string> seen;
        for (uint32_t i = 0; i < count && i < 50; ++i) {
            const uint64_t id = st.lobbyByIndex(st.mm, static_cast<int>(i));
            if (!id) continue;
            Found f;
            f.id = std::to_string(id);
            seen.insert(f.id);
            gResults.push_back(f);
        }
        if (withFriends) {
            // friends' lobbies: friends-only lobbies are never in the list above
            const int n = st.friendCount(st.friends, kFriendFlagImmediate);
            for (int i = 0; i < n && i < 500; ++i) {
                const uint64_t f = st.friendByIndex(st.friends, i, kFriendFlagImmediate);
                uint8_t info[24] = {};
                if (!st.friendGame(st.friends, f, info)) continue;
                uint64_t game = 0, lobbyId = 0;
                std::memcpy(&game, info, 8);
                std::memcpy(&lobbyId, info + 16, 8);
                if ((game & 0xFFFFFF) != st.appId || !lobbyId || seen.count(std::to_string(lobbyId))) continue;
                st.requestData(st.mm, lobbyId);     // their data arrives a moment later (read again in Results)
                Found fl;
                fl.id = std::to_string(lobbyId);
                fl.friends = true;
                seen.insert(fl.id);
                gResults.push_back(fl);
            }
        }
        gSearching = false;
    });
}

bool Searching() { return gSearching; }

std::vector<Found> Results() {
    if (UseTest() || !S().ok) return gResults;
    std::vector<Found> out;
    Steam& s = S();
    for (Found f : gResults) {
        const uint64_t id = Parse64(f.id);
        f.data = SteamData(id);
        if (!f.data.count("ct") || f.data.at("ct") != "1") continue;  // not ours (or emptied), or a friend's not arrived
        const auto vis = f.data.find("vis");
        if (!gSearchCode.empty() ? f.data.count("ch") == 0 || f.data.at("ch") != gSearchCode
                                 : vis != f.data.end() && (vis->second == "code" || vis->second == "invite") && !f.friends)
            continue;                           // a code search finds only its lobby; the lists never show code-only ones
        f.members = s.memberCount(s.mm, id);
        f.max = s.memberLimit(s.mm, id);
        out.push_back(f);
    }
    return out;
}

bool InviteDialog() {
    if (UseTest() || gId.empty() || !S().ok) return false;
    S().inviteDialog(S().friends, Parse64(gId));
    return true;
}

std::string TakeInvite() {
    std::lock_guard<std::mutex> lock(gCallbackLock);
    std::string id;
    id.swap(gPendingInvite);
    return id;
}

std::string OwnerId() {
    if (gId.empty()) return "";
    if (UseTest()) {
        if (gOwner) return OwnTestId();
        const auto data = TestLobby(gId, nullptr);
        auto it = data.find("_owner");
        return it == data.end() ? "" : it->second;
    }
    return S().ok ? std::to_string(S().owner(S().mm, Parse64(gId))) : "";
}

bool IsFriend(const std::string& memberId) {
    if (UseTest()) return true;         // every test copy is the same Steam account
    static auto hasFriend = reinterpret_cast<bool (*)(void*, uint64_t, int)>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"steam_api64.dll"), "SteamAPI_ISteamFriends_HasFriend")));
    return hasFriend && S().friends && hasFriend(S().friends, Parse64(memberId), kFriendFlagImmediate);
}

int Type() { return gId.empty() || !gOwner ? -1 : gType; }

std::string OwnId() { return UseTest() ? OwnTestId() : std::to_string(steam::OwnSteamId()); }

bool Send(const std::string& text) {
    if (gId.empty() || gState != "in lobby" || text.empty() || text.size() > 1024 || text.find('\n') != std::string::npos) return false;
    if (!UseTest()) return S().ok && S().sendChat(S().mm, Parse64(gId), text.data(), static_cast<int>(text.size()));
    HANDLE h = CreateFileW(ChatFile(gId).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::string line = OwnTestId() + "|" + text + "\n";
    DWORD n = 0;
    const bool ok = WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &n, nullptr) && n == line.size();
    CloseHandle(h);
    return ok;
}

std::vector<Message> Receive() {
    std::vector<Message> out;
    if (!UseTest()) {
        std::lock_guard<std::mutex> lock(gCallbackLock);
        out.swap(gInbox);
        return out;
    }
    if (gId.empty()) return out;
    // only what was added since the last read
    HANDLE h = CreateFileW(ChatFile(gId).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{}, at{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart < gChatRead) gChatRead = 0;
    at.QuadPart = gChatRead;
    std::string text;
    if (size.QuadPart > gChatRead && SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) {
        text.resize(static_cast<size_t>(std::min<long long>(size.QuadPart - gChatRead, 64 * 1024)));
        DWORD n = 0;
        if (!ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr)) n = 0;
        text.resize(n);
    }
    CloseHandle(h);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t end = text.find('\n', pos);
        if (end == std::string::npos) break;               // a line still being written
        const std::string line = text.substr(pos, end - pos);
        const size_t bar = line.find('|');
        if (bar != std::string::npos) out.push_back({line.substr(0, bar), line.substr(bar + 1)});
        pos = end + 1;
    }
    gChatRead += static_cast<long long>(pos);
    return out;
}

std::string Status() {
    std::string members;
    for (const auto& m : Members()) members += (members.empty() ? "" : ", ") + m.name + (m.owner ? " (owner)" : "");
    return "backend=" + Backend() + " state=" + gState + " id=" + gId + " owner=" + (gOwner ? "1" : "0") + " members=[" + members +
           "] code=" + Data("code") + " addr=" + Data("addr") + " found=" + std::to_string(gResults.size());
}

}  // namespace lobby
