"""Measurement session: launches the game, optionally opens a map, then sends host test-channel commands.

    python tools/dev_session.py commands.txt [--map Map_LethTrial_01] [--keep] [--force | --attach]

The map is opened with the host's own `open` command (GameplayStatics.OpenLevel), which skips the menu's level
setup: medal times and the leaderboard stay unloaded in that map.

commands.txt lines:
    sleep <seconds>
    wait <regex>          wait (up to 60 s) for a host.log line matching the regex
    shot <name>           screenshot to %LOCALAPPDATA%/Ballest/Saved/PluginManager/<name>.png
    anything else         written to the host's test_command.txt (see src/host/main.cpp)
Prints every host.log line produced. Never touches a game the user is running (unless --force). --attach sends the
commands to a game a previous --keep run left open, without restarting it.
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = Path(os.environ["LOCALAPPDATA"]) / "Ballest" / "Saved" / "PluginManager"
LOG = DATA / "host.log"
SHOT = HERE / "screenshot_game.ps1"
PROCESS = "Ballest-Win64-Shipping.exe"


def running():
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {PROCESS}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    return PROCESS.lower() in out.lower()


def log_lines():
    try:
        return LOG.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("commands")
    ap.add_argument("--map", default="")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--attach", action="store_true", help="use the game a previous --keep run left open")
    ap.add_argument("--settle", type=int, default=25, help="seconds to wait after opening the map")
    args = ap.parse_args()
    if args.attach:
        if not running():
            sys.exit("--attach: Ballest is not running")
    elif running():
        if not args.force:
            print("Ballest is already running; close it or pass --force.")
            sys.exit(2)
        subprocess.run(["taskkill", "/IM", PROCESS, "/F"], capture_output=True)
        time.sleep(3)

    if not args.attach:
        if LOG.exists():
            LOG.unlink()
        os.startfile("steam://rungameid/3339810")
        for _ in range(90):
            if running() and any("per-frame hook installed" in l for l in log_lines()):
                break
            time.sleep(1)
        time.sleep(8)
    if args.map:
        (DATA / "test_command.txt").write_text(f"open {args.map}", encoding="utf-8")
        time.sleep(args.settle)
    print(f"game up; {len(log_lines())} host log lines so far")

    seen = len(log_lines()) if args.attach else 0
    for raw in Path(args.commands).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if not running():
            print("game exited")
            break
        if line.startswith("sleep "):
            time.sleep(float(line.split()[1]))
        elif line.startswith("wait "):
            pattern = line[5:]
            end = time.time() + 60
            while time.time() < end and not any(re.search(pattern, l) for l in log_lines()[seen:]):
                time.sleep(0.5)
        elif line.startswith("shot "):
            out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SHOT),
                                  str(DATA / f"{line.split()[1]}.png")], capture_output=True, text=True)
            print("  screenshot:", (out.stdout or out.stderr).strip()[:150])
        else:
            (DATA / "test_command.txt").write_text(line, encoding="utf-8")
            time.sleep(1.5)
        lines = log_lines()
        for l in lines[seen:]:
            print("  " + l)
        seen = len(lines)

    time.sleep(1)
    for l in log_lines()[seen:]:
        print("  " + l)
    print("game still running:", running())
    if not args.keep and running():
        subprocess.run(["taskkill", "/IM", PROCESS, "/F"], capture_output=True)


if __name__ == "__main__":
    main()
