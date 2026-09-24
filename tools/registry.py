"""Maintains registry.json, the list of plugins the game can install.

    python tools/registry.py add <plugin repo folder> <tag> [--repo owner/name]
        Adds or updates the plugin's entry from its repo at that tag: name, version, description and so on from its
        info.toml, the tag's commit, and the SHA-256 of each file the game downloads (info.toml, the script files, the
        icon), hashed exactly as git stores them, which is what raw.githubusercontent.com serves. Push the tag first.

    python tools/registry.py mirror <out folder> <plugin repo folder>...
        Writes a local copy of the registry and the files it points at, for testing before anything is published:
        put the printed file:/// URL in %LOCALAPPDATA%\\Ballest\\Saved\\PluginManager\\registry_url.txt.
"""
import argparse
import hashlib
import json
import subprocess
import sys
import tomllib
from pathlib import Path

REGISTRY = Path(__file__).resolve().parents[1] / "registry.json"
DEFAULT_OWNER = "AnythingGoes-ballest"


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=True).stdout


def load():
    return json.loads(REGISTRY.read_text(encoding="utf-8")) if REGISTRY.exists() else {"plugins": []}


def save(registry):
    REGISTRY.write_text(json.dumps(registry, indent=2) + "\n", encoding="utf-8", newline="\n")


def files_of(manifest, repo, commit):
    """The files the game needs: info.toml, the scripts, and the icon if there is one."""
    meta, script = manifest.get("meta", {}), manifest.get("script", {})
    names = ["info.toml", *script.get("files", ["main.as"])]
    tree = set(git(repo, "ls-tree", "--name-only", commit).decode().split())
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
        "files": {n: hashlib.sha256(git(repo, "show", f"{commit}:{n}")).hexdigest() for n in names},
    }
    registry = load()
    plugins = [p for p in registry["plugins"] if p["id"] != entry["id"]]
    plugins.append(entry)
    registry["plugins"] = sorted(plugins, key=lambda p: p["name"].lower())
    save(registry)
    print(f"{entry['id']} {entry['version']} at {commit[:12]} ({len(names)} files) -> {REGISTRY.name}")


def mirror(out, paths):
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
            (target / name).write_bytes(git(repo, "show", f"{entry['commit']}:{name}"))
    local = dict(registry, raw_base=out.as_uri() + "/")
    (out / "registry.json").write_text(json.dumps(local, indent=2) + "\n", encoding="utf-8")
    print((out / "registry.json").as_uri())


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="command", required=True)
    a = sub.add_parser("add")
    a.add_argument("path")
    a.add_argument("tag")
    a.add_argument("--repo", help="owner/name on GitHub (default: %s/<folder name>)" % DEFAULT_OWNER)
    m = sub.add_parser("mirror")
    m.add_argument("out")
    m.add_argument("paths", nargs="+")
    args = ap.parse_args()
    if args.command == "add":
        add(args.path, args.tag, args.repo)
    else:
        mirror(args.out, args.paths)


if __name__ == "__main__":
    main()
