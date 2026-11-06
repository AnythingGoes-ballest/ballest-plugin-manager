// Create Together: editing one map with other players, through the editor's own replication.
//
// The game's track editor is the SKG multiplayer level editor: its pieces and the editing pawn are replicated
// actors, and every change goes through the handler's Server_ functions. So an editor opened as an Unreal listen
// server is shared by every client that connects to it: the game itself spawns their pawns and replicates every
// piece, material and property. What the host adds:
//   * a listen server: the game opens its editor with OpenLevel(Map_LevelEditorMain, "game=...GM_LevelEditor_C");
//     while ListenNextEditor is armed, that travel is made again with "?listen" added (OpenLevelHooked);
//   * joining: an Unreal client travel to the host's address (Join);
//   * how players reach each other: Steam P2P (steam lobbies and networking messages, lobby.cpp) carries the
//     Unreal net driver's UDP datagrams between the players' own loopback ports (relay.cpp), so nobody needs an open
//     port. In a sandboxed test copy Steam is blocked and two copies reach each other on loopback directly
//     (sandbox.hpp: kTogetherFirstPort).
// Game thread only.
#pragma once
#include <string>
#include <vector>

namespace together {

void Frame();
// The next time the game opens its editor, it opens as a listen server on `port` (0: the game's own, 7777).
void ListenNextEditor(bool on);
bool ListenArmed();
void SetSandboxDriver(const std::string& driverClass);   // test: the GameNetDriver class a test copy uses
void SetReopenSuffix(const std::string& suffix);   // test: what an armed reopen adds ("listen"; "" for none)
// What this game is: "standalone", "listen server" or "client" (the world's net mode), and the editor's address
// the clients use.
std::string NetMode();
bool IsServer();                        // a listen server (not standalone)
bool IsClient();
// Leaves for an Unreal server at address "host:port" (a client travel; the server's editor loads).
bool Join(const std::string& address);
// Back to a game of its own: the main menu (as the editor's quit does).
bool Leave();
// Where clients reach this game's editor: "steam.<Steam id>" (the game's SteamSocketsNetDriver), or in a test copy
// "127.0.0.1:<its port>".
std::string OwnAddress();
int ListenPort();
// The game's own ways into its editor (each goes through ListenNextEditor when armed): a new map ("+new"), or one of
// its saved maps (a file in its UserSavedMaps folder, as the Create page's files list opens it).
bool NewMap();
bool OpenMap(const std::string& path);
std::vector<std::string> SavedMaps();   // the saved maps' full paths
int Players();
                          // players in this game's world (its game state's PlayerArray)                       // the port this game listens on (its -port, else 7777)
int ConnectedClients();                 // a listen server's client connections, -1 if not one
std::string Status();                   // for the test channel
void WatchGameInstance();
std::string HistoryStatus();            // test: each history's transactions (the world's, each player's own)               // test: logs the game instance's travel and network error events

}  // namespace together
