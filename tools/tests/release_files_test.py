"""Offline: the rules for registry "release_files" (maintainer-built cooked content on a plugin's GitHub release).
Run by tools/tests/run.sh. The host applies the same rules in src/host/registry.cpp (ValidReleaseFileName/Url)."""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from review_guard import release_file_problems          # noqa: E402
from registry import release_files_of                    # noqa: E402

REPO = "AnythingGoes-ballest/ballest-elemental-balls"
BASE = f"https://github.com/{REPO}/releases/download/v0.1.0/"
SHA = "a" * 64
failures = 0


def check(ok, what):
    global failures
    if not ok:
        failures += 1
        print("FAIL", what)


def entry(files, repo=REPO):
    return {"id": "x", "repo": repo, "release_files": files}


ok = {"assets/elemental_balls_b0008-Windows.utoc": {"url": BASE + "elemental_balls_b0008-Windows.utoc", "sha256": SHA}}
check(release_file_problems(entry(ok)) == [], "a correct release file passes")
check(release_file_problems({"id": "x", "repo": REPO}) == [], "no release files: nothing to say")
bad = {
    "not under assets/": {"elemental_balls_b0008-Windows.utoc": {"url": BASE + "a.utoc", "sha256": SHA}},
    "a script": {"assets/main.as": {"url": BASE + "main.as", "sha256": SHA}},
    "a dll": {"assets/version.dll": {"url": BASE + "version.dll", "sha256": SHA}},
    "a subfolder": {"assets/sub/a.utoc": {"url": BASE + "a.utoc", "sha256": SHA}},
    "another repo's release": {"assets/a.utoc": {"url": "https://github.com/AnythingGoes-ballest/other/releases/download/v1/a.utoc", "sha256": SHA}},
    "not a release URL": {"assets/a.utoc": {"url": f"https://raw.githubusercontent.com/{REPO}/main/a.utoc", "sha256": SHA}},
    "a path in the URL": {"assets/a.utoc": {"url": BASE + "../a.utoc", "sha256": SHA}},
    "a short hash": {"assets/a.utoc": {"url": BASE + "a.utoc", "sha256": "abc"}},
}
for what, files in bad.items():
    check(release_file_problems(entry(files)) != [], f"refused: {what}")
check(release_file_problems(entry(ok, repo="someone/ballest-elemental-balls")) != [],
      "refused: a plugin outside the maintainer account (not maintainer-built)")

with tempfile.TemporaryDirectory() as d:
    f = Path(d) / "elemental_balls_b0008-Windows.ucas"
    f.write_bytes(b"container")
    made = release_files_of([str(f)], REPO, "v0.1.0")
    spec = made.get("assets/elemental_balls_b0008-Windows.ucas", {})
    check(spec.get("url") == BASE + "elemental_balls_b0008-Windows.ucas", "registry.py builds the release URL")
    import hashlib
    check(spec.get("sha256") == hashlib.sha256(b"container").hexdigest(), "registry.py hashes the file")
    check(release_file_problems(entry(made)) == [], "what registry.py writes passes the guard")

print(f"release_files_test: {'ok' if not failures else 'FAILED'} ({failures} failure(s))")
sys.exit(1 if failures else 0)
