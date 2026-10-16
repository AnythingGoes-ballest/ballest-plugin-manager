# Custom cooked assets for plugins

Host 0.27.0 and newer. Everything here was measured in a sandboxed test copy of the game.
A plugin ships cooked UE 5.8 content (new materials with their own shaders, material instances, and so on) in its
`assets/` folder. The host gates it, mounts it, opens its shader library, and a model file uses it like a game material.

## 1. Engine
- UE **5.8.3** from the Epic launcher (CL 58210709). The game is 5.8.2 (CL 56702186), which is compatible.

## 2. The content project
- Name the project after the plugin's **mod name**: the plugin id with `-` changed to `_`. For example, plugin
  `elemental-balls` uses project `elemental_balls.uproject`.
- Put all mod content under **`/Game/Mods/<mod>/`**. The host refuses anything else.
- `.uproject`: set `"EngineAssociation": "5.8"`. `PythonScriptPlugin` is only needed for scripted assets.
- `Config/DefaultEngine.ini` (the game renders with DX12, Shader Model 6):
  ```ini
  [/Script/WindowsTargetPlatform.WindowsTargetSettings]
  DefaultGraphicsRHI=DefaultGraphicsRHI_DX12
  -D3D12TargetedShaderFormats=PCD3D_SM5
  +D3D12TargetedShaderFormats=PCD3D_SM6
  -D3D11TargetedShaderFormats=PCD3D_SM5
  ```
- `Config/DefaultGame.ini`:
  ```ini
  [/Script/EngineSettings.GeneralProjectSettings]
  ProjectName=<mod>
  [/Script/UnrealEd.ProjectPackagingSettings]
  UsePakFile=True
  bUseIoStore=True
  bShareMaterialShaderCode=True
  +DirectoriesToAlwaysCook=(Path="/Game/Mods")
  ```
- Leave signing and encryption off. The host gets past the game's signature check for its own container only.

## 3. Build into the plugin: one command
```
python tools/make_mod_container.py <project>\<mod>.uproject <plugin folder>\assets
```
Each run is a new **build N**. The steps and their measured times:

| Step | What it does | Time |
|---|---|---|
| Scratch copy | Copies the project to `Saved/ModBuild/<mod>/` and, in the copy, moves the mod's content into `/Game/Mods/<mod>/b<NNNN>/` with the editor's reference-fixing rename (`tools/mod_build_folder.py`). Your project is untouched. | ~9 s |
| Cook | Editor cook commandlet with `-OverrideShaderLibraryName=<mod>_b<NNNN>` | ~35-45 s |
| Package | UE's IoStore packager on only the build's packages and its shader library, into `assets/<mod>_b<NNNN>-Windows.utoc/.ucas`, plus a tiny `.pak` | ~3 s |
| Clean up | Deletes older builds the game isn't holding open | — |

- Size: about 16.7 MB for the test material. Nearly all of it is the material's compiled shaders.
- Fast mode covers materials and material instances.
  - Use `--stage` for content with bulk data (textures, meshes). It runs `BuildCookRun -stage` and filters UE's own
    staging file list. Untested end to end.
- Why every build gets new names:
  - IoStore shader libraries are de-duplicated by name. A rebuilt library with an old name would be ignored.
  - A loaded package is reused, and the host keeps its cosmetics' objects alive. A rebuilt package with an old path
    would never be seen.

## 4. The plugin
```
<plugin>/
  info.toml          (the container is not listed: it comes from the release, see 3.)
  main.as
  models/ball.txt
  assets/<mod>_b0001-Windows.utoc / .ucas / .pak     (from make_mod_container.py)
```
The model file names the material by its **unversioned** path. The host maps it to the newest mounted build:
```
ball hidden
material shell asset /Game/Mods/<mod>/MI_Shell.MI_Shell
set shell "Tint" #00ffff          # set / animate / race work as for "material <name> game <path>"
sphere shell r=49 at=0,0,0        # or: mesh sphere.glb ... material=shell
```
A `material <name> asset` path must start with `/Game/Mods/`. Add the ball with Cosmetic Kit's `AddBall` as usual. It
shows in Customize and in races.

## 5. What the host does (src/host/assets.cpp, modcontainer.cpp, plugins.cpp, models.cpp)
When a plugin starts (and on every reload), for each `.utoc` in its `assets/` not yet mounted, oldest first:
1. **Gate** (`modcontainer::Verify`, reads only the `.utoc`). It refuses unless all of these hold:
   - every file is a `.uasset`/`.umap` under `Mods/<mod>/`, or the build's own `ShaderArchive-<mod>[_*]-....ushaderbytecode`;
   - every package id the container serves is the CityHash64 of one of those paths. A crafted container can't shadow a
     game asset such as `/Game/Core/SM_PlayerBall`.
   - Measured: a container refused under the wrong id, accepted under its own.
2. Mounts it (`FPakPlatformFile::HandleMountPakDelegate`).
   - The signing-keys delegate and `bSigned` are cleared only around this one call; the game's own content is untouched.
   - Each newer build goes at a higher order.
3. Opens that build's shader library (`FShaderCodeLibrary::OpenLibrary`, RVA 0x3308570, bytes checked before calling).
4. `material <n> asset /Game/Mods/<mod>/X` then loads `/Game/Mods/<mod>/b<newest>/X`.

The addresses are measured on Ballest CL 56702186. After a game update they must be found again; the host checks the
bytes and refuses rather than calling a wrong address.

## 6. Iterating on a shader in the running game
1. Edit the material in the content project and save.
2. Run `python tools/make_mod_container.py <project> <slot's plugin folder>\assets`.
   - The slot's copy is `%LOCALAPPDATA%\BallestTest<N>\Ballest\Saved\PluginManager\plugins\<id>\assets`.
3. Send these test commands: `assetreload <plugin id>`, then `wear ball -`, then `wear ball <ball id>`.

Measured: a **new shader graph** (flat
magenta emissive → cyan fresnel rim) on the ball without restarting.

| Stage | Time |
|---|---|
| Edit the assets (scripted) | 8.6 s |
| Cook + package | 57.1 s |
| Reload, re-wear, screenshot | 23.1 s (mostly the test tool's ~3 s per command; the reload itself takes under 1 ms) |
| **Total** | **88.8 s** |

The cook dominates. Every build stays mounted until the game closes, at about 17 MB of memory per build (fine for a dev session).

## 7. Safety and TODO
- **Done:** the gate. A plugin's container can only add `/Game/Mods/<mod>/` packages and its own shader library.
- **TODO, not built:** cosmetic-only enforcement. A container could hold a mesh with collision, or a material with
  world-position offset. Before release: taint runs while a plugin's container holding anything but materials, material
  instances and textures is mounted, and/or allow containers only on the player's own never-published maps.
- **TODO:** the registry's `review_guard.py` refuses `.pak` and other packed files from contributors. Shipping containers
  through the public registry needs a policy decision: for example, maintainers build containers from a reviewed content
  project, as with `version.dll`.
- **Shelf tiles:** a custom ball's Customize tile uses the plugin's preview picture (the `AddBall` preview argument), not a
  live render. Give it a preview PNG. Not checked beyond that.
- `GetMaterial(0)` reads null on some shelf `BP_LBall05` instances: their look is drawn by a skin child actor. Harmless.
