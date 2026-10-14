// The plugin manager window: the overhaul of the plugin manager's menu. The plugin manager plugin opens it from the
// footer (its Theme setting: Ballest Night, the default, Ballest Light or Lucid Lime; Classic keeps its own classic
// menu, whose cog opens this window's settings). One window over the dimmed game for everything:
//   top bar    the logo (refresh), settings (cog), the pages (plugins; cosmetics with Cosmetic Kit), update all,
//              folder and console (>_) keys, close.
//   plugins    installed / browse, compact or big rows, sections by category that fold, each page its own search.
//              Installed rows: on/off toggle, settings, icon, name, version, status, source, folder, remove; a click
//              opens the row downward with the details. Browse rows: get (with what it needs), source.
//   cosmetics  the plugins page's twin for cosmetic packs: owned / explore.
//   settings   an overlay over the page: the plugin manager's own (general, advanced) and each installed plugin's
//              settings. The console is one too. Esc or their key goes back.
//   welcome    the first open after an install or update: the logo's intro, then the window on plugins / installed.
//   popups     get with dependencies, remove (and forget its settings), reset.
//   banner     a newer plugin manager in the registry.
// Its own settings (size, scale, dim, hover sounds, views, folds) are kept in the plugin manager's storage. Drawn with
// pmui. Game thread only.
#pragma once
#include <string>

namespace manager {

// "" where it was; "plugins", "cosmetics", "settings", "console". Not over a run being raced (unpaused): it closes if
// one starts under it too.
void Open(const std::string& page = "");
void Close();
bool IsOpen();
bool TookInputThisFrame();                   // open now, or closed during this host frame (its closing key or click)
double HoverSoundLevel();                    // its hover sounds' level as set in general (0 when off; 1 = the default)
bool NewLook();                              // a theme other than Classic (the Plugin Manager plugin's Theme setting)
void Frame();                                // every frame, after ui::Frame (it sets the typing widget after windows)

}  // namespace manager
