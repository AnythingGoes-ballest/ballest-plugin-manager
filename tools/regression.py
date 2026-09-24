"""Regression suite for the host and the bundled plugins, run against the real game.

    python tools/regression.py [--keep] [--skip-map]

Launches Ballest with the installed host (./build.sh install first), drives it only through the host's test channel
(no mouse, no focus changes, no UE4SS), and checks each behaviour with a pass/fail line. Screenshots go next to
host.log in %LOCALAPPDATA%\\Ballest\\Saved\\PluginManager. Exit code 0 only if every check passed and nothing
crashed. It refuses to start while the game is running, so it never takes over a session in use.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = Path(os.environ["LOCALAPPDATA"]) / "Ballest" / "Saved" / "PluginManager"
LOG = DATA / "host.log"
CRASHES = Path(os.environ["LOCALAPPDATA"]) / "Ballest" / "Saved" / "Crashes"
SHOT = HERE / "screenshot_game.ps1"
PROCESS = "Ballest-Win64-Shipping.exe"
SPACE = 0x20
GAME_DIR = Path(os.environ.get("BALLEST_GAME_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Ballest of Them All\Ballest\Binaries\Win64"))
GAME_PLUGINS = GAME_DIR / "plugins"
FIXTURES = HERE / "test-plugins"          # faulty plugins, installed only for the run
# Plugin repos checked out next to this one: if present, installs and removals are tested against a local copy of
# the registry built from them (nothing is downloaded).
PLUGIN_REPOS = [HERE.parents[1] / "ballest-grind-timer-plugin", HERE.parents[1] / "ballest-replay-manager"]
MIRROR = DATA / "test-registry"
REGISTRY_URL_FILE = DATA / "registry_url.txt"

results = []


def running():
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {PROCESS}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return PROCESS.lower() in out.lower()


def lines():
    try:
        return LOG.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []


class Cursor:
    """Reads only the log lines written after a point, so each check sees its own evidence."""

    def __init__(self):
        self.seen = len(lines())

    @staticmethod
    def at(other):
        c = Cursor()
        c.seen = other.seen
        return c

    def wait(self, pattern, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            for line in lines()[self.seen:]:
                if re.search(pattern, line):
                    return line
            if not running():
                return None
            time.sleep(0.5)
        return None


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    return ok


def command(text, settle=0.0):
    (DATA / "test_command.txt").write_text(text, encoding="utf-8")
    # The host reads the channel twice a second; wait until it has taken the command.
    end = time.time() + 5
    while (DATA / "test_command.txt").exists() and time.time() < end:
        time.sleep(0.1)
    time.sleep(settle)


def click_when_there(label, timeout=5):
    """Clicks a button that may only appear once the UI has caught up (cards rebuild a few times a second)."""
    end = time.time() + timeout
    while time.time() < end:
        c = Cursor()
        command(f"click {label}")
        line = c.wait(r"test: click ", 3) or ""
        if line.endswith("-> ok"):
            return True
        time.sleep(0.3)
    return False


def replay_time():
    c = Cursor()
    command("replaytime")
    line = c.wait(r"test: replay time", 5)
    m = re.search(r"replay time ([\d.]+) of ([\d.-]+)", line or "")
    return (float(m.group(1)), float(m.group(2))) if m else (None, None)


def screenshot(name):
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SHOT), str(DATA / f"{name}.png")],
                   capture_output=True, text=True)


def startup(c):
    print("startup")
    check("host starts and accepts the build", c.wait(r"game build: PE timestamp 1949201014", 90))
    check("per-frame hook installed", c.wait(r"per-frame hook installed", 120))
    check("plugin manager loaded", c.wait(r"\[plugin-manager\] loaded Plugin Manager", 60))
    check("hello world logs through the API", c.wait(r"\[hello-world\] hello from a plugin", 10))
    check("replay manager loaded", c.wait(r"\[replay-manager\] replay manager ready", 10))
    check("a plugin that throws is stopped, with the reason", c.wait(r"\[zz-test-throws\] exception: Null pointer access", 30))
    check("a plugin that hangs is stopped at its time budget", c.wait(r"\[zz-test-hangs\] stopped: exceeded its 20 ms budget", 30))
    check("no compile or API errors", not any(re.search(r"\[error\] \[(compiler|host)\]", l) for l in lines()))
    time.sleep(12)      # the game's loading screen covers the menu for a few seconds; screenshots should show it


def footer(where, placed_since):
    print(f"footer ({where})")
    check(f"'plugins' button placed in the {where} footer", placed_since.wait(r"footer button 'plugins' placed", 30))
    c = Cursor()
    command("click plugins")
    check("panel opens on click", c.wait(r"\[plugin-manager\] panel opened", 10))
    c = Cursor()
    command("state", 0.5)
    state = c.wait(r"test: state", 5) or ""
    check("panel shown, the real plugins running", "panel[plugins]=shown" in state and
          all(f"{p}=running" in state for p in ("plugin-manager", "hello-world", "replay-manager")), state[-200:])
    screenshot(f"regression_footer_{where}")
    c = Cursor()
    command("click plugins")
    check("panel closes on second click", c.wait(r"\[plugin-manager\] panel closed", 10))


def console_menu():
    print("plugin manager menu and console")
    c = Cursor()
    command("click plugins")
    check("panel opens for the menu", c.wait(r"\[plugin-manager\] panel opened", 10))
    c = Cursor()
    command("click open")
    check("menu opens from the panel's open button", c.wait(r"\[plugin-manager\] menu opened", 10))
    check("menu window built", c.wait(r"window built with \d+ widgets", 10))
    c = Cursor()
    command("submit find WBP_Footer_C", 0.5)
    check("console echoes the command", c.wait(r"\[plugin-manager\] > find WBP_Footer_C", 5))
    check("console runs the command", c.wait(r"test: found \S+ .*WBP_Footer_C", 5))
    time.sleep(1)
    screenshot("regression_console")
    c = Cursor()
    command("click close")
    check("menu closes", c.wait(r"\[plugin-manager\] menu closed", 5))


def plugin_browser(mirrored):
    print("plugin browser" + ("" if mirrored else " (no plugin repos next to this one: install and remove not tested)"))
    c = Cursor()
    command("click open")
    check("menu opens again", c.wait(r"\[plugin-manager\] menu opened", 10))
    command("click window:installed", 1.0)
    c = Cursor()
    command("state", 0.5)
    state = c.wait(r"test: state", 5) or ""
    check("installed tab lists the installed plugins as cards", "text[Plugin Manager]" in state and "text[Hello World]" in state)
    if mirrored:
        check("registry read from the local copy", any(re.search(r"registry: \d+ plugin\(s\) from file:", l) for l in lines()))
        # Start from a known state whatever an earlier run left: both registry plugins installed. The first card with
        # a remove button (plugins load in folder order after the plugin manager) is then a registry plugin.
        for plugin in ("grind-timer", "replay-manager"):
            if not (GAME_PLUGINS / plugin).exists():
                c = Cursor()
                command(f"install {plugin}")
                c.wait(rf"registry: installed {plugin}", 20)
        screenshot("regression_browser")
        c = Cursor()
        command("click remove#1")
        removed = c.wait(r"registry: removed ([\w-]+)", 10)
        victim = re.search(r"registry: removed ([\w-]+)", removed).group(1) if removed else ""
        check("remove on a card takes the plugin out straight away", bool(victim) and any(f"[{victim}] unloaded" in l for l in lines()))
        check("its folder is gone", bool(victim) and not (GAME_PLUGINS / victim).exists())
        command("click window:browse", 1.0)
        c = Cursor()
        click_when_there("install")
        check("install from the browse tab downloads, verifies and starts it",
              bool(victim) and c.wait(rf"registry: installed {victim}", 20) and c.wait(rf"\[{victim}\] loaded", 5))
        command("click window:installed", 1.0)
    c = Cursor()
    command("click settings#1", 1.0)
    command("state", 0.5)
    state = c.wait(r"test: state", 5) or ""
    check("a card's settings button opens that plugin's settings page", " settings]" in state and any(f"text[{name}]" in state for name in ("Placement distance", "Time size", "Message")))
    c = Cursor()
    command("click window:console", 1.0)
    command("state", 0.5)
    check("console tab opens", bool(c.wait(r"test: state", 5)))
    c = Cursor()
    command("click close")
    check("menu closes", c.wait(r"\[plugin-manager\] menu closed", 5))


def replay_manager():
    print("replay manager (simulated 30 s replay)")
    c = Cursor()
    command("fakereplay on 30")
    check("controls appear when a replay starts", c.wait(r"\[replay-manager\] replay controls shown", 10))
    check("controls window built", c.wait(r"window built with 7 widgets", 10))
    time.sleep(3)
    t, length = replay_time()
    check("total length known from the start", length == 30.0, f"{t} of {length}")
    screenshot("regression_replay")

    c = Cursor()
    command(f"press {SPACE}")
    check("Space pauses", c.wait(r"\[replay-manager\] paused", 5))
    a, _ = replay_time()
    time.sleep(2)
    b, _ = replay_time()
    check("paused time holds still", a is not None and abs(b - a) < 0.05, f"{a} then {b}")

    c = Cursor()
    command(f"press {SPACE}")
    check("Space resumes", c.wait(r"\[replay-manager\] playing", 5))
    c = Cursor()
    command("select .1x 4")
    check("speed dropdown sets 2x", c.wait(r"\[replay-manager\] speed 2x", 5))
    a, _ = replay_time()
    time.sleep(3)
    b, _ = replay_time()
    rate = (b - a) / 3 if a is not None and b is not None else 0
    check("2x plays at about twice real time", 1.6 < rate < 2.4 or b < a, f"{a} -> {b}, rate {rate:.2f}")

    command("slider 0.5", 0.3)
    t, _ = replay_time()
    check("scrubbing to the middle seeks there", t is not None and 14.0 <= t <= 16.5, f"{t}")

    c = Cursor()
    command("select .1x 5")
    check("5x restarts at the end instead of stopping", c.wait(r"replay finished at 5x; restarted", 15))

    c = Cursor()
    command("select default 1", 0.5)
    check("camera dropdown selects follow 3d", c.wait(r"camera mode 1", 5))
    command("select default 0", 0.5)

    c = Cursor()
    command("fakereplay off")
    check("controls hide when the replay ends", c.wait(r"\[replay-manager\] replay controls hidden", 10))


def map_load():
    print("map load")
    c = Cursor()
    command("open Map_Track13")
    check("map opens", c.wait(r"test: open Map_Track13 -> ok", 10))
    check("in-map footer adopted", c.wait(r"footer .*WBP_RaceUIManager_C", 60))
    time.sleep(5)
    check("game still running after the map load", running())
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="leave the game running afterwards")
    ap.add_argument("--skip-map", action="store_true")
    args = ap.parse_args()
    if running():
        print("Ballest is running; close it first (this never takes over a game in use).")
        sys.exit(2)
    if LOG.exists():
        LOG.unlink()
    for fixture in FIXTURES.iterdir():
        shutil.copytree(fixture, GAME_PLUGINS / fixture.name, dirs_exist_ok=True)
    mirrored = all(repo.exists() for repo in PLUGIN_REPOS)
    if mirrored:
        url = subprocess.run([sys.executable, str(HERE / "registry.py"), "mirror", str(MIRROR), *map(str, PLUGIN_REPOS)],
                             capture_output=True, text=True, check=True).stdout.strip()
        REGISTRY_URL_FILE.write_text(url, encoding="utf-8")
    launch = Cursor()
    started = time.time()
    os.startfile("steam://rungameid/3339810")
    for _ in range(90):
        if running():
            break
        time.sleep(1)

    try:
        startup(Cursor.at(launch))
        footer("main menu", launch)
        console_menu()
        plugin_browser(mirrored)
        replay_manager()
        if not args.skip_map:
            footer("map", map_load())
    finally:
        crashes = [d.name for d in CRASHES.glob("UECC-*") if d.stat().st_mtime > started] if CRASHES.exists() else []
        check("no crash report", not crashes, ", ".join(crashes))
        if not args.keep and running():
            subprocess.run(["taskkill", "/IM", PROCESS, "/F"], capture_output=True)
            time.sleep(2)
        for fixture in FIXTURES.iterdir():
            shutil.rmtree(GAME_PLUGINS / fixture.name, ignore_errors=True)
        REGISTRY_URL_FILE.unlink(missing_ok=True)
        shutil.rmtree(MIRROR, ignore_errors=True)

    failed = [name for name, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed" + (f"; failed: {', '.join(failed)}" if failed else ""))
    sys.exit(0 if not failed else 1)


if __name__ == "__main__":
    main()
