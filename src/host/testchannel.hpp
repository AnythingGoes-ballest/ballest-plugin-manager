// Test and measurement commands, from two sources: the in-game console (Console::Run in the script API) and the
// file %LOCALAPPDATA%\Ballest\Saved\PluginManager\test_command.txt, one command per write, read twice a second
// and used by tools/regression.py and tools/dev_session.py so the game can be driven and measured without a
// mouse, focus changes, or UE4SS. Results go to host.log.
//
//   state                                  UI and plugin status
//   click <label>[#n] | select <first option> <index> | slider <0..1> | press <virtual key> | submit [@<hint>|]<text>
//   fakereplay on [length] | fakereplay off | replaytime
//   install <id> | remove <id>             through the registry, as the plugin browser's buttons do
//   editor [rotatecontext on|off]         the track editor's selection with each piece's transform
//   setting <plugin id> <variable> <value> change a plugin's [Setting] as the settings view does
//   open <map>                             load a map directly (skips the menu's level setup)
//   functions <Class> | instances <Class> [fragment+fragment] | props <Class> [filter] | find <name fragment>
//   struct <Class> <Function> | call <Class> <Function> [filter] | viewtarget | pov <leaderboard entry filter>
//   watch <leaderboard entry filter> | materials [fragment] | replaycam <distance> <see-through 0|1>
//   crash [plugin id]                     test the fault guard: fault in host code now, or in that plugin's next call
//   callx <Class> <Function> [filter] | i:5 | f:1.5 | d:1.5 | u8:3 | b:1 | s:str | t:text | n:name | o:Class,filter | v:x,y,z
#pragma once
#include <string>

namespace testchannel {

void Frame();                               // game thread, every frame: runs queued commands, reads the file
void Enqueue(const std::string& command);   // run on the next Frame, outside any plugin's time budget

}  // namespace testchannel
