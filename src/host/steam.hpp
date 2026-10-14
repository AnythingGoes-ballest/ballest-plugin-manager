// Steam, read-only: leaderboard entries, the files attached to them (ghost replays) and workshop searches, through
// the game's own steam_api64.dll (its flat C API; the game's Steam plugin wraps the same calls). Requests are polled
// once a frame (ISteamUtils IsAPICallCompleted / GetAPICallResult), so nothing is registered with Steam and nothing
// runs outside the game thread.
//
// Layouts and values are from the Steamworks SDK headers as Steamworks.NET generates them (8-byte packing on
// Windows): LeaderboardFindResult_t (1104), LeaderboardScoresDownloaded_t (1105), LeaderboardEntry_t,
// RemoteStorageDownloadUGCResult_t (1317), SteamUGCQueryCompleted_t (3401), SteamUGCDetails_t. The interface versions
// are the ones the game's DLL exports (SteamUserStats_v013, SteamRemoteStorage_v016, SteamUGC_v021, SteamUtils_v010).
// Game thread only.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace steam {

bool Available();                       // the game's Steam DLL is loaded and has everything used here
void Frame();                           // polls the requests under way; call once a frame
int Pending();                          // requests under way

struct Entry {
    uint64_t steamId = 0;
    int rank = 0, score = 0;
    uint64_t file = 0;                  // the attached file (UGC handle), 0 for none
};

// A leaderboard by name: its handle, 0 if there is no such leaderboard.
void FindLeaderboard(const std::string& name, std::function<void(uint64_t handle)> done);
// Entries by global rank, first..last (1-based, inclusive); around the player: their own entry only.
void DownloadEntries(uint64_t leaderboard, int first, int last, std::function<void(bool ok, std::vector<Entry>)> done);
void DownloadOwnEntry(uint64_t leaderboard, std::function<void(bool ok, std::vector<Entry>)> done);
// A file attached to an entry (a ghost replay), whole.
void DownloadFile(uint64_t file, std::function<void(bool ok, std::string bytes)> done);

struct Item {
    uint64_t id = 0, owner = 0;
    uint64_t preview = 0;               // the preview image's file (UGC handle), 0 for none
    std::string title;
};
// How many entries a leaderboard has in all (after its entries have been downloaded once).
int EntryCount(uint64_t leaderboard);
// Workshop items of this game matching the text, one page of results (1-based); total is all the matches.
void SearchWorkshop(const std::string& text, int page, std::function<void(bool ok, std::vector<Item>, int total)> done);

// A workshop query of this game's items, one page of up to 50 (kResultsPerPage) results (1-based pages).
//   * kAll: every item, ranked by `rank` (EUGCQuery), matching `text` (title, description or tags), with every tag in
//     `with` (any one of them if `anyTag`) and none in `without`; `trendDays` for the trend rankings.
//   * kUser: one Steam account's list (EUserUGCList `list`, sorted by EUserUGCListSortOrder `userSort`).
//   * kIds: the items with these ids (one page, at most 50).
constexpr int kResultsPerPage = 50;
struct Query {
    enum Kind { kAll, kUser, kIds } kind = kAll;
    int rank = 0, trendDays = 7, page = 1;
    std::string text;
    std::vector<std::string> with, without;
    bool anyTag = false;
    uint32_t account = 0;
    int list = 0, userSort = 0;
    std::vector<uint64_t> ids;
};
// EUGCQuery, EUserUGCList and EUserUGCListSortOrder values used (the game's SIK enums carry the same values).
constexpr int kRankVote = 0, kRankPublished = 1, kRankTrend = 3, kRankNotYetRated = 8, kRankVotesUp = 10,
              kRankTextSearch = 11, kRankSubscriptions = 12, kRankPlaySessions = 18, kRankUpdated = 19;
constexpr int kListPublished = 0, kListVotedUp = 2, kListVotedDown = 3, kListFavorited = 5, kListSubscribed = 6,
              kListPlayed = 7;
constexpr int kSortNewest = 0, kSortOldest = 1, kSortTitle = 2, kSortUpdated = 3, kSortSubscribed = 4, kSortTop = 5;

struct Details {
    uint64_t id = 0, owner = 0, preview = 0;
    std::string title, description, tags, metadata;
    uint32_t created = 0, updated = 0, votesUp = 0, votesDown = 0;
    float score = 0;
    int64_t size = 0;
    uint64_t plays = 0, subscribers = 0, favorites = 0;
};
void QueryWorkshop(const Query& query, std::function<void(bool ok, std::vector<Details>, int total)> done);

// A Steam user's name as Steam shows it to this player; "" while unknown (asking for it starts a request: Steam keeps
// the name once it arrives). Your own Steam id.
std::string PersonaName(uint64_t steamId);
bool RequestPersona(uint64_t steamId);
uint64_t OwnSteamId();

// A Steam Web API auth ticket for this player, per plugin (ISteamUser::GetAuthTicketForWebApi), so the plugin's server can
// check who is asking (ISteamUserAuth/AuthenticateUserTicket with the same identity). The identity is always
// WebTicketIdentity(plugin id), "ballest-plugin:<id>": a plugin can't ask for a ticket meant for another service, and its
// ticket is answered only to it. Read-only: Steam makes the ticket, nothing is uploaded or changed. The ticket comes in
// Steam's GetTicketForWebApiResponse_t callback (168), received through SteamAPI_RegisterCallback (the game dispatches
// callbacks with SteamAPI_RunCallbacks: its exe imports both). One per plugin; asking again cancels its last ticket
// (CancelAuthTicket), and so does the plugin stopping (ForgetWebTicket). A sandboxed test copy blocks the call
// (sandbox.cpp: GetAuthTicketForWebApi is one of its Steam blocks), so it ends in an error there.
std::string WebTicketIdentity(const std::string& pluginId);
bool RequestWebTicket(int owner, const std::string& pluginId);
std::string WebTicketState(int owner);      // "idle", "pending", "ready", "error: ..."
std::string WebTicket(int owner);           // the ticket as lowercase hex once ready, else ""
void ForgetWebTicket(int owner);

// A workshop item on this computer: EItemState flags (Subscribed 1, Installed 4, NeedsUpdate 8, Downloading 16,
// DownloadPending 32), where it is installed ("" if it isn't), and asking Steam to download it (as the game's own
// track browser does before playing one).
constexpr uint32_t kItemInstalled = 4, kItemNeedsUpdate = 8, kItemDownloading = 16, kItemDownloadPending = 32;
uint32_t ItemState(uint64_t item);
std::string ItemFolder(uint64_t item);
bool DownloadItem(uint64_t item);

}  // namespace steam
