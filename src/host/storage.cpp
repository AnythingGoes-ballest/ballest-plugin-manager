#include "storage.hpp"

#include <windows.h>

#include <fstream>
#include <map>
#include <sstream>

#include "engine.hpp"
#include "log.hpp"

namespace storage {
namespace {

using Values = std::map<std::string, std::string>;
std::map<std::string, Values> gPlugins;         // loaded files, by plugin id

std::wstring Dir() { return hostlog::DataDir() + L"\\storage"; }
std::wstring FileOf(const std::string& plugin) { return Dir() + L"\\" + eng::Widen(plugin) + L".txt"; }

// Keys and values are single lines; a key cannot contain '='.
std::string OneLine(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string Key(const std::string& key) {
    std::string k = OneLine(key);
    for (char& c : k)
        if (c == '=') c = '_';
    return k;
}

Values& Load(const std::string& plugin) {
    auto it = gPlugins.find(plugin);
    if (it != gPlugins.end()) return it->second;
    Values& values = gPlugins[plugin];
    std::ifstream in(FileOf(plugin).c_str());
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return values;
}

void Save(const std::string& plugin, const Values& values) {
    CreateDirectoryW(Dir().c_str(), nullptr);       // fails harmlessly when it exists
    std::ofstream out(FileOf(plugin).c_str(), std::ios::trunc);
    for (const auto& [name, saved] : values) out << name << '=' << saved << '\n';
    if (!out) hostlog::Warn("could not save storage for " + plugin);
}

}  // namespace

std::string Get(const std::string& plugin, const std::string& key, const std::string& fallback) {
    const Values& values = Load(plugin);
    const auto it = values.find(Key(key));
    return it == values.end() ? fallback : it->second;
}

void Set(const std::string& plugin, const std::string& key, const std::string& value) {
    Values& values = Load(plugin);
    const std::string k = Key(key), v = OneLine(value);
    if (values.count(k) && values[k] == v) return;
    values[k] = v;
    Save(plugin, values);
}

void Erase(const std::string& plugin, const std::string& key) {
    Values& values = Load(plugin);
    if (values.erase(Key(key))) Save(plugin, values);
}

bool Has(const std::string& plugin, const std::string& key) { return Load(plugin).count(Key(key)) > 0; }

}  // namespace storage
