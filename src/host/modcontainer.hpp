// Security gate for mod IoStore containers; see modcontainer.cpp.
#pragma once
#include <string>

namespace modcontainer {

// True if the .utoc ships only packages under /Game/Mods/<pluginId>/ (plus its own ShaderArchive-*.ushaderbytecode
// and *.json), proven from both the directory index and the container's package ids. On false, `error` says why.
// Reads only the .utoc. A plugin container that passes this cannot shadow a game or engine package.
// `shaderLibrary` (optional) receives the container's shader-library name ("<id>" or "<id>_<suffix>"; empty if none).
bool Verify(const std::wstring& utocPath, const std::string& pluginId, std::string& error,
            std::string* shaderLibrary = nullptr);

}  // namespace modcontainer
