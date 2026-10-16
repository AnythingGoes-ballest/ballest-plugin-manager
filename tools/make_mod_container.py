"""Cook a plugin's content project and package only its own content into the plugin's assets/ folder.

Usage:
    python tools/make_mod_container.py <project .uproject> <plugin assets dir> [--engine <UE_5.8 root>] [--stage]

The project must be named after the plugin id and keep its content under /Game/Mods/<id>/ (docs/custom-assets-recipe.md).
Each run is a new build: the container is named <id>_b<NNNN>-Windows and its shader library <id>_b<NNNN>, because the
game de-duplicates IoStore shader libraries by name. Older builds the game no longer holds open are deleted.

Default (fast): the editor's cook commandlet, then a container listing the cooked /Game/Mods/<id>/ packages and the
build's shader library. That covers materials and material instances. Use --stage for content with bulk data (textures,
meshes): it runs BuildCookRun -stage and filters UE's own staging file list instead.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time

DEFAULT_ENGINE = r"C:\Program Files\Epic Games\UE_5.8"


def run(cmd, label, env=None):
    t = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", env=env)
    out = p.stdout + p.stderr
    if p.returncode != 0:
        print(out[-4000:])
        sys.exit(f"{label} failed ({p.returncode})")
    print(f"  {label}: {time.time() - t:.1f} s")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("project")
    ap.add_argument("assets_dir")
    ap.add_argument("--engine", default=DEFAULT_ENGINE)
    ap.add_argument("--stage", action="store_true")
    a = ap.parse_args()

    project = os.path.abspath(a.project)
    proj_dir = os.path.dirname(project)
    proj = os.path.splitext(os.path.basename(project))[0]            # = the plugin id
    out_dir = os.path.abspath(a.assets_dir)
    os.makedirs(out_dir, exist_ok=True)
    editor = os.path.join(a.engine, r"Engine\Binaries\Win64\UnrealEditor-Cmd.exe")
    unrealpak = os.path.join(a.engine, r"Engine\Binaries\Win64\UnrealPak.exe")
    uat = os.path.join(a.engine, r"Engine\Build\BatchFiles\RunUAT.bat")

    builds = [int(m.group(1)) for f in os.listdir(out_dir) if (m := re.match(re.escape(proj) + r"_b(\d+)-Windows\.utoc$", f))]
    build = max(builds, default=0) + 1
    name = f"{proj}_b{build:04d}"
    container = f"{name}-Windows"
    print(f"{proj}: build {build} -> {container}")
    t0 = time.time()

    # 0. A scratch copy of the project in which the mod's content moves to /Game/Mods/<id>/b<NNNN>/: each build is a set
    #    of NEW packages, so a running game loads it fresh (the host maps "/Game/Mods/<id>/X" to the newest build).
    build_dir = os.path.join(proj_dir, "Saved", "ModBuild", proj)
    shutil.rmtree(os.path.join(build_dir, "Content"), ignore_errors=True)
    os.makedirs(build_dir, exist_ok=True)
    shutil.copy2(project, os.path.join(build_dir, proj + ".uproject"))
    shutil.copytree(os.path.join(proj_dir, "Config"), os.path.join(build_dir, "Config"), dirs_exist_ok=True)
    shutil.copytree(os.path.join(proj_dir, "Content"), os.path.join(build_dir, "Content"))
    build_project = os.path.join(build_dir, proj + ".uproject")
    folder = f"b{build:04d}"
    env = dict(os.environ, MOD_NAME=proj, MOD_BUILD=folder)
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mod_build_folder.py").replace(os.sep, "/")
    # One string, quoted by hand: UE reads -script="..." only with the quote right after "=" (paths may hold spaces).
    run(f'"{editor}" "{build_project}" -run=pythonscript -script="{script}" -unattended -nop4 -nosplash',
        "move to " + folder, env)
    mods = os.path.join(build_dir, "Content", "Mods", proj)
    for f in glob.glob(os.path.join(mods, "**", "*.uasset"), recursive=True):     # redirectors left at the old paths
        if not os.path.relpath(f, mods).replace("\\", "/").startswith(folder + "/"):
            os.remove(f)
    project, proj_dir = build_project, build_dir
    cooked = os.path.join(proj_dir, "Saved", "Cooked", "Windows")

    # 1. Cook, with this build's shader-library name.
    if a.stage:
        run([uat, "BuildCookRun", f"-project={project}", "-noP4", "-platform=Win64", "-clientconfig=Shipping", "-cook",
             "-stage", "-pak", "-iostore", "-unattended", "-nocompileeditor", "-nocompile",
             f"-AdditionalCookerOptions=-OverrideShaderLibraryName={name}"], "cook + stage")
        resp_src = os.path.join(a.engine, r"Engine\Programs\AutomationTool\Saved\ResponseFiles", f"PakListIoStore_{proj}-Windows.txt")
        lines = open(resp_src, encoding="utf-8-sig").read().splitlines()
        keep = [l for l in lines if f"/Content/Mods/{proj}/{folder}/" in l.replace("\\", "/") or f"ShaderArchive-{name}-" in l]
    else:
        run([editor, project, "-run=cook", "-targetplatform=Windows", "-unattended", "-nop4", "-nosplash",
             f"-OverrideShaderLibraryName={name}"], "cook")
        keep = []
        content = os.path.join(proj_dir, "Content")
        for f in glob.glob(os.path.join(content, "Mods", proj, folder, "**", "*.*"), recursive=True):
            if os.path.splitext(f)[1].lower() not in (".uasset", ".umap"):
                continue
            rel = os.path.relpath(f, content).replace("\\", "/")
            keep.append(f'"{os.path.join(cooked, proj, "Content", rel)}" "../../../{proj}/Content/{rel}" -compress')
        for f in glob.glob(os.path.join(cooked, proj, "Content", f"ShaderArchive-{name}-*.ushaderbytecode")):
            keep.append(f'"{f}" "../../../{proj}/Content/{os.path.basename(f)}"')
    if not any("ShaderArchive-" in l for l in keep):
        print("  note: no shader library in this build (no new shaders cooked)")
    if not any("/Content/Mods/" in l.replace("\\", "/") for l in keep):
        sys.exit("no /Game/Mods content cooked")

    # 2. Package only those files with UE's IoStore packager.
    work = os.path.join(proj_dir, "Saved", "ModContainer")
    os.makedirs(work, exist_ok=True)
    resp = os.path.join(work, "files.txt")
    open(resp, "w", encoding="utf-8").write("\n".join(keep) + "\n")
    cmds = os.path.join(work, "commands.txt")
    open(cmds, "w", encoding="utf-8").write(
        f'-Output="{os.path.join(out_dir, container + ".utoc")}" -ContainerName={container} -ResponseFile="{resp}"\n')
    crypto = os.path.join(cooked, proj, "Metadata", "Crypto.json")    # written by UAT staging only
    run([unrealpak, project, f"-CreateGlobalContainer={os.path.join(work, 'global.utoc')}",
         f"-ProjectStore={os.path.join(cooked, 'ue.projectstore')}", f"-CookedDirectory={cooked}", f"-Commands={cmds}",
         "-compressionformats=Oodle", "-compressmethod=Kraken", "-platform=Windows", "-unattended"]
        + ([f"-cryptokeys={crypto}"] if os.path.exists(crypto) else []),
        "package")

    # 3. The .pak the game's mount needs beside the container (one small text file).
    note = os.path.join(work, "container.txt")
    open(note, "w").write(f"{container}\n")
    pak_resp = os.path.join(work, "pak.txt")
    open(pak_resp, "w").write(f'"{note}" "../../../{proj}/Content/Mods/{proj}/{container}.txt"\n')
    run([unrealpak, os.path.join(out_dir, container + ".pak"), f"-create={pak_resp}"], "pak")

    # 4. Remove older builds the game isn't holding open.
    for f in os.listdir(out_dir):
        m = re.match(re.escape(proj) + r"_b(\d+)-Windows\.(utoc|ucas|pak)$", f)
        if m and int(m.group(1)) < build:
            try:
                os.remove(os.path.join(out_dir, f))
            except OSError:
                pass                        # still mounted in a running game: removed next time
    size = sum(os.path.getsize(os.path.join(out_dir, container + e)) for e in (".utoc", ".ucas", ".pak"))
    print(f"{container}: {size / 1e6:.2f} MB in {out_dir}  ({time.time() - t0:.1f} s total)")


if __name__ == "__main__":
    main()
