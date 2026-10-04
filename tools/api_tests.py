"""API tests: runs every test of tools/api-tests in the game and reports each documented API function as passing,
failing, skipped or untested.

    python tools/api_tests.py [--slot N] [--phases core,ui,...] [--editor-map <map name part>] [--track <level>] [--keep] [--strict]
    python tools/api_tests.py --coverage      (no game) checks every documented API has a test, and every test names real ones

The test plugin (tools/api-tests) is installed for the run only, with every other plugin turned off so nothing else
changes what it measures, and drives the game itself through its phases (main menu, Customize page, a track, a race,
a simulated replay, a workshop track, the track editor and a test run there). Each test logs a RESULT line; this maps
them onto the API as docs/api-examples.txt documents it (tools/gen_api_docs.py reads both), so a function no test
covers shows as untested.

--slot N runs it in sandboxed test copy N (tools/test_instance.py), beside the player's own game: its plugins, data,
saves and crashes are the copy's own. Without --slot it runs in the player's install, started through Steam, and
refuses to start while the game is running (it never takes over a game in use). The player's plugin settings and
storage, the plugins turned off, and window_at_start.txt are put back afterwards, and the saves and saved maps are
checked for changes. The report goes to build/api-test-report.md (and .json). Exit code 1 if anything failed (and,
with --strict, if anything is untested).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import gen_api_docs  # noqa: E402
try:
    import test_instance  # noqa: E402  (Windows only: the docs check, --coverage, also runs on Linux in CI)
except (ImportError, KeyError, AttributeError, OSError):
    test_instance = None

DATA = Path(os.environ.get("LOCALAPPDATA", ".")) / "Ballest" / "Saved" / "PluginManager"
SAVED = DATA.parent
LOG = DATA / "host.log"
CRASHES = SAVED / "Crashes"
PROCESS = "Ballest-Win64-Shipping.exe"
GAME_DIR = Path(os.environ.get("BALLEST_GAME_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Ballest of Them All\Ballest\Binaries\Win64"))
PLUGINS = GAME_DIR / "plugins"
SOURCE = HERE / "api-tests"
SLOT = 0
ID = "api-tests"
REPORT = ROOT / "build" / "api-test-report"
RESULT = re.compile(r"\[api-tests\] RESULT (PASS|FAIL|SKIP) (.*?) covers=(\S*) \| ?(.*)$")


def running():
    if SLOT:
        return bool(test_instance.alive(SLOT))
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {PROCESS}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return PROCESS.lower() in out.lower()


def lines():
    try:
        return LOG.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []


def snapshot(folder):
    """{relative path: (size, mtime)} of a folder's files, to tell whether anything changed."""
    if not folder.exists():
        return {}
    return {str(p.relative_to(folder)): (p.stat().st_size, p.stat().st_mtime) for p in folder.rglob("*") if p.is_file()}


def api_keys():
    """{key: namespace} of every documented function and property, as the docs name them ("UI::Window.SetAnchor")."""
    entries, _ = gen_api_docs.parse_api()
    return {k: v["namespace"] for k, v in gen_api_docs.collect(entries).items()}


def key_of(cover):
    """A test's covers entry as a documented key: "Window.AddText" is UI's."""
    return cover if "::" in cover else "UI::" + cover


def coverage():
    """Every documented API is covered by a test, and every test's covers name documented APIs (offline)."""
    keys = set(api_keys())
    covered = set()
    for m in re.finditer(r'Add\("[^"]+", "[^"]+", "([^"]*)"', (SOURCE / "main.as").read_text(encoding="utf-8")):
        covered |= {key_of(c) for c in m.group(1).split(",") if c}
    untested, unknown = sorted(keys - covered), sorted(covered - keys)
    print(f"{len(keys & covered)} of {len(keys)} documented APIs have a test")
    if untested:
        print("no test (add one to tools/api-tests/main.as):", ", ".join(untested))
    if unknown:
        print("tests name APIs the docs don't have:", ", ".join(unknown))
    sys.exit(1 if untested or unknown else 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phases", default="all", help="comma-separated phases to run (default all)")
    ap.add_argument("--editor-map", default="", help="part of a saved map's name for the editor phases (default: the first)")
    ap.add_argument("--keep", action="store_true", help="leave the game running afterwards")
    ap.add_argument("--track", default="", help="the official track's level for the track and race phases (default: the first)")
    ap.add_argument("--strict", action="store_true", help="exit 1 if any API is untested")
    ap.add_argument("--timeout", type=int, default=1500, help="seconds for the whole run")
    ap.add_argument("--coverage", action="store_true", help="only check (offline) that every documented API has a test")
    ap.add_argument("--slot", type=int, default=0, choices=range(0, 10), help="run in sandboxed test copy N (1-9)")
    args = ap.parse_args()
    if args.coverage:
        coverage()
    global SLOT, DATA, SAVED, LOG, CRASHES, PLUGINS
    if not args.slot and not args.coverage:
        sys.exit("api_tests.py only runs in a sandboxed test copy (its race tests teleport and fling the ball): pass --slot N")
    if args.slot:
        SLOT = args.slot
        DATA = test_instance.data_dir(SLOT)
        SAVED = test_instance.user_dir(SLOT) / "Saved"
        LOG, CRASHES, PLUGINS = DATA / "host.log", SAVED / "Crashes", DATA / "plugins"
        if not running():
            test_instance.prepare(SLOT, None, False, False)
    elif any(test_instance.alive(n) for n in range(1, 10)):
        sys.exit("test copies are running, and Steam won't start the game while they are: use --slot N")
    if running():
        sys.exit("Ballest is running; close it first (this never takes over a game in use).")

    # The player's state, put back afterwards.
    backup = Path(os.environ.get("TEMP", ".")) / f"ballest-api-tests-backup-slot{SLOT}-{os.getpid()}"   # never shared between runs
    shutil.rmtree(backup, ignore_errors=True)
    backup.mkdir(parents=True)
    for name in ("off.txt", "window_at_start.txt"):
        if (DATA / name).exists():
            shutil.copy2(DATA / name, backup / name)
    if (DATA / "storage").exists():
        shutil.copytree(DATA / "storage", backup / "storage")
    saves_before, maps_before = snapshot(SAVED / "SaveGames"), snapshot(SAVED / "UserSavedMaps")

    # Only the test plugin runs (the plugin manager can't be turned off; it is part of the host's own setup).
    others = [p.name for p in PLUGINS.iterdir() if p.is_dir() and p.name not in (ID, "plugin-manager")]
    (DATA / "off.txt").write_text("\n".join(sorted(others)) + "\n", encoding="utf-8")
    shutil.rmtree(PLUGINS / ID, ignore_errors=True)
    shutil.copytree(SOURCE, PLUGINS / ID)
    (DATA / "storage").mkdir(exist_ok=True)
    (DATA / "storage" / f"{ID}.txt").write_text(f"setting.Phases={args.phases}\nsetting.EditorMap={args.editor_map}\nsetting.Track={args.track}\n",
                                                encoding="utf-8")
    if LOG.exists():
        LOG.unlink()

    started = time.time()
    print(f"launching the game with {ID} (other plugins off: {', '.join(others) or 'none'})")
    if SLOT:
        if test_instance.start(SLOT, [], None, False, 180) != 0:
            sys.exit(f"slot {SLOT} didn't start sandboxed")
    else:
        os.startfile("steam://rungameid/3339810")
    finished, reason = False, "timed out"
    seen = 0
    try:
        while time.time() - started < args.timeout:
            time.sleep(1)
            if time.time() - started > 90 and not running():
                reason = "the game closed"
                break
            now = lines()
            for line in now[seen:]:
                if "[api-tests] PHASE " in line:
                    print("  phase", line.split("PHASE ", 1)[1])
                m = RESULT.search(line)
                if m and m.group(1) != "PASS":
                    print(f"    {m.group(1)} {m.group(2)}: {m.group(4)}")
                if re.search(r"\[api-tests\] (exception|stopped)", line):
                    reason = "the test plugin stopped: " + line.split("] ", 2)[-1]
                    finished = True
            seen = len(now)
            if any("[api-tests] DONE" in l for l in now):
                finished, reason = True, "done"
            if finished:
                break
    finally:
        crashes = [d.name for d in CRASHES.glob("UECC-*") if d.stat().st_mtime > started] if CRASHES.exists() else []
        if not args.keep and running():
            if SLOT:
                test_instance.stop(SLOT)
            else:
                subprocess.run(["taskkill", "/IM", PROCESS, "/F"], capture_output=True)
            time.sleep(3)
        shutil.rmtree(PLUGINS / ID, ignore_errors=True)
        for name in ("off.txt", "window_at_start.txt"):
            if (backup / name).exists():
                shutil.copy2(backup / name, DATA / name)
            else:
                (DATA / name).unlink(missing_ok=True)
        if (backup / "storage").exists():
            shutil.rmtree(DATA / "storage", ignore_errors=True)
            shutil.copytree(backup / "storage", DATA / "storage")
        changed_saves = sorted(set(snapshot(SAVED / "SaveGames").items()) ^ set(saves_before.items()))
        changed_maps = [c for c in sorted(set(snapshot(SAVED / "UserSavedMaps").items()) ^ set(maps_before.items()))
                        if not c[0].endswith(".vdf")]              # Steam Cloud's own bookkeeping, not a map

    # The results, by documented key.
    # A failing test's messages name the APIs they are about: only those fail. Its other APIs passed when every check
    # ran ("checks: ..."), and are unverified when it stopped early. A failure naming none fails them all.
    tests = []
    for line in lines():
        m = RESULT.search(line)
        if m:
            raw = [c for c in m.group(3).split(",") if c]
            detail = m.group(4)
            per = {}
            if m.group(1) == "FAIL":
                named = [c for c in raw if re.search(re.escape(c) + r"(?![\w])", detail)]
                for c in raw:
                    per[key_of(c)] = "FAIL" if c in named or not named else "PASS" if detail.startswith("checks:") else "UNVERIFIED"
            else:
                per = {key_of(c): m.group(1) for c in raw}
            tests.append({"status": m.group(1), "name": m.group(2), "covers": [key_of(c) for c in raw], "per": per, "detail": detail})
    keys = api_keys()
    by_key = {k: [] for k in keys}
    unknown = set()
    for t in tests:
        for k in t["covers"]:
            if k in by_key:
                by_key[k].append(t)
            else:
                unknown.add(k)
    status = {}
    for k, ts in by_key.items():
        states = {t["per"][k] for t in ts}
        status[k] = next((s for s in ("FAIL", "PASS", "UNVERIFIED", "SKIP") if s in states), "UNTESTED")
    counts = {s: sum(1 for v in status.values() if v == s) for s in ("PASS", "FAIL", "UNVERIFIED", "SKIP", "UNTESTED")}

    out = [f"# API test report", "", f"Host {next((l.split('host ')[1].split(' ')[0] for l in lines() if 'Ballest plugin host' in l), '?')}, "
           f"{time.strftime('%Y-%m-%d %H:%M')}. Run: {reason}. Tests: {len(tests)} "
           f"({sum(t['status'] == 'PASS' for t in tests)} passed, {sum(t['status'] == 'FAIL' for t in tests)} failed, "
           f"{sum(t['status'] == 'SKIP' for t in tests)} skipped).", "",
           f"API: {len(keys)} functions and properties: {counts['PASS']} pass, {counts['FAIL']} fail, {counts['UNVERIFIED']} unverified "
           f"(a test stopped before checking them), {counts['SKIP']} skipped, {counts['UNTESTED']} untested.", ""]
    if crashes:
        out += [f"**The game crashed**: {', '.join(crashes)}", ""]
    if changed_maps:
        out += ["**Saved maps changed during the run** (check them): " + ", ".join(sorted({p for p, _ in changed_maps})), ""]
    if changed_saves:
        out += ["The game saved " + ", ".join(sorted({p for p, _ in changed_saves})) + " (it does as it runs).", ""]
    if unknown:
        out += ["Tests name keys the docs don't have: " + ", ".join(sorted(unknown)), ""]
    failing = [k for k in keys if status[k] == "FAIL"]
    if failing:
        out += ["## Failing", ""]
        for k in failing:
            for t in by_key[k]:
                if t["per"][k] == "FAIL":
                    out.append(f"- `{k}`: {t['name']}: {t['detail']}")
        out.append("")
    out += ["## Every API", ""]
    for ns in dict.fromkeys(keys.values()):
        out.append(f"### {ns}")
        out.append("")
        out.append("| API | Result | Tests |")
        out.append("|---|---|---|")
        for k in (k for k in keys if keys[k] == ns):
            detail = "; ".join(f"{t['name']}" + (f" ({t['detail']})" if t["per"][k] != "PASS" and t["detail"] else "") for t in by_key[k])
            out.append(f"| `{k}` | {status[k]} | {detail} |")
        out.append("")
    REPORT.parent.mkdir(exist_ok=True)
    REPORT.with_suffix(".md").write_text("\n".join(out), encoding="utf-8")
    REPORT.with_suffix(".json").write_text(json.dumps({"run": reason, "crashes": crashes, "status": status, "tests": tests}, indent=1), encoding="utf-8")

    print()
    print(f"run: {reason}; {len(tests)} tests")
    print(f"API: {counts['PASS']} pass, {counts['FAIL']} fail, {counts['UNVERIFIED']} unverified, {counts['SKIP']} skipped, "
          f"{counts['UNTESTED']} untested of {len(keys)}")
    if counts["UNVERIFIED"]:
        print("  unverified:", ", ".join(k for k in keys if status[k] == "UNVERIFIED"))
    for k in failing:
        print("  FAIL", k)
    if counts["UNTESTED"]:
        print("  untested:", ", ".join(k for k in keys if status[k] == "UNTESTED"))
    if crashes:
        print("  CRASH:", ", ".join(crashes))
    if changed_maps:
        print("  SAVED MAPS CHANGED:", ", ".join(sorted({p for p, _ in changed_maps})))
    print(f"report: {REPORT.with_suffix('.md')}")
    sys.exit(1 if failing or crashes or reason != "done" or (args.strict and counts["UNTESTED"]) else 0)


if __name__ == "__main__":
    main()
