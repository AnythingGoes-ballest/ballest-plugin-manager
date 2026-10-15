// Cooked content a plugin ships in its assets/ folder (docs/custom-assets-recipe.md): mounted when the plugin starts,
// only after modcontainer::Verify proves it holds nothing but /Game/Mods/<mod>/ packages and the plugin's own shader
// library, past the game's pak signature check for that one container. Models use its materials through
// "material <name> asset /Game/Mods/<mod>/X", which accepts only materials and material instances. Nothing from a
// container is spawned or run: the host loads a material by path and makes a dynamic instance of it.
#pragma once
#include <cstdint>
#include <string>
#include "engine.hpp"

namespace assets {

// Mount an unsigned, unencrypted .pak + sibling .utoc/.ucas by absolute path, getting past the pak signature
// check for THIS container only (the game's own containers are mounted and validated at startup and untouched).
// Returns true if FPakPlatformFile::Mount succeeded. Game thread only.
bool MountContainer(const std::wstring& pakAbsolutePath, int32_t order = 1);

// FShaderCodeLibrary::OpenLibrary: opens a mounted container's own shader library by name, so its materials'
// cooked shaders are found. Use a mod-unique name, never the game's own "Ballest" or "Global".
bool OpenShaderLibrary(const std::wstring& name, const std::wstring& directory);

// Mount every not-yet-mounted container in a plugin's assets folder (oldest build first, each above the last): gate
// (only /Game/Mods/<id>/ packages and the plugin's own shader library), mount, open that build's shader library.
// Called when a plugin starts, so reloading a plugin picks up a newly built container. Returns how many it mounted.
int MountPluginAssets(const std::string& pluginId, const std::wstring& assetsDir);
// A model's "/Game/Mods/<mod>/X" as the newest mounted build's "/Game/Mods/<mod>/b<NNNN>/X" (unchanged otherwise).
std::wstring ResolveModPath(const std::wstring& path);
void CollectGarbage();   // frees objects nothing holds (lets a reloaded plugin load a newer build of a package)

}  // namespace assets
