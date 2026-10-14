"""Maintains registry.json, the list of plugins the game can install.

    python tools/registry.py add <plugin repo folder> <tag> [--repo owner/name] [--path folder/in/repo]
        Adds or updates the plugin's entry from its repo at that tag: name, version, description and so on from its
        info.toml, the tag's commit, and the SHA-256 of each file the game downloads (info.toml, the script files, the
        icon), hashed exactly as git stores them, which is what raw.githubusercontent.com serves. Push the tag first.
        For a repo that holds several plugins, --path names the plugin's folder in it (plugins/grind-stats); the tag
        is then the plugin's id and version (grind-stats-v0.1.0), and the entry needs a host that reads "path".

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
PATH_MIN_HOST = "0.12.0"        # the first host that reads an entry's "path"


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=True).stdout


def load():
    return json.loads(REGISTRY.read_text(encoding="utf-8")) if REGISTRY.exists() else {"plugins": []}


def save(registry):
    REGISTRY.write_text(json.dumps(registry, indent=2) + "\n", encoding="utf-8", newline="\n")


def in_repo(path, name):
    """Where a plugin file is in its repo: under the plugin's folder when it has one."""
    return f"{path}/{name}" if path else name


def version_of(tag):
    """The version a tag names: v0.1.0, or grind-stats-v0.1.0 for one plugin of several in a repo."""
    return tag[1:] if tag.startswith("v") else tag.rpartition("-v")[2]


def newer(a, b):
    return tuple(int(n) for n in a.split(".")) > tuple(int(n) for n in b.split("."))


def files_of(manifest, repo, commit, path=""):
    """The files the game needs: info.toml, the scripts, the assets ([script] assets, wildcards allowed) and the icon
    if there is one. Names are relative to the plugin's folder (path)."""
    meta, script = manifest.get("meta", {}), manifest.get("script", {})
    names = ["info.toml", *script.get("files", ["main.as"])]
    listed = git(repo, "ls-tree", "-r", "--name-only", commit, *(["--", path + "/"] if path else [])).decode()
    tree = {n[len(path) + 1:] if path else n for n in listed.splitlines()}
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


def add(path, tag, repo_name, folder=""):
    repo = Path(path).resolve()
    folder = folder.strip("/")
    commit = git(repo, "rev-parse", f"{tag}^{{commit}}").decode().strip()
    manifest = tomllib.loads(git(repo, "show", f"{commit}:{in_repo(folder, 'info.toml')}").decode())
    meta = manifest.get("meta", {})
    if meta.get("version") and version_of(tag) != meta["version"]:
        sys.exit(f"tag {tag} but info.toml says version {meta['version']}")
    names, icon = files_of(manifest, repo, commit, folder)
    default_id = folder.rpartition("/")[2] if folder else repo.name.removeprefix("ballest-").removesuffix("-plugin")
    min_host = meta.get("min_host", "")
    if folder and (not min_host or newer(PATH_MIN_HOST, min_host)):
        min_host = PATH_MIN_HOST        # an older host would download from the repo's root
    entry = {
        "id": meta.get("id", default_id),
        "name": meta.get("name", folder.rpartition("/")[2] or repo.name),
        "description": meta.get("description", ""),
        "author": meta.get("author", ""),
        "repo": repo_name or f"{DEFAULT_OWNER}/{repo.name}",
        **({"path": folder} if folder else {}),
        "version": meta.get("version", version_of(tag)),
        "commit": commit,
        "min_host": min_host,
        "icon": icon,
        "dependencies": meta.get("dependencies", []),
        "files": {n: hashlib.sha256(git(repo, "show", f"{commit}:{in_repo(folder, n)}")).hexdigest() for n in names},
    }
    # The review rules (CLAUDE.md, tools/review_guard.py): no built binaries, no Console:: in its scripts.
    from review_guard import plugin_problems
    downloaded = {n: git(repo, "show", f"{commit}:{in_repo(folder, n)}") for n in names}
    problems = plugin_problems(entry["id"], commit, downloaded)
    if problems:
        sys.exit("refused by the review rules (see CLAUDE.md):\n  " + "\n  ".join(problems))
    registry = load()
    # where the plugin manager lists it, set by hand in registry.json, kept through an update (else from its info.toml)
    previous = next((p for p in registry["plugins"] if p["id"] == entry["id"]), {})
    for key in ("category", "priority", "added", "listed"):
        value = previous.get(key, meta.get(key))
        if value is not None:
            entry[key] = value
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
        folder = entry.get("path", "")
        target = out / entry["repo"] / entry["commit"] / folder
        target.mkdir(parents=True, exist_ok=True)
        for name in entry["files"]:
            (target / name).parent.mkdir(parents=True, exist_ok=True)
            (target / name).write_bytes(git(repo, "show", f"{entry['commit']}:{in_repo(folder, name)}"))
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
    a.add_argument("--path", dest="folder", default="", help="the plugin's folder in the repo, for a repo with several plugins")
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
        add(args.path, args.tag, args.repo, args.folder)
    elif args.command == "host":
        host(args.tag, args.dll)
    else:
        mirror(args.out, args.paths, args.host_test, args.host_dll)


if __name__ == "__main__":
    main()
