// Lobbies for Create Together: finding each other before the editor's own networking (together.hpp) connects.
//
// For players this is Steam matchmaking, through the game's steam_api64.dll (ISteamMatchmaking v009, ISteamFriends
// v018; layouts and callback ids from the Steamworks SDK headers): lobbies of this game, tagged as ours ("ct" = "1")
// so nothing else of the game's (it has no Steam lobbies of its own) is ever listed. The lobby only says where the
// editor is ("addr", the host's Steam id: the editor's net driver is SteamSocketsNetDriver); the editing itself is the
// editor's replication. Invites are Steam lobby invites (GameLobbyJoinRequested_t, or +connect_lobby on the command
// line when Steam starts the game for one); never rich presence "connect", which the game itself answers by opening
// the tower (BP_BallgameGameInstance.HandleInviteWhileRunning).
//
// In a sandboxed test copy Steam's lobbies are blocked (sandbox.cpp), so lobbies there are files in a folder shared by
// every test copy on this computer (%LOCALAPPDATA%\BallestTogetherTest): one per lobby, kept fresh by its owner every
// second, plus one per member. Same calls, same answers; nothing leaves the computer.
// Game thread only.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace lobby {

// Who can find and join a lobby: Steam's ELobbyType, by what it means here.
constexpr int kInviteOnly = 0;          // k_ELobbyTypePrivate: invites only
constexpr int kFriends = 1;             // k_ELobbyTypeFriendsOnly: friends see it (Find lists friends' lobbies)
constexpr int kPublic = 2;              // k_ELobbyTypePublic: everyone's list
constexpr int kCode = 3;                // k_ELobbyTypeInvisible: found only by its code

void Frame();
std::string Backend();                  // "steam", "test" (a sandboxed copy) or "none"

// The one lobby this game is in.
// State: "none", "creating", "joining", "in lobby", or "error: ..." (until the next Create or Join).
std::string State();
void Create(int type, int maxMembers);
void Join(const std::string& id);
void Leave();
std::string Id();                       // the lobby's id ("" if none)
bool IsOwner();
bool SetData(const std::string& key, const std::string& value);   // the owner's only
std::string Data(const std::string& key);
bool SetType(int type);                 // owner
bool SetJoinable(bool joinable);        // owner
int MaxMembers();
struct Member {
    std::string id, name;
    bool owner = false;
};
std::vector<Member> Members();         // the owner first

// Lobbies to join: every lobby of ours that's public, friends' lobbies, and (with a code) the one whose "ch" data is
// that code (a lookup key the plugin makes from the session's code: never the code itself). The lists never show
// code-only or invite-only lobbies, even though Steam returns invisible lobbies to searches.
// Results come a moment later (Searching false again).
struct Found {
    std::string id;
    std::map<std::string, std::string> data;
    int members = 0, max = 0;
    bool friends = false;               // a friend's lobby (found through the friend, not the public list)
};
void Find(const std::string& code);     // "" for the public and friends' lobbies
bool Searching();
std::vector<Found> Results();

// Steam's invite dialog for the lobby (the overlay's friend picker); false where there is none (a test copy).
bool InviteDialog();
// An invite the player accepted (in Steam, or the command line Steam started the game with): the lobby's id, given
// once; "" if none.
std::string TakeInvite();
std::string OwnName();
std::string OwnId();                    // this player's member id (their Steam id; a test copy's own)
std::string OwnerId();                  // the lobby owner's member id ("" if not in a lobby)
bool IsFriend(const std::string& memberId);     // one of this player's Steam friends (a test copy: always)
int Type();                             // the type this game's own lobby was made with (-1: not an owner)

// Short messages to everyone in the lobby (Steam lobby chat: k_EChatEntryTypeChatMsg; in a test copy a shared file),
// at most 1024 bytes each. Receive gives the ones that arrived since it was last asked, each with its sender's member
// id, the player's own included.
struct Message {
    std::string from, text;
};
bool Send(const std::string& text);
std::vector<Message> Receive();

std::string Status();                   // for the test channel

}  // namespace lobby
