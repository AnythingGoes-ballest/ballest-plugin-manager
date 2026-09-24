"""Maintains registry.json, the list of plugins the game can install.

    python tools/registry.py add <plugin repo folder> <tag> [--repo owner/name]
        Adds or updates the plugin's entry from its repo at that tag: name, version, description and so on from its
        info.toml, the tag's commit, and the SHA-256 of each file the game downloads (info.toml, the script files, the
        icon), hashed exactly as git stores them, which is what raw.githubusercontent.com serves. Push the tag first.

    python tools/registry.py host <tag> <path to version.dll>
        Points the registry's "host" entry at a release of this repo: the tag's commit, the DLL uploaded to that
        GitHub release (its SHA-256 from the file given), and the SHA-256 of every bundled plugin file at the tag.
        The game offers the update to anyone running an older host. Build the DLL from the tag, upload it to the
        release as version.dll, then run this and push registry.json.

    python tools/registry.py mirror <out folder> <plugin repo folder>... [--host-test VERSION --host-dll PATH]
        Writes a local copy of the registry and the files it points at, for testing before anything is published:
        put the printed file:/// URL in %LOCALAPPDATA%\\Ballest\\Saved\\PluginManager\\registry_url.txt. With
        --host-test, the copy also offers a host update to VERSION: the DLL given, and the bundled plugins as
        committed at HEAD.
"""
import argparse
import fnmatch
import hashlib
import json
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "registry.json"
DEFAULT_OWNER = "AnythingGoes-ballest"
HOST_REPO = f"{DEFAULT_OWNER}/ballest-plugin-manager"


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=True).stdout


def load():
    return json.loads(REGISTRY.read_text(encoding="utf-8")) if REGISTRY.exists() else {"plugins": []}


def save(registry):
    REGISTRY.write_text(json.dumps(registry, indent=2) + "\n", encoding="utf-8", newline="\n")


def files_of(manifest, repo, commit):
    """The files the game needs: info.toml, the scripts, the assets ([script] assets, wildcards allowed) and the icon
    if there is one."""
    meta, script = manifest.get("meta", {}), manifest.get("script", {})
    names = ["info.toml", *script.get("files", ["main.as"])]
    tree = set(git(repo, "ls-tree", "-r", "--name-only", commit).decode().splitlines())
    for pattern in script.get("assets", []):
        matched = sorted(n for n in tree if fnmatch.fnmatchcase(n, pattern))
        if not matched:
            sys.exit(f"assets pattern {pattern} matches nothing")
        names += [n for n in matched if n not in names]
    icon = meta.get("icon", "icon.png")
    if icon in tree:
        names.append(icon)
    missing = [n for n in names if n not in tree]
    if missing:
        sys.exit(f"not in the commit: {', '.join(missing)}")
    return names, (icon if icon in tree else "")


def add(path, tag, repo_name):
    repo = Path(path).resolve()
    commit = git(repo, "rev-parse", f"{tag}^{{commit}}").decode().strip()
    manifest = tomllib.loads(git(repo, "show", f"{commit}:info.toml").decode())
    meta = manifest.get("meta", {})
    if meta.get("version") and tag.lstrip("v") != meta["version"]:
        sys.exit(f"tag {tag} but info.toml says version {meta['version']}")
    names, icon = files_of(manifest, repo, commit)
    entry = {
        "id": repo.name.removeprefix("ballest-").removesuffix("-plugin") if "id" not in meta else meta["id"],
        "name": meta.get("name", repo.name),
        "description": meta.get("description", ""),
        "author": meta.get("author", ""),
        "repo": repo_name or f"{DEFAULT_OWNER}/{repo.name}",
        "version": meta.get("version", tag.lstrip("v")),
        "commit": commit,
        "min_host": meta.get("min_host", ""),
        "icon": icon,
        "dependencies": meta.get("dependencies", []),
        "files": {n: hashlib.sha256(git(repo, "show", f"{commit}:{n}")).hexdigest() for n in names},
    }
    registry = load()
    plugins = [p for p in registry["plugins"] if p["id"] != entry["id"]]
    plugins.append(entry)
    registry["plugins"] = sorted(plugins, key=lambda p: p["name"].lower())
    save(registry)
    print(f"{entry['id']} {entry['version']} at {commit[:12]} ({len(names)} files) -> {REGISTRY.name}")


def bundled_files(commit):
    """Every file of the bundled plugins at a commit of this repo, with its SHA-256."""
    names = git(ROOT, "ls-tree", "-r", "--name-only", commit, "--", "plugins/").decode().split()
    return {n: hashlib.sha256(git(ROOT, "show", f"{commit}:{n}")).hexdigest() for n in names}


def host(tag, dll):
    commit = git(ROOT, "rev-parse", f"{tag}^{{commit}}").decode().strip()
    source = git(ROOT, "show", f"{commit}:src/host/plugins.hpp").decode()
    version = tag.lstrip("v")
    if f'kHostVersion = "{version}"' not in source:
        sys.exit(f"{tag}: src/host/plugins.hpp at that commit does not have kHostVersion = {version}")
    registry = load()
    registry["host"] = {
        "version": version,
        "repo": HOST_REPO,
        "commit": commit,
        "dll": f"https://github.com/{HOST_REPO}/releases/download/{tag}/version.dll",
        "dll_sha256": hashlib.sha256(Path(dll).read_bytes()).hexdigest(),
        "files": bundled_files(commit),
    }
    save(registry)
    print(f"host {version} at {commit[:12]} ({len(registry['host']['files'])} bundled files) -> {REGISTRY.name}")


def mirror(out, paths, host_test=None, host_dll=None):
    out = Path(out).resolve()
    registry = load()
    by_folder = {Path(p).resolve().name: Path(p).resolve() for p in paths}
    for entry in registry["plugins"]:
        repo = by_folder.get(entry["repo"].split("/")[1])
        if not repo:
            continue
        target = out / entry["repo"] / entry["commit"]
        target.mkdir(parents=True, exist_ok=True)
        for name in entry["files"]:
            (target / name).parent.mkdir(parents=True, exist_ok=True)
            (target / name).write_bytes(git(repo, "show", f"{entry['commit']}:{name}"))
    local = dict(registry, raw_base=out.as_uri() + "/")
    if host_test:
        # A pretend newer host: the DLL given, and the bundled plugins as committed at HEAD.
        commit = git(ROOT, "rev-parse", "HEAD").decode().strip()
        (out / "host").mkdir(parents=True, exist_ok=True)
        (out / "host" / "version.dll").write_bytes(Path(host_dll).read_bytes())
        files = bundled_files(commit)
        for name in files:
            target = out / HOST_REPO / commit / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(git(ROOT, "show", f"{commit}:{name}"))
        local["host"] = {"version": host_test, "repo": HOST_REPO, "commit": commit,
                         "dll": (out / "host" / "version.dll").as_uri(),
                         "dll_sha256": hashlib.sha256(Path(host_dll).read_bytes()).hexdigest(), "files": files}
    (out / "registry.json").write_text(json.dumps(local, indent=2) + "\n", encoding="utf-8")
    print((out / "registry.json").as_uri())


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="command", required=True)
    a = sub.add_parser("add")
    a.add_argument("path")
    a.add_argument("tag")
    a.add_argument("--repo", help="owner/name on GitHub (default: %s/<folder name>)" % DEFAULT_OWNER)
    h = sub.add_parser("host")
    h.add_argument("tag")
    h.add_argument("dll")
    m = sub.add_parser("mirror")
    m.add_argument("out")
    m.add_argument("paths", nargs="+")
    m.add_argument("--host-test", help="also offer a host update to this version")
    m.add_argument("--host-dll", help="the DLL that update installs")
    args = ap.parse_args()
    if args.command == "add":
        add(args.path, args.tag, args.repo)
    elif args.command == "host":
        host(args.tag, args.dll)
    else:
        mirror(args.out, args.paths, args.host_test, args.host_dll)


if __name__ == "__main__":
    main()
