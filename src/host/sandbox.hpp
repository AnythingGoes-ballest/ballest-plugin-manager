// Sandbox mode: a session where nothing the game does can leave this computer or change the player's records, for
// test copies of the game (tools/test_instance.py) and AI drivers. It is on for the whole session when sandbox.txt
// (or its older name, ai_mode.txt) is in the host's data folder (%LOCALAPPDATA%\Ballest\Saved\PluginManager, where
// LOCALAPPDATA is this process's own variable) at launch, and is never turned off mid-session.
//
// What it blocks, and where (each block is logged once per kind, then counted):
//   * the internet: every host name lookup the game makes (its HTTP and Socket.IO clients resolve names with
//     WS2_32 getaddrinfo, which the game exe imports; WinHTTP is imported only for proxy settings) fails, so the
//     game's backend (leaderboard runs, telemetry, competitions, daily challenge) and multiplayer can't connect;
//   * the player's records on disk: opening anything under Saved\SaveGames or Saved\Ghosts for writing fails
//     (KERNEL32 CreateFileW, imported by the game exe), so times, medals and ghosts aren't saved;
//   * Steam: the calls that upload or change anything are answered "failed" without reaching Steam: leaderboard
//     scores and their attached replays, stats and achievements, cloud files, workshop items, votes, playtime.
//     Steam's interfaces are C++ objects; their methods are replaced in each object's own table. The slot of each
//     method is read from the game's steam_api64.dll flat wrapper (mov rax,[rcx]; jmp [rax+offset]), not assumed.
// Both import hooks are installed from DllMain, before the game's own code runs; the Steam ones as soon as Steam's
// interfaces exist (polled from the init thread).
#pragma once
#include <string>

namespace sandbox {

bool On();                      // sandbox mode is on for this session
bool Flagged();                 // the flag is there (before InstallEarly has read it)
bool Interactive();             // a copy the player uses (interactive.txt): normal window and sound, every block kept
std::wstring OwnHostPath();     // a test copy's own host build, run in place of the installed one when it exists
void InstallEarly();            // from DllMain: reads the flag, hooks the imports
bool InstallSteam();            // until true: hooks the Steam interfaces once they exist
std::string Status();           // what is installed and what was blocked so far
bool Complete();                // on, and every block verified in place in the game's own interfaces (Status says so)
// The copy's sound is muted in Windows (its own audio session), so plugins' sounds still play, only nobody hears
// them; the volume mixer shows it muted. Called again once the game has started its audio.
bool MuteAudio();

// For the test channel: the calls the game would make, through the same paths it uses, so the blocks can be checked.
std::string SelfTest();

}  // namespace sandbox
