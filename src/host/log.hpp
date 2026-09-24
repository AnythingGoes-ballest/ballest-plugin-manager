// Host log: %LOCALAPPDATA%\Ballest\Saved\PluginManager\host.log, recreated on every launch. The most recent lines
// are also kept in memory, numbered from the start of the session, for the in-game console.
#pragma once
#include <cstddef>
#include <string>

namespace hostlog {
void Open();
void Write(const char* level, const std::string& source, const std::string& message);
inline void Info(const std::string& m) { Write("info", "host", m); }
inline void Warn(const std::string& m) { Write("warn", "host", m); }
inline void Error(const std::string& m) { Write("error", "host", m); }
std::string Hex(unsigned long long v);
std::wstring DataDir();          // %LOCALAPPDATA%\Ballest\Saved\PluginManager

size_t LineCount();              // lines written this session
std::string Line(size_t index);  // one of the recent lines, or "" once it has been dropped from memory
}
