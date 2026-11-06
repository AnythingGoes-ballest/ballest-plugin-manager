// Create Together: editing one map with other players, through the editor's own replication.
//
// The game's track editor is the SKG multiplayer level editor: its pieces and the editing pawn are replicated
// actors, and every change goes through the handler's Server_ functions. So an editor opened as an Unreal listen
// server is shared by every client that connects to it: the game itself spawns their pawns and replicates every
// piece, material and property. What the host adds:
//   * a listen server: the game opens its editor with OpenLevel(Map_LevelEditorMain, "game=...GM_LevelEditor_C");
//     while ListenNextEditor is armed, that travel is made again with "?listen" added (OpenLevelHooked);
//   * joining: an Unreal client travel to the host's address (Join);
//   * how players reach each other: the game's own net driver, SteamSocketsNetDriver (Steam's relay: no open port, no
//     address shown; ICE, Steam's direct path, is switched off), found through Steam lobbies (lobby.cpp). In a
//     sandboxed test copy Steam is blocked and two copies reach each other on loopback (sandbox.hpp: kTogetherFirstPort).
// Game thread only.
#pragma once
#include <string>
#include <vector>

namespace together {

void Frame();
// The next time the game opens its editor, it opens as a listen server (refused where Steam's networking isn't safe).
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
// Code-only sessions: the host's secret code (members who prove it are let in), and the code a joiner typed (proved to
// the host over the lobby). "" for none.
void SetSessionCode(const std::string& code);
std::string NewSessionCode();           // 8 random letters and digits (the system's random source), "" if none
bool JoinPending();                     // a Join has started and not yet arrived (Leave calls it off)
void SetJoinCode(const std::string& code);
// The last thing the host refused or ended for the player ("only the host can save this map", "the host left..."),
// given once; "" if none.
std::string TakeNotice();
// The plugin running the session (the last to host, make or join a lobby, or join); when it stops, the session ends:
// hosting disarmed, the lobby left, the editor left.
void SetOwner(int plugin);
void PluginStopped(int plugin);
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
std::string HistoryStatus();
std::string SteamIdsStatus();           // test: every player's Steam id as the host reads it            // test: each history's transactions (the world's, each player's own)               // test: logs the game instance's travel and network error events

}  // namespace together
