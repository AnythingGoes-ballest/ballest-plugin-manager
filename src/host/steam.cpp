#include "steam.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include "log.hpp"

namespace steam {
namespace {

using Call = uint64_t;                  // SteamAPICall_t
constexpr Call kInvalidCall = 0;
constexpr int kResultOk = 1;            // EResult k_EResultOK

// Callback ids (k_iCallback) and sizes of the results polled for.
constexpr int kLeaderboardFindResult = 1104, kLeaderboardFindResultSize = 16;
constexpr int kLeaderboardScoresDownloaded = 1105, kLeaderboardScoresDownloadedSize = 24;
constexpr int kDownloadUGCResult = 1317, kDownloadUGCResultSize = 296;
constexpr int kUGCQueryCompleted = 3401, kUGCQueryCompletedSize = 280;
// SteamUGCDetails_t: m_nPublishedFileId at 0, m_rgchTitle[129] at 24, m_ulSteamIDOwner at 8160, m_hFile at 9216,
// m_hPreviewFile at 9224 (after m_rgchTags[1025] from 8187, aligned to 8); the whole struct is a little under 10 KB,
// so it is read into a larger buffer.
constexpr size_t kDetailsTitle = 24, kDetailsTitleSize = 129, kDetailsOwner = 8160, kDetailsPreview = 9224, kDetailsBuffer = 16384;
// The rest of SteamUGCDetails_t (fields in the order of Steamworks.NET's SteamStructs.cs, sizes from its
// SteamConstants.cs, 8-byte packing): m_rgchDescription[8000] at 153, m_rtimeCreated 8168, m_rtimeUpdated 8172,
// m_rgchTags[1025] 8187, m_pchFileName[260] 9232, m_nFileSize 9492, m_rgchURL[256] 9500, m_unVotesUp 9756,
// m_unVotesDown 9760, m_flScore 9764, m_unNumChildren 9768, m_ulTotalFilesSize 9776.
constexpr size_t kDetailsDescription = 153, kDetailsDescriptionSize = 8000, kDetailsCreated = 8168, kDetailsUpdated = 8172,
                 kDetailsTags = 8187, kDetailsTagsSize = 1025, kDetailsFileSize = 9492, kDetailsVotesUp = 9756,
                 kDetailsVotesDown = 9760, kDetailsScore = 9764;
constexpr uint32_t kMetadataMax = 10000;  // k_cchDeveloperMetadataMax (Steamworks.NET SteamConstants.cs)
// EItemStatistic
constexpr int kStatFavorites = 1, kStatUniqueSubscriptions = 3, kStatPlaytimeSessions = 9;
constexpr int kLeaderboardRequestGlobal = 0, kLeaderboardRequestAroundUser = 1;   // ELeaderboardDataRequest
constexpr int kUGCQueryRankedByTextSearch = 11, kUGCMatchingItems = 0;           // EUGCQuery, EUGCMatchingUGCType
constexpr int kUGCReadUntilFinished = 0;                                         // EUGCReadAction

struct Api {
    void* stats = nullptr;
    void* storage = nullptr;
    void* ugc = nullptr;
    void* utils = nullptr;
    Call (*findLeaderboard)(void*, const char*) = nullptr;
    Call (*downloadEntries)(void*, uint64_t, int, int, int) = nullptr;
    bool (*downloadedEntry)(void*, uint64_t, int, void*, int32_t*, int) = nullptr;
    Call (*ugcDownload)(void*, uint64_t, uint32_t) = nullptr;
    int32_t (*ugcRead)(void*, uint64_t, void*, int32_t, uint32_t, int) = nullptr;
    bool (*callCompleted)(void*, Call, bool*) = nullptr;
    bool (*callResult)(void*, Call, void*, int, int, bool*) = nullptr;
    uint32_t (*appId)(void*) = nullptr;
    uint64_t (*createQuery)(void*, int, int, uint32_t, uint32_t, uint32_t) = nullptr;
    bool (*setSearchText)(void*, uint64_t, const char*) = nullptr;
    Call (*sendQuery)(void*, uint64_t) = nullptr;
    bool (*queryResult)(void*, uint64_t, uint32_t, void*) = nullptr;
    bool (*releaseQuery)(void*, uint64_t) = nullptr;
    uint32_t (*itemState)(void*, uint64_t) = nullptr;
    int (*entryCount)(void*, uint64_t) = nullptr;
    bool (*itemInstallInfo)(void*, uint64_t, uint64_t*, char*, uint32_t, uint32_t*) = nullptr;
    bool (*downloadItem)(void*, uint64_t, bool) = nullptr;
    bool ok = false;
    // For QueryWorkshop and names: not needed by the rest, so missing ones only turn those off.
    void* friends = nullptr;
    void* user = nullptr;
    uint64_t (*createUserQuery)(void*, uint32_t, int, int, int, uint32_t, uint32_t, uint32_t) = nullptr;
    uint64_t (*createDetailsQuery)(void*, const uint64_t*, uint32_t) = nullptr;
    bool (*addRequiredTag)(void*, uint64_t, const char*) = nullptr;
    bool (*addExcludedTag)(void*, uint64_t, const char*) = nullptr;
    bool (*setMatchAnyTag)(void*, uint64_t, bool) = nullptr;
    bool (*setTrendDays)(void*, uint64_t, uint32_t) = nullptr;
    bool (*setPlaytimeStats)(void*, uint64_t, uint32_t) = nullptr;
    bool (*setReturnMetadata)(void*, uint64_t, bool) = nullptr;
    bool (*queryMetadata)(void*, uint64_t, uint32_t, char*, uint32_t) = nullptr;
    bool (*queryStatistic)(void*, uint64_t, uint32_t, int, uint64_t*) = nullptr;
    const char* (*personaName)(void*, uint64_t) = nullptr;
    bool (*requestUserInfo)(void*, uint64_t, bool) = nullptr;
    uint64_t (*steamId)(void*) = nullptr;
    bool queries = false, names = false;
};

Api& TheApi() {
    static Api api;
    static bool tried = false;
    if (tried) return api;
    tried = true;
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    if (!dll) {
        hostlog::Warn("steam: the game's steam_api64.dll is not loaded");
        return api;
    }
    auto get = [&](const char* name) {
        FARPROC p = GetProcAddress(dll, name);
        if (!p) hostlog::Warn(std::string("steam: missing ") + name);
        return reinterpret_cast<void*>(p);
    };
    auto open = [&](const char* name) {
        auto accessor = reinterpret_cast<void* (*)()>(get(name));
        return accessor ? accessor() : nullptr;
    };
    api.stats = open("SteamAPI_SteamUserStats_v013");
    api.storage = open("SteamAPI_SteamRemoteStorage_v016");
    api.ugc = open("SteamAPI_SteamUGC_v021");
    api.utils = open("SteamAPI_SteamUtils_v010");
    api.findLeaderboard = reinterpret_cast<decltype(api.findLeaderboard)>(get("SteamAPI_ISteamUserStats_FindLeaderboard"));
    api.downloadEntries = reinterpret_cast<decltype(api.downloadEntries)>(get("SteamAPI_ISteamUserStats_DownloadLeaderboardEntries"));
    api.downloadedEntry = reinterpret_cast<decltype(api.downloadedEntry)>(get("SteamAPI_ISteamUserStats_GetDownloadedLeaderboardEntry"));
    api.ugcDownload = reinterpret_cast<decltype(api.ugcDownload)>(get("SteamAPI_ISteamRemoteStorage_UGCDownload"));
    api.ugcRead = reinterpret_cast<decltype(api.ugcRead)>(get("SteamAPI_ISteamRemoteStorage_UGCRead"));
    api.callCompleted = reinterpret_cast<decltype(api.callCompleted)>(get("SteamAPI_ISteamUtils_IsAPICallCompleted"));
    api.callResult = reinterpret_cast<decltype(api.callResult)>(get("SteamAPI_ISteamUtils_GetAPICallResult"));
    api.appId = reinterpret_cast<decltype(api.appId)>(get("SteamAPI_ISteamUtils_GetAppID"));
    api.createQuery = reinterpret_cast<decltype(api.createQuery)>(get("SteamAPI_ISteamUGC_CreateQueryAllUGCRequestPage"));
    api.setSearchText = reinterpret_cast<decltype(api.setSearchText)>(get("SteamAPI_ISteamUGC_SetSearchText"));
    api.sendQuery = reinterpret_cast<decltype(api.sendQuery)>(get("SteamAPI_ISteamUGC_SendQueryUGCRequest"));
    api.queryResult = reinterpret_cast<decltype(api.queryResult)>(get("SteamAPI_ISteamUGC_GetQueryUGCResult"));
    api.releaseQuery = reinterpret_cast<decltype(api.releaseQuery)>(get("SteamAPI_ISteamUGC_ReleaseQueryUGCRequest"));
    api.itemState = reinterpret_cast<decltype(api.itemState)>(get("SteamAPI_ISteamUGC_GetItemState"));
    api.entryCount = reinterpret_cast<decltype(api.entryCount)>(get("SteamAPI_ISteamUserStats_GetLeaderboardEntryCount"));
    api.itemInstallInfo = reinterpret_cast<decltype(api.itemInstallInfo)>(get("SteamAPI_ISteamUGC_GetItemInstallInfo"));
    api.downloadItem = reinterpret_cast<decltype(api.downloadItem)>(get("SteamAPI_ISteamUGC_DownloadItem"));
    api.ok = api.stats && api.storage && api.ugc && api.utils && api.findLeaderboard && api.downloadEntries && api.downloadedEntry &&
             api.ugcDownload && api.ugcRead && api.callCompleted && api.callResult && api.appId && api.createQuery && api.setSearchText &&
             api.sendQuery && api.queryResult && api.releaseQuery && api.itemState && api.itemInstallInfo && api.downloadItem &&
             api.entryCount;
    hostlog::Info(api.ok ? "steam: ready" : "steam: not everything needed is there; leaderboard ghosts are off");
    // SteamFriends_v018 and SteamUser_v023: the versions the game's steam_api64.dll exports (read from the DLL).
    api.friends = open("SteamAPI_SteamFriends_v018");
    api.user = open("SteamAPI_SteamUser_v023");
    api.createUserQuery = reinterpret_cast<decltype(api.createUserQuery)>(get("SteamAPI_ISteamUGC_CreateQueryUserUGCRequest"));
    api.createDetailsQuery = reinterpret_cast<decltype(api.createDetailsQuery)>(get("SteamAPI_ISteamUGC_CreateQueryUGCDetailsRequest"));
    api.addRequiredTag = reinterpret_cast<decltype(api.addRequiredTag)>(get("SteamAPI_ISteamUGC_AddRequiredTag"));
    api.addExcludedTag = reinterpret_cast<decltype(api.addExcludedTag)>(get("SteamAPI_ISteamUGC_AddExcludedTag"));
    api.setMatchAnyTag = reinterpret_cast<decltype(api.setMatchAnyTag)>(get("SteamAPI_ISteamUGC_SetMatchAnyTag"));
    api.setTrendDays = reinterpret_cast<decltype(api.setTrendDays)>(get("SteamAPI_ISteamUGC_SetRankedByTrendDays"));
    api.setPlaytimeStats = reinterpret_cast<decltype(api.setPlaytimeStats)>(get("SteamAPI_ISteamUGC_SetReturnPlaytimeStats"));
    api.setReturnMetadata = reinterpret_cast<decltype(api.setReturnMetadata)>(get("SteamAPI_ISteamUGC_SetReturnMetadata"));
    api.queryMetadata = reinterpret_cast<decltype(api.queryMetadata)>(get("SteamAPI_ISteamUGC_GetQueryUGCMetadata"));
    api.queryStatistic = reinterpret_cast<decltype(api.queryStatistic)>(get("SteamAPI_ISteamUGC_GetQueryUGCStatistic"));
    api.personaName = reinterpret_cast<decltype(api.personaName)>(get("SteamAPI_ISteamFriends_GetFriendPersonaName"));
    api.requestUserInfo = reinterpret_cast<decltype(api.requestUserInfo)>(get("SteamAPI_ISteamFriends_RequestUserInformation"));
    api.steamId = reinterpret_cast<decltype(api.steamId)>(get("SteamAPI_ISteamUser_GetSteamID"));
    api.queries = api.ok && api.createUserQuery && api.createDetailsQuery && api.addRequiredTag && api.addExcludedTag &&
                  api.setMatchAnyTag && api.setTrendDays && api.setPlaytimeStats && api.setReturnMetadata && api.queryMetadata &&
                  api.queryStatistic;
    api.names = api.friends && api.user && api.personaName && api.requestUserInfo && api.steamId;
    if (!api.queries || !api.names) hostlog::Warn("steam: workshop queries or player names are missing; Workshop is off");
    return api;
}

// A request under way: polled until Steam has its result, which is then handed to `done` (failed: no result).
struct Request {
    Call call = kInvalidCall;
    int callback = 0, size = 0;
    std::function<void(const uint8_t* result, bool failed)> done;
};
std::vector<Request> gRequests;

void Start(Call call, int callback, int size, std::function<void(const uint8_t*, bool)> done) {
    if (call == kInvalidCall) return done(nullptr, true);
    gRequests.push_back({call, callback, size, std::move(done)});
}

template <class T>
T At(const uint8_t* p, size_t offset) {
    T v;
    std::memcpy(&v, p + offset, sizeof v);
    return v;
}

void ReadEntries(uint64_t entries, int count, std::function<void(bool, std::vector<Entry>)>& done) {
    Api& api = TheApi();
    std::vector<Entry> list;
    for (int i = 0; i < count; ++i) {
        uint8_t raw[32] = {};           // LeaderboardEntry_t: steamID 0, rank 8, score 12, details 16, UGC handle 24
        if (!api.downloadedEntry(api.stats, entries, i, raw, nullptr, 0)) continue;
        const uint64_t file = At<uint64_t>(raw, 24);
        list.push_back({At<uint64_t>(raw, 0), At<int32_t>(raw, 8), At<int32_t>(raw, 12), file == ~0ull ? 0 : file});
    }
    done(true, std::move(list));
}

}  // namespace

bool Available() { return TheApi().ok; }
int Pending() { return static_cast<int>(gRequests.size()); }

void Frame() {
    if (gRequests.empty()) return;
    Api& api = TheApi();
    // A request's `done` may start more; they are polled from the next frame.
    std::vector<Request> ready;
    for (size_t i = 0; i < gRequests.size();) {
        bool failed = false;
        if (api.callCompleted(api.utils, gRequests[i].call, &failed)) {
            ready.push_back(std::move(gRequests[i]));
            gRequests.erase(gRequests.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (auto& r : ready) {
        std::vector<uint8_t> result(static_cast<size_t>(r.size));
        bool failed = false;
        const bool got = api.callResult(api.utils, r.call, result.data(), r.size, r.callback, &failed);
        r.done(got && !failed ? result.data() : nullptr, !got || failed);
    }
}

void FindLeaderboard(const std::string& name, std::function<void(uint64_t)> done) {
    Api& api = TheApi();
    if (!api.ok) return done(0);
    Start(api.findLeaderboard(api.stats, name.c_str()), kLeaderboardFindResult, kLeaderboardFindResultSize,
          [done](const uint8_t* r, bool failed) { done(!failed && r && At<uint8_t>(r, 8) ? At<uint64_t>(r, 0) : 0); });
}

void DownloadEntries(uint64_t leaderboard, int first, int last, std::function<void(bool, std::vector<Entry>)> done) {
    Api& api = TheApi();
    if (!api.ok || !leaderboard) return done(false, {});
    Start(api.downloadEntries(api.stats, leaderboard, kLeaderboardRequestGlobal, first, last), kLeaderboardScoresDownloaded,
          kLeaderboardScoresDownloadedSize, [done](const uint8_t* r, bool failed) mutable {
              if (failed || !r) return done(false, {});
              ReadEntries(At<uint64_t>(r, 8), At<int32_t>(r, 16), done);
          });
}

void DownloadOwnEntry(uint64_t leaderboard, std::function<void(bool, std::vector<Entry>)> done) {
    Api& api = TheApi();
    if (!api.ok || !leaderboard) return done(false, {});
    Start(api.downloadEntries(api.stats, leaderboard, kLeaderboardRequestAroundUser, 0, 0), kLeaderboardScoresDownloaded,
          kLeaderboardScoresDownloadedSize, [done](const uint8_t* r, bool failed) mutable {
              if (failed || !r) return done(false, {});
              ReadEntries(At<uint64_t>(r, 8), At<int32_t>(r, 16), done);
          });
}

void DownloadFile(uint64_t file, std::function<void(bool, std::string)> done) {
    Api& api = TheApi();
    if (!api.ok || !file) return done(false, "");
    Start(api.ugcDownload(api.storage, file, 0), kDownloadUGCResult, kDownloadUGCResultSize, [done, file](const uint8_t* r, bool failed) {
        // RemoteStorageDownloadUGCResult_t: m_eResult 0, m_hFile 8, m_nAppID 16, m_nSizeInBytes 20.
        if (failed || !r || At<int32_t>(r, 0) != kResultOk) return done(false, "");
        const int32_t size = At<int32_t>(r, 20);
        if (size <= 0 || size > 64 * 1024 * 1024) return done(false, "");
        Api& a = TheApi();
        std::string bytes(static_cast<size_t>(size), '\0');
        const int32_t read = a.ugcRead(a.storage, file, bytes.data(), size, 0, kUGCReadUntilFinished);
        if (read <= 0) return done(false, "");
        bytes.resize(static_cast<size_t>(read));
        done(true, std::move(bytes));
    });
}

void SearchWorkshop(const std::string& text, int page, std::function<void(bool, std::vector<Item>, int)> done) {
    Api& api = TheApi();
    if (!api.ok) return done(false, {}, 0);
    const uint32_t app = api.appId(api.utils);
    const uint64_t query = api.createQuery(api.ugc, kUGCQueryRankedByTextSearch, kUGCMatchingItems, app, app, static_cast<uint32_t>(page < 1 ? 1 : page));
    if (!query || query == ~0ull) return done(false, {}, 0);
    if (!text.empty()) api.setSearchText(api.ugc, query, text.c_str());
    Start(api.sendQuery(api.ugc, query), kUGCQueryCompleted, kUGCQueryCompletedSize, [done, query](const uint8_t* r, bool failed) {
        Api& a = TheApi();
        std::vector<Item> items;
        int total = 0;
        // SteamUGCQueryCompleted_t: m_handle 0, m_eResult 8, m_unNumResultsReturned 12, m_unTotalMatchingResults 16.
        const bool ok = !failed && r && At<int32_t>(r, 8) == kResultOk;
        if (ok) {
            const uint32_t count = At<uint32_t>(r, 12);
            total = static_cast<int>(At<uint32_t>(r, 16));
            std::vector<uint8_t> details(kDetailsBuffer);
            for (uint32_t i = 0; i < count; ++i) {
                std::fill(details.begin(), details.end(), uint8_t{0});
                if (!a.queryResult(a.ugc, query, i, details.data())) continue;
                Item item;
                item.id = At<uint64_t>(details.data(), 0);
                item.owner = At<uint64_t>(details.data(), kDetailsOwner);
                item.preview = At<uint64_t>(details.data(), kDetailsPreview);
                if (item.preview == ~0ull) item.preview = 0;
                const char* title = reinterpret_cast<const char*>(details.data() + kDetailsTitle);
                item.title.assign(title, strnlen(title, kDetailsTitleSize));
                items.push_back(std::move(item));
            }
        }
        a.releaseQuery(a.ugc, query);
        done(ok, std::move(items), total);
    });
}

void QueryWorkshop(const Query& q, std::function<void(bool, std::vector<Details>, int)> done) {
    Api& api = TheApi();
    if (!api.queries) return done(false, {}, 0);
    const uint32_t app = api.appId(api.utils);
    const uint32_t page = static_cast<uint32_t>(q.page < 1 ? 1 : q.page);
    uint64_t query = 0;
    if (q.kind == Query::kAll) {
        query = api.createQuery(api.ugc, q.rank, kUGCMatchingItems, app, app, page);
    } else if (q.kind == Query::kUser) {
        query = api.createUserQuery(api.ugc, q.account, q.list, kUGCMatchingItems, q.userSort, app, app, page);
    } else {
        if (q.ids.empty()) return done(true, {}, 0);
        const size_t n = std::min<size_t>(q.ids.size(), kResultsPerPage);
        query = api.createDetailsQuery(api.ugc, q.ids.data(), static_cast<uint32_t>(n));
    }
    if (!query || query == ~0ull) return done(false, {}, 0);
    if (q.kind == Query::kAll) {
        if (!q.text.empty()) api.setSearchText(api.ugc, query, q.text.c_str());
        for (const auto& tag : q.with) api.addRequiredTag(api.ugc, query, tag.c_str());
        for (const auto& tag : q.without) api.addExcludedTag(api.ugc, query, tag.c_str());
        if (q.anyTag && !q.with.empty()) api.setMatchAnyTag(api.ugc, query, true);
        if (q.rank == kRankTrend) api.setTrendDays(api.ugc, query, static_cast<uint32_t>(q.trendDays < 1 ? 1 : q.trendDays));
    }
    // As the game's own hub asks (DA_DefaultHubQueries: metadata, and playtime stats for 0 days).
    api.setReturnMetadata(api.ugc, query, true);
    api.setPlaytimeStats(api.ugc, query, 0);
    Start(api.sendQuery(api.ugc, query), kUGCQueryCompleted, kUGCQueryCompletedSize, [done, query](const uint8_t* r, bool failed) {
        Api& a = TheApi();
        std::vector<Details> items;
        int total = 0;
        const bool ok = !failed && r && At<int32_t>(r, 8) == kResultOk;
        if (ok) {
            const uint32_t count = At<uint32_t>(r, 12);
            total = static_cast<int>(At<uint32_t>(r, 16));
            std::vector<uint8_t> d(kDetailsBuffer);
            std::vector<char> metadata(kMetadataMax + 1);
            auto text = [&](size_t at, size_t size) {
                const char* s = reinterpret_cast<const char*>(d.data() + at);
                return std::string(s, strnlen(s, size));
            };
            for (uint32_t i = 0; i < count; ++i) {
                std::fill(d.begin(), d.end(), uint8_t{0});
                if (!a.queryResult(a.ugc, query, i, d.data())) continue;
                Details item;
                item.id = At<uint64_t>(d.data(), 0);
                if (!item.id) continue;
                item.owner = At<uint64_t>(d.data(), kDetailsOwner);
                item.preview = At<uint64_t>(d.data(), kDetailsPreview);
                if (item.preview == ~0ull) item.preview = 0;
                item.title = text(kDetailsTitle, kDetailsTitleSize);
                item.description = text(kDetailsDescription, kDetailsDescriptionSize);
                item.tags = text(kDetailsTags, kDetailsTagsSize);
                item.created = At<uint32_t>(d.data(), kDetailsCreated);
                item.updated = At<uint32_t>(d.data(), kDetailsUpdated);
                item.size = At<int32_t>(d.data(), kDetailsFileSize);
                item.votesUp = At<uint32_t>(d.data(), kDetailsVotesUp);
                item.votesDown = At<uint32_t>(d.data(), kDetailsVotesDown);
                item.score = At<float>(d.data(), kDetailsScore);
                std::fill(metadata.begin(), metadata.end(), char{0});
                if (a.queryMetadata(a.ugc, query, i, metadata.data(), kMetadataMax)) item.metadata = metadata.data();
                uint64_t v = 0;
                if (a.queryStatistic(a.ugc, query, i, kStatPlaytimeSessions, &v)) item.plays = v;
                v = 0;
                if (a.queryStatistic(a.ugc, query, i, kStatUniqueSubscriptions, &v)) item.subscribers = v;
                v = 0;
                if (a.queryStatistic(a.ugc, query, i, kStatFavorites, &v)) item.favorites = v;
                items.push_back(std::move(item));
            }
        }
        a.releaseQuery(a.ugc, query);
        done(ok, std::move(items), total);
    });
}

std::string PersonaName(uint64_t steamId) {
    Api& api = TheApi();
    if (!api.names || !steamId) return "";
    const char* name = api.personaName(api.friends, steamId);
    // "[unknown]" until Steam has the user's information (ISteamFriends::GetFriendPersonaName).
    if (!name || !*name || std::strcmp(name, "[unknown]") == 0) return "";
    return name;
}

bool RequestPersona(uint64_t steamId) {
    Api& api = TheApi();
    return api.names && steamId && api.requestUserInfo(api.friends, steamId, true);
}

uint64_t OwnSteamId() {
    Api& api = TheApi();
    return api.names ? api.steamId(api.user) : 0;
}

// --- web API tickets ------------------------------------------------------------------------------------------------
namespace {
// GetTicketForWebApiResponse_t (Steamworks SDK isteamuser.h, k_iSteamUserCallbacks + 68): HAuthTicket at 0, EResult at
// 4, m_cubTicket at 8, m_rgubTicket[k_nCubTicketMaxLength = 2560] at 12.
constexpr int kWebTicketCallback = 168, kWebTicketSize = 12 + 2560;
// Each plugin's own ticket: asked for under its own identity, answered only to it.
struct Ticket {
    std::string state = "idle", hex;
    uint32_t handle = 0;                    // the ticket asked for (k_HAuthTicketInvalid = 0)
};
std::map<int, Ticket> gTickets;             // by plugin index

// A CCallbackBase as Steam sees it: a table pointer, m_nCallbackFlags (uint8) at 8 and m_iCallback (int) at 12. The
// table's first two entries are the two Run overloads (MSVC orders overloads in reverse, so Run(param, ioFailure, call)
// comes first on Windows); both are pointed at the same function, which only reads `param`, so the order can't matter.
// The third is GetCallbackSizeBytes.
struct CallbackObject {
    void* const* table;
    uint8_t flags;
    int32_t callback;
};

void TicketArrived(void*, void* param) {
    if (!param) return;
    const auto* p = static_cast<const uint8_t*>(param);
    const uint32_t handle = At<uint32_t>(p, 0);
    const int32_t result = At<int32_t>(p, 4), size = At<int32_t>(p, 8);
    for (auto& [owner, t] : gTickets) {
        if (!handle || t.handle != handle || t.state != "pending") continue;    // not this plugin's (or the game's own)
        if (result != kResultOk || size <= 0 || size > 2560) {
            t.state = "error: Steam answered " + std::to_string(result);
            return;
        }
        static const char* digits = "0123456789abcdef";
        t.hex.clear();
        for (int i = 0; i < size; ++i) {
            t.hex += digits[p[12 + i] >> 4];
            t.hex += digits[p[12 + i] & 15];
        }
        t.state = "ready";
        return;
    }
}
void TicketRunWithCall(void* self, void* param, bool, uint64_t) { TicketArrived(self, param); }
int TicketSize(void*) { return kWebTicketSize; }

void* const gTicketTable[] = {reinterpret_cast<void*>(&TicketRunWithCall), reinterpret_cast<void*>(&TicketArrived),
                              reinterpret_cast<void*>(&TicketSize)};
CallbackObject gTicketCallback{gTicketTable, 0, 0};
bool gTicketRegistered = false;

using CancelFn = void (*)(void*, uint32_t);
CancelFn CancelTicketFn() {
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    return dll ? reinterpret_cast<CancelFn>(reinterpret_cast<void*>(GetProcAddress(dll, "SteamAPI_ISteamUser_CancelAuthTicket"))) : nullptr;
}
}  // namespace

std::string WebTicketIdentity(const std::string& pluginId) { return "ballest-plugin:" + pluginId; }

bool RequestWebTicket(int owner, const std::string& pluginId) {
    Ticket& t = gTickets[owner];
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    Api& api = TheApi();
    if (!dll || !api.user) {
        t.state = "error: Steam is not available";
        return false;
    }
    using GetTicketFn = uint32_t (*)(void*, const char*);
    using RegisterFn = void (*)(void*, int);
    auto getTicket = reinterpret_cast<GetTicketFn>(reinterpret_cast<void*>(GetProcAddress(dll, "SteamAPI_ISteamUser_GetAuthTicketForWebApi")));
    auto cancel = CancelTicketFn();
    auto registerCallback = reinterpret_cast<RegisterFn>(reinterpret_cast<void*>(GetProcAddress(dll, "SteamAPI_RegisterCallback")));
    if (!getTicket || !cancel || !registerCallback) {
        t.state = "error: this Steam has no web API tickets";
        return false;
    }
    if (!gTicketRegistered) {
        registerCallback(&gTicketCallback, kWebTicketCallback);
        gTicketRegistered = true;
    }
    if (t.handle) cancel(api.user, t.handle);
    t.hex.clear();
    const std::string identity = WebTicketIdentity(pluginId);
    t.handle = getTicket(api.user, identity.c_str());
    if (!t.handle) {
        t.state = "error: Steam refused the ticket (a sandboxed test copy blocks it)";
        return false;
    }
    t.state = "pending";
    return true;
}

std::string WebTicketState(int owner) {
    auto it = gTickets.find(owner);
    return it == gTickets.end() ? "idle" : it->second.state;
}
std::string WebTicket(int owner) {
    auto it = gTickets.find(owner);
    return it != gTickets.end() && it->second.state == "ready" ? it->second.hex : std::string();
}
void ForgetWebTicket(int owner) {
    auto it = gTickets.find(owner);
    if (it == gTickets.end()) return;
    Api& api = TheApi();
    if (it->second.handle && api.user)
        if (auto cancel = CancelTicketFn()) cancel(api.user, it->second.handle);
    gTickets.erase(it);
}

uint32_t ItemState(uint64_t item) {
    Api& api = TheApi();
    return api.ok && item ? api.itemState(api.ugc, item) : 0;
}

std::string ItemFolder(uint64_t item) {
    Api& api = TheApi();
    if (!api.ok || !item) return "";
    uint64_t size = 0;
    uint32_t stamp = 0;
    char folder[1024] = {};
    if (!api.itemInstallInfo(api.ugc, item, &size, folder, sizeof folder, &stamp)) return "";
    return folder;
}

bool DownloadItem(uint64_t item) {
    Api& api = TheApi();
    return api.ok && item && api.downloadItem(api.ugc, item, true);
}

int EntryCount(uint64_t leaderboard) {
    Api& api = TheApi();
    return api.ok && leaderboard ? api.entryCount(api.stats, leaderboard) : 0;
}

}  // namespace steam
