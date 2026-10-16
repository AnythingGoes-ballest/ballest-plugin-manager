# Run by make_mod_container.py through the editor's pythonscript commandlet, on a scratch copy of a mod's content
# project: moves every asset under /Game/Mods/<MOD>/ into /Game/Mods/<MOD>/<BUILD>/ with the editor's own rename, so
# references between them (a material instance's parent, a mesh's materials) point at the moved copies.
import os
import unreal

mod = os.environ["MOD_NAME"]
build = os.environ["MOD_BUILD"]                     # "b0007"
root = f"/Game/Mods/{mod}"
dest = f"{root}/{build}"
lib = unreal.EditorAssetLibrary

# A plugin container holds looks only: materials and what they sample. No Blueprints, maps, meshes (collision),
# sounds or particle systems, so nothing in it can run code or change gameplay. The host also applies only materials.
ALLOWED = {"Material", "MaterialInstanceConstant", "MaterialFunction", "MaterialFunctionMaterialLayer",
           "MaterialFunctionMaterialLayerBlend", "MaterialParameterCollection", "SubsurfaceProfile",
           "Texture2D", "TextureCube", "VolumeTexture", "Texture2DArray"}

moves, refused = [], []
for path in lib.list_assets(root, recursive=True, include_folder=False):
    package = path.split(".")[0]
    rel = package[len(root) + 1:]
    if rel.startswith(build + "/"):
        continue
    folder, name = os.path.split(rel)
    asset = lib.load_asset(package)
    if not asset:
        continue
    cls = asset.get_class().get_name()
    if cls not in ALLOWED:
        refused.append(f"{package} ({cls})")
        continue
    moves.append(unreal.AssetRenameData(asset, dest + ("/" + folder if folder else ""), name))
if refused:
    raise RuntimeError("MOD_BUILD_REFUSED: only materials and textures go in a plugin container: " + ", ".join(refused))
ok = unreal.AssetToolsHelpers.get_asset_tools().rename_assets(moves)
lib.save_directory(dest, only_if_is_dirty=False, recursive=True)
print(f"MOD_BUILD_FOLDER moved={len(moves)} ok={ok}")
