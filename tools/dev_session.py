"""Measurement session: launches the game, optionally opens a map, then sends host test-channel commands.

    python tools/dev_session.py commands.txt [--slot N] [--map Map_LethTrial_01] [--keep] [--force | --attach]

--slot N runs the commands on sandboxed test copy N (tools/test_instance.py), started for the session (or left
running by an earlier --keep, with --attach) and stopped after it: any number of sessions at once, each on its own
slot, beside the user's own game. Without --slot it's the one game started through Steam, the player's own install
with nothing sandboxed (only when the user isn't playing).

The map is opened with the host's own `open` command (GameplayStatics.OpenLevel), which skips the menu's level
setup: medal times and the leaderboard stay unloaded in that map.

commands.txt lines:
    sleep <seconds>
    wait <regex>          wait (up to 60 s) for a host.log line matching the regex (prints WAIT TIMED OUT if none)
    shot <name>           screenshot to the host's data folder: <name>.png beside host.log (with --slot, the slot's)
    quick <command>       a command without the 1.5 s wait after it (to time a screenshot)
    anything else         written to the host's test_command.txt (see src/host/main.cpp)
Prints every host.log line produced. Never touches a game the user is running (unless --force, which ends the user's
game and never a test copy). --attach sends the commands to a game a previous --keep run left open, without restarting
it.
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_instance  # noqa: E402

DATA = Path(os.environ["LOCALAPPDATA"]) / "Ballest" / "Saved" / "PluginManager"
LOG = DATA / "host.log"
SHOT = HERE / "screenshot_game.ps1"
PROCESS = "Ballest-Win64-Shipping.exe"
SLOT = 0


def own_games():
    """The game processes this session works with: its slot's, or the ones that aren't any slot's."""
    if SLOT:
        info = test_instance.instance(SLOT) or {}
        return [pid for pid in test_instance.alive(SLOT) if pid == info.get("game")]
    slots = {pid for n in range(1, 10) for pid in test_instance.alive(n)}
    return [pid for pid, (_, name) in test_instance.processes().items() if name.lower() == PROCESS.lower() and pid not in slots]


def running():
    return bool(own_games())


def end_game():
    if SLOT:
        test_instance.stop(SLOT)
    else:
        for pid in own_games():
            subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)


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
    ap.add_argument("--slot", type=int, default=0, choices=range(0, 10), help="sandboxed test copy N (1-9)")
    ap.add_argument("--plugin", action="append", help="with --slot: a plugin folder you're working on, installed into the copy")
    args = ap.parse_args()
    # Only ever a sandboxed slot (2026-10-03: test commands can teleport the ball, and one reached the real leaderboard
    # from a slot whose sandbox missed Steam's interfaces). The player's own game is never driven by a tool.
    if not args.slot:
        sys.exit("dev_session.py only drives a sandboxed test copy: pass --slot N (tools/test_instance.py start next ...)")
    global SLOT, DATA, LOG
    if args.slot:
        SLOT = args.slot
        DATA = test_instance.data_dir(SLOT)
        LOG = DATA / "host.log"
        if args.force:
            sys.exit("--force is for the Steam game; a slot's own copy is stopped with tools/test_instance.py stop N")
    if args.attach:
        if not running():
            sys.exit("--attach: Ballest is not running")
    elif running():
        if not args.force:
            print("Ballest is already running; close it or pass --force.")
            sys.exit(2)
        end_game()
        time.sleep(3)

    if not args.attach and SLOT:
        start = [sys.executable, str(HERE / "test_instance.py"), "start", str(SLOT)] + [x for p in args.plugin or [] for x in ("--plugin", p)]
        if subprocess.run(start).returncode != 0:
            sys.exit(f"slot {SLOT} didn't start sandboxed")
        for _ in range(90):
            if any("per-frame hook installed" in l for l in log_lines()):
                break
            time.sleep(1)
        time.sleep(8)
    elif not args.attach:
        if any(test_instance.alive(n) for n in range(1, 10)):
            sys.exit("test copies are running, and Steam won't start the game while they are (it counts them as Ballest "
                     "running): use --slot N, or start the game with tools/test_instance.py play")
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
    # The copy's own verdict, asked now: every block in place, in the game's own Steam interfaces too. Nothing is sent
    # to a copy that isn't sandboxed completely.
    before = len(log_lines())
    (DATA / "test_command.txt").write_text("sandbox", encoding="utf-8")
    verdict = ""
    for _ in range(60):
        verdict = next((l for l in log_lines()[before:] if "test: sandbox:" in l), "")
        if verdict:
            break
        time.sleep(0.5)
    if "test: sandbox: on, complete" not in verdict:
        sys.exit(f"slot {SLOT} isn't completely sandboxed, refusing to drive it: {verdict.strip()[:300] or 'no answer'}")

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
            if not any(re.search(pattern, l) for l in log_lines()[seen:]):
                print(f"  WAIT TIMED OUT: nothing matched {pattern!r} in 60 s")
        elif line.startswith("shot "):
            out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(SHOT),
                                  str(DATA / f"{line.split()[1]}.png")] + (["-GamePid", str(own_games()[0])] if own_games() else []),
                                 capture_output=True, text=True)
            print("  screenshot:", (out.stdout or out.stderr).strip()[:150])
        elif line.startswith("quick "):          # a command without the wait after it (timing a screenshot)
            (DATA / "test_command.txt").write_text(line[6:], encoding="utf-8")
            time.sleep(0.2)
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
        end_game()


if __name__ == "__main__":
    main()
