// Per-plugin saved values that survive relaunches: %LOCALAPPDATA%\Ballest\Saved\PluginManager\storage\<id>.txt,
// one "key=value" per line. Read on first use, rewritten on every change.
#pragma once
#include <string>

namespace storage {

std::string Get(const std::string& plugin, const std::string& key, const std::string& fallback);
void Set(const std::string& plugin, const std::string& key, const std::string& value);

}  // namespace storage
