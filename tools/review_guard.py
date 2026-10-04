"""The review rules every outside change and every registry plugin has to pass (see CLAUDE.md):

  * No built binaries. A .dll, .exe, archive or any other compiled file can hold different code from the source next to
    it, and nobody can review it. Releases are built by us from reviewed source; the in-game updater sends version.dll
    to every player.
  * No Console:: in plugins. Console::Run reaches the host's console and test commands: any game function (callx),
    moving the ball, posting key presses, installing or turning off plugins. That gets around everything the API is
    meant to allow. Our own bundled plugin manager (its console box) and API tests are the only users.

    python tools/review_guard.py changed <base ref>   the files and added lines of a pull request (CI)
    python tools/review_guard.py registry             every plugin in registry.json, as pinned (downloads each file)

Exits 1 with the problems listed. tools/registry.py add runs the same check before it adds or updates a plugin.
"""
import json
import subprocess
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Files that are binary but are data, not code: pictures, sounds, 3D models, fonts.
DATA_EXTENSIONS = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".ico", ".svg", ".wav", ".ogg", ".mp3", ".glb", ".gltf",
                   ".bin", ".obj", ".mtl", ".ttf", ".otf"}
# Built or packed files: never from anyone else, whatever is inside.
BINARY_EXTENSIONS = {".dll", ".exe", ".sys", ".so", ".dylib", ".lib", ".a", ".o", ".msi", ".scr", ".com",
                     ".zip", ".7z", ".rar", ".tar", ".gz", ".pak", ".utoc", ".ucas", ".pyd", ".jar"}
# Where Console:: belongs: our own API tests and the plugin manager's console box.
CONSOLE_ALLOWED_PATHS = ("tools/api-tests/", "plugins/plugin-manager/")
# Registry plugins let through as they are, pinned to one commit: an update with Console:: is refused. Each needs a
# reason and a link.
EXCEPTIONS = {
    ("practice-checkpoints", "70b302d"): "Practice Checkpoints 0.3.0 predates the rule; Pedro has been asked to move "
                                         "off Console:: (https://github.com/Pedro-Brito-09/ballest-plugins/issues/1).",
}


def suffix(name):
    return Path(name.lower()).suffix


# Console::Run however it's spelled: spaces around the ::, or the namespace opened (using namespace Console).
CONSOLE = re.compile(rb"\bConsole\s*::|\bnamespace\s+Console\b")


def file_problems(name, data):
    """What's wrong with one file a plugin or a pull request brings in."""
    ext = suffix(name)
    if ext in BINARY_EXTENSIONS:
        return [f"{name}: a built or packed file ({ext}); only reviewable source is accepted"]
    if ext not in DATA_EXTENSIONS and b"\0" in data[:65536]:
        return [f"{name}: binary content in a file that should be text"]
    if ext == ".as" and CONSOLE.search(data):
        return [f"{name}: uses Console:: ({len(CONSOLE.findall(data))}x), which gets around the plugin API"]
    return []


def plugin_problems(plugin_id, commit, files):
    """files: {name: bytes} as the game downloads them. Exceptions match on id and commit."""
    problems = []
    for name, data in files.items():
        problems += file_problems(name, data)
    for (pid, prefix), why in EXCEPTIONS.items():
        if pid == plugin_id and commit.startswith(prefix):
            kept = [p for p in problems if "Console::" not in p]
            if kept != problems:
                print(f"  exception: {plugin_id} {commit[:7]}: {why}")
            problems = kept
    return problems


def check_registry():
    registry = json.loads((ROOT / "registry.json").read_text(encoding="utf-8"))
    problems = []
    for p in registry.get("plugins", []):
        base = f"https://raw.githubusercontent.com/{p['repo']}/{p['commit']}/" + (f"{p['path']}/" if p.get("path") else "")
        files = {}
        for name in p.get("files", {}):
            if suffix(name) in BINARY_EXTENSIONS:
                files[name] = b""              # refused by name, no need to download it
            else:
                files[name] = urllib.request.urlopen(base + name.replace(" ", "%20"), timeout=60).read()
        found = plugin_problems(p["id"], p["commit"], files)
        problems += [f"{p['id']} {p['version']}: {x}" for x in found]
        print(f"{p['id']:24} {'PROBLEMS' if found else 'ok'}")
    return problems


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, check=True).stdout


def check_changed(base):
    problems = []
    names = git("diff", "--name-only", "--diff-filter=AMR", f"{base}...HEAD").decode().splitlines()
    for name in names:
        data = (ROOT / name).read_bytes()
        if suffix(name) == ".as":
            continue                            # Console:: is checked on the added lines below
        problems += file_problems(name, data)
    # Added script lines (.as) with Console:: outside the places that are ours.
    path = None
    for line in git("diff", "-U0", f"{base}...HEAD").decode("utf-8", "replace").splitlines():
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else None
        elif (line.startswith("+") and path and path.endswith(".as") and CONSOLE.search(line.encode())
              and not path.startswith(CONSOLE_ALLOWED_PATHS)):
            problems.append(f"{path}: adds Console:: ({line[1:].strip()[:80]})")
    return problems


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "registry":
        problems = check_registry()
    elif len(sys.argv) >= 3 and sys.argv[1] == "changed":
        problems = check_changed(sys.argv[2])
    else:
        sys.exit(__doc__)
    if problems:
        print("\nRefused (see CLAUDE.md, review rules):")
        for p in problems:
            print("  " + p)
        sys.exit(1)
    print("review rules: ok")


if __name__ == "__main__":
    main()
