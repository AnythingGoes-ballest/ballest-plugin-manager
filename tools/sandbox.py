r"""sandbox: the sandboxed test copies and the player's own game, for agents (AXI: live state first, TOON on stdout,
progress on stderr, a few next commands after each answer, exit 0 ok / 1 error / 2 usage).

    python tools/sandbox.py                      the slots and the player's game at a glance
    python tools/sandbox.py <command> --help     one command's flags and examples

Built on test_instance.py (the slots) and the host's test channel (test_command.txt), which both keep working alone.
"""
import argparse
import contextlib
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_instance as ti  # noqa: E402

VERSION = "1.0.0"
DESCRIPTION = "Sandboxed Ballest test copies (slots 1-9) and the player's own game: start, drive, screenshot, reload plugins"
REPLY_LINES = 60                 # replies shown before --full
REPLY_CHARS = 300

# ---------------------------------------------------------------------------------------------------- TOON output

_NUMBER = re.compile(r"^-?\d+(\.\d+)?([eE][-+]?\d+)?$")


def q(v):
    """One TOON value: numbers and booleans bare, a string quoted only when it has to be."""
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return str(v)
    s = str(v)
    if (s == "" or s != s.strip() or s in ("true", "false", "null") or _NUMBER.match(s) or s.startswith("-")
            or any(c in s for c in ':",[]{}\\\n\r\t')):
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t") + '"'
    return s


class Out:
    """Collects one answer's lines; printed once at the end."""

    def __init__(self):
        self.lines = []
        self.helps = []

    def kv(self, key, value, indent=0):
        self.lines.append("  " * indent + f"{key}: {q(value)}")

    def obj(self, key, fields, indent=0):
        self.lines.append("  " * indent + f"{key}:")
        for k, v in fields.items():
            self.kv(k, v, indent + 1)

    def table(self, key, rows, fields, empty):
        if not rows:
            self.lines.append(f"{key}: {empty}")
            return
        self.lines.append(f"{key}[{len(rows)}]{{{','.join(fields)}}}:")
        for r in rows:
            self.lines.append("  " + ",".join(q(r.get(f)) for f in fields))

    def items(self, key, values, empty):
        if not values:
            self.lines.append(f"{key}: {empty}")
            return
        self.lines.append(f"{key}[{len(values)}]:")
        for v in values:
            self.lines.append("  - " + q(v))

    def help(self, *commands):
        self.helps.extend(commands)

    def emit(self):
        if self.helps:
            self.lines.append(f"help[{len(self.helps)}]:")
            self.lines.extend("  " + h for h in self.helps)
        print("\n".join(self.lines))


def fail(message, *helps, code=1):
    o = Out()
    o.kv("error", message)
    o.help(*helps)
    o.emit()
    sys.exit(code)


@contextlib.contextmanager
def progress():
    """What the wrapped scripts print is progress: it goes to stderr."""
    with contextlib.redirect_stdout(sys.stderr):
        yield


CLI = "python tools/sandbox.py"

# ---------------------------------------------------------------------------------------------------- state


def read_log(log):
    try:
        return log.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []


def host_version(lines):
    for l in lines[:5]:
        m = re.search(r"plugin host (\S+) starting", l)
        if m:
            return m.group(1)
    return None


def log_errors(lines):
    return [l for l in lines if "[error]" in l]


def plugins_in(folder, off_file=None):
    try:
        ids = sorted(p.name for p in folder.iterdir() if p.is_dir() and (p / "info.toml").exists())
    except OSError:
        return []
    off = set()
    if off_file and off_file.exists():
        off = set(off_file.read_text(encoding="utf-8").split())
    return [i for i in ids if i not in off]


def slot_state(n):
    pids = ti.alive(n)
    if pids:
        return "running"
    return "starting" if ti.claim_file(n).exists() else "free"


def slot_game(n):
    info = ti.instance(n) or {}
    return info.get("game") if info.get("game") in ti.alive(n) else None


def player_game():
    return next((p for p, name in ti.player_pids().items() if name.lower() == "ballest-win64-shipping.exe"), None)


def last_error(lines):
    errs = log_errors(lines)
    return strip_time(errs[-1])[:160] if errs else ""


def strip_time(line):
    return re.sub(r"^\[\d\d:\d\d:\d\d\.\d+\] ", "", line.strip())


def dashboard(o, compact=False):
    rows = []
    for n in range(1, 10):
        state = slot_state(n)
        if state == "free":
            continue
        lines = read_log(ti.data_dir(n) / "host.log")
        rows.append({"slot": n, "state": state, "host": host_version(lines), "errors": len(log_errors(lines)),
                     "plugins": " ".join(plugins_in(ti.data_dir(n) / "plugins", ti.data_dir(n) / "off.txt"))})
    o.kv("slots", f"{len(rows)} of 9 in use")
    o.table("running", rows, ["slot", "state", "host", "errors", "plugins"], "no slot running")
    game = player_game()
    lines = read_log(ti.PLAYER_LOG) if game else []
    player = {"game": "running" if game else "not running", "host": host_version(lines) if game else None,
              "errors": len(log_errors(lines)) if game else 0,
              "plugins": " ".join(plugins_in(ti.WIN64 / "plugins"))}
    if game and log_errors(lines):
        player["last_error"] = last_error(lines)
    o.obj("player", player)
    if compact:
        return
    if rows:
        n = rows[0]["slot"]
        o.help(f'{CLI} run {n} -c "state"', f"{CLI} reload {n} <plugin folder>", f"{CLI} stop {n}")
    else:
        o.help(f"{CLI} start next --plugin <plugin folder> [--map Map_LethTrial_01]",
               f"{CLI} install-player <plugin folder>")


# ---------------------------------------------------------------------------------------------------- driving a slot


def send(n, command):
    (ti.data_dir(n) / "test_command.txt").write_text(command, encoding="utf-8")


def wait_reply(n, before, match, timeout):
    """The first log line after line `before` that matches; '' if none in time."""
    end = time.time() + timeout
    while time.time() < end:
        for l in read_log(ti.data_dir(n) / "host.log")[before:]:
            if re.search(match, l):
                return l
        time.sleep(0.2)
    return ""


def sandboxed(n):
    before = len(read_log(ti.data_dir(n) / "host.log"))
    send(n, "sandbox")
    return "test: sandbox: on, complete" in wait_reply(n, before, r"test: sandbox:", 15)


def need_running(n):
    if not ti.alive(n):
        fail(f"slot {n} isn't running", f"{CLI} start {n} --plugin <plugin folder>")


def screenshot(n, name):
    path = ti.data_dir(n) / f"{name}.png"
    game = slot_game(n)
    out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(HERE / "screenshot_game.ps1"),
                          str(path)] + (["-GamePid", str(game)] if game else []), capture_output=True, text=True)
    return path if path.exists() and "saved" in (out.stdout + out.stderr).lower() else None


def run_commands(n, commands, full):
    """Each command in turn; returns (replies, shots, problems). Only the commands' own replies are kept."""
    log = ti.data_dir(n) / "host.log"
    replies, shots, problems = [], [], []
    previous = len(read_log(log))        # where the command before this one started: `wait` looks from there
    for raw in commands:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if not ti.alive(n):
            problems.append(f"the slot's game exited before `{line}`")
            break
        before = len(read_log(log))
        since, previous = previous, before
        if line.startswith("sleep "):
            time.sleep(float(line.split()[1]))
            continue
        if line.startswith("wait "):
            if not wait_reply(n, since, line[5:], 60):
                problems.append(f"wait: nothing matched {line[5:]!r} in 60 s")
        elif line.startswith("waitstate "):
            end, matched, reply = time.time() + 30, False, ""
            while time.time() < end and not matched:
                mark = len(read_log(log))
                send(n, "state")
                reply = wait_reply(n, mark, r"test: state ", 5)
                matched = bool(re.search(line[10:], reply))
                if not matched:
                    time.sleep(0.4)
            if not matched:
                problems.append(f"waitstate: no state matched {line[10:]!r} in 30 s")
            continue
        elif line.startswith("shot "):
            path = screenshot(n, line.split()[1])
            if path:
                shots.append(str(path))
            else:
                problems.append(f"shot {line.split()[1]}: no screenshot (is the window minimised?)")
            continue
        elif line.startswith("quick "):
            send(n, line[6:])
            time.sleep(0.2)
        else:
            send(n, line)
            # its reply, or a refusal; then a moment for the lines that follow it
            if wait_reply(n, before, r"test: |\[warn\] \[host\] test:", 5):
                time.sleep(0.3)
            else:
                time.sleep(1.0)
        for l in read_log(log)[before:]:
            if "test: " in l or "[error]" in l or "[warn]" in l:
                replies.append(strip_time(l).replace("[info] [host] test: ", "").replace("[host] test: ", ""))
    return replies, shots, problems


# ---------------------------------------------------------------------------------------------------- commands


def cmd_home(a):
    o = Out()
    exe = str(Path(__file__).resolve())
    home = str(Path.home())
    o.kv("bin", ("~" + exe[len(home):] if exe.lower().startswith(home.lower()) else exe).replace("\\", "/"))
    o.kv("description", DESCRIPTION)
    dashboard(o)
    o.emit()


def cmd_hook(a):
    o = Out()
    dashboard(o, compact=True)
    o.help(f"{CLI} --help for every command")
    o.emit()


def cmd_start(a):
    if a.slot != "next" and not (a.slot.isdigit() and 1 <= int(a.slot) <= 9):
        fail(f"slot must be 1-9 or next, not {a.slot}", f"{CLI} start next --plugin <plugin folder>", code=2)
    for p in a.plugin or []:
        if not (Path(p) / "info.toml").exists():
            fail(f"--plugin {p}: no info.toml there", f"{CLI} start {a.slot} --plugin <a folder with info.toml>", code=2)
    if a.slot != "next" and ti.alive(int(a.slot)):
        n = int(a.slot)
        o = Out()
        o.kv("slot", n)
        o.kv("result", "already running (no-op)")
        o.help(f"{CLI} reload {n} <plugin folder>", f"{CLI} stop {n}")
        o.emit()
        return
    with progress():
        if a.slot == "next":
            n = next((k for k in range(1, 10) if not ti.alive(k) and ti.claim(k)), None)
            if n is None:
                fail("no free slot", f"{CLI}", f"{CLI} stop <slot>")
            code = ti.start(n, [], None, False, a.timeout, plugin_dirs=a.plugin, claimed=True, wipe=a.clean,
                            only_mine=a.only, host=a.host)
        else:
            n = int(a.slot)
            code = ti.start(n, [], None, False, a.timeout, plugin_dirs=a.plugin, wipe=a.clean, only_mine=a.only, host=a.host)
    if code != 0:
        lines = read_log(ti.data_dir(n) / "host.log")
        fail(f"slot {n} didn't start sandboxed" + (f": {last_error(lines)}" if last_error(lines) else ""),
             f"{CLI} start {n} --plugin <plugin folder>")
    o = Out()
    o.kv("slot", n)
    o.kv("result", "started: sandbox complete, main menu up")
    if a.map:
        before = len(read_log(ti.data_dir(n) / "host.log"))
        send(n, f"open {a.map}")
        track = wait_reply(n, before, r"race: track ", 60)
        o.kv("map", a.map if track else f"{a.map} (no track loaded in 60 s)")
    lines = read_log(ti.data_dir(n) / "host.log")
    o.kv("host", host_version(lines))
    loaded = [strip_time(l) for l in lines if re.search(r"\] loaded ", l)]
    o.items("plugins", [re.sub(r"^\[info\] ", "", l) for l in loaded], "none loaded")
    o.items("errors", [strip_time(l) for l in log_errors(lines)], "none")
    circuit = "callx GM_Climb_C S_RPC_PlayerWantsToRestart | o:BP_MyPlayerController_C"
    o.help(f'{CLI} run {n} -c "{circuit}"' if a.map else f'{CLI} run {n} -c "open Map_LethTrial_01"',
           f"{CLI} front {n}", f"{CLI} stop {n}")
    o.emit()


def cmd_stop(a):
    o = Out()
    o.kv("slot", a.slot)
    if not ti.alive(a.slot):
        ti.release(a.slot)
        o.kv("result", "not running (no-op)")
        o.emit()
        return
    with progress():
        ok = ti.stop(a.slot)
    if not ok:
        fail(f"slot {a.slot} is still running", f"{CLI} stop {a.slot}")
    o.kv("result", "stopped")
    o.help(f"{CLI}", f"{CLI} start {a.slot} --plugin <plugin folder>")
    o.emit()


def cmd_run(a):
    need_running(a.slot)
    commands = a.c.split(";") if a.c else Path(a.file).read_text(encoding="utf-8").splitlines() if a.file else None
    if not commands:
        fail("give the commands with -c or --file", f'{CLI} run {a.slot} -c "state; press 34; waitstate E2; shot e2"', code=2)
    if not sandboxed(a.slot):
        fail(f"slot {a.slot} isn't completely sandboxed: nothing sent", f"{CLI} stop {a.slot}", f"{CLI} start {a.slot}")
    replies, shots, problems = run_commands(a.slot, commands, a.full)
    report_run(a.slot, replies, shots, problems, a.full)


def report_run(n, replies, shots, problems, full):
    o = Out()
    total = len(replies)
    shown = replies if full else [r if len(r) <= REPLY_CHARS else r[:REPLY_CHARS] + "..." for r in replies[:REPLY_LINES]]
    cut = not full and (total > REPLY_LINES or any(len(r) > REPLY_CHARS for r in replies[:REPLY_LINES]))
    o.items("replies", shown, "none (the commands logged no reply)")
    if cut:
        o.kv("truncated", f"{min(total, REPLY_LINES)} of {total} replies shown, long ones cut at {REPLY_CHARS} chars")
    if shots:
        o.items("shots", shots, "none")
    if problems:
        o.items("problems", problems, "none")
    if cut:
        o.help(f"{CLI} run {n} --full -c \"...\" for every reply in full")
    elif not problems:
        o.help(f'{CLI} run {n} -c "state"', f"{CLI} front {n}")
    o.emit()
    if problems:
        sys.exit(1)


def cmd_waitstate(a):
    need_running(a.slot)
    replies, shots, problems = run_commands(a.slot, [f"waitstate {a.regex}"], False)
    o = Out()
    if problems:
        fail(problems[0], f'{CLI} run {a.slot} -c "state"')
    o.kv("waitstate", f"matched {a.regex!r}")
    o.emit()


def cmd_shot(a):
    need_running(a.slot)
    path = screenshot(a.slot, a.name)
    if not path:
        fail(f"no screenshot of slot {a.slot}", f"{CLI} front {a.slot}")
    o = Out()
    o.kv("shot", str(path))
    o.emit()


def cmd_reload(a):
    need_running(a.slot)
    if not (Path(a.plugin) / "info.toml").exists():
        fail(f"{a.plugin}: no info.toml there", f"{CLI} reload {a.slot} <a folder with info.toml>", code=2)
    src, pid = ti.plugin_id(a.plugin)
    if not (ti.data_dir(a.slot) / "plugins" / pid).exists():
        fail(f"slot {a.slot} doesn't have {pid}", f"{CLI} stop {a.slot}", f"{CLI} start {a.slot} --plugin {a.plugin}")
    log = ti.data_dir(a.slot) / "host.log"
    before = len(read_log(log))
    with progress():
        code = ti.reload(a.slot, a.plugin)
    lines = [strip_time(l) for l in ti.plugin_lines("\n".join(read_log(log)[before:]), pid)]
    errors = [l for l in lines if "[error]" in l]
    if code != 0 and not errors:
        fail(f"{pid} didn't load again in slot {a.slot}", f"{CLI} run {a.slot} -c \"enable {pid} 1\"")
    o = Out()
    o.kv("slot", a.slot)
    o.kv("plugin", pid)
    o.kv("result", "reloaded" if not errors else "reloaded with errors")
    o.items("errors", errors, "none")
    o.help(f'{CLI} run {a.slot} -c "state"', f"{CLI} shot {a.slot} <name>")
    o.emit()
    if errors:
        sys.exit(1)


def cmd_install_player(a):
    if not (Path(a.plugin) / "info.toml").exists():
        fail(f"{a.plugin}: no info.toml there", f"{CLI} install-player <a folder with info.toml>", code=2)
    src, pid = ti.plugin_id(a.plugin)
    dest = ti.WIN64 / "plugins" / pid
    if dest.exists() and same_tree(src, dest) and player_game():
        lines = read_log(ti.PLAYER_LOG)
        if any(f"[{pid}] loaded" in l for l in lines):
            o = Out()
            o.kv("plugin", pid)
            o.kv("result", "already installed and loaded in the player's game (no-op)")
            o.emit()
            return
    was_running = bool(player_game())
    with progress():
        code = ti.install_player(a.plugin, a.timeout)
    lines = read_log(ti.PLAYER_LOG)
    mine = [strip_time(l) for l in ti.plugin_lines("\n".join(lines), pid)] if was_running else []
    errors = [l for l in mine if "[error]" in l]
    o = Out()
    o.kv("plugin", pid)
    o.kv("installed", str(dest))
    if was_running:
        o.kv("result", "game restarted, plugin loaded" if code == 0 else "game restarted, plugin didn't load cleanly")
        o.kv("host", host_version(lines))
        o.items("errors", errors, "none")
    else:
        o.kv("result", "installed; the player's game wasn't running, so it loads at the next start")
        o.help(f"{CLI} play")
    o.emit()
    if code != 0:
        sys.exit(1)


def same_tree(a, b):
    files = lambda root: {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()
                          and ".git" not in p.parts and p.suffix != ".md"}
    try:
        return files(a) == files(b)
    except OSError:
        return False


def cmd_front(a):
    if a.which == "player":
        game = player_game()
        if not game:
            fail("the player's game isn't running", f"{CLI} play")
    elif a.which.isdigit() and 1 <= int(a.which) <= 9:
        game = slot_game(int(a.which))
        if not game:
            fail(f"slot {a.which} isn't running", f"{CLI} start {a.which}")
    else:
        fail(f"front takes a slot (1-9) or player, not {a.which}", f"{CLI} front player", code=2)
    if not ti.front(game):
        fail(f"Windows didn't bring {a.which} in front", f"{CLI} front {a.which}")
    o = Out()
    o.kv("front", a.which)
    o.emit()


def cmd_play(a):
    o = Out()
    if player_game():
        o.kv("result", "the player's game is already running (no-op)")
        o.help(f"{CLI} front player")
        o.emit()
        return
    with progress():
        ti.play()
    o.kv("result", "the player's game is starting")
    o.help(f"{CLI} front player", f"{CLI}")
    o.emit()


def cmd_log(a):
    if a.which == "player":
        log = ti.PLAYER_LOG
    elif a.which.isdigit() and 1 <= int(a.which) <= 9:
        log = ti.data_dir(int(a.which)) / "host.log"
    else:
        fail(f"log takes a slot (1-9) or player, not {a.which}", f"{CLI} log player --errors", code=2)
    lines = [strip_time(l) for l in read_log(log)]
    if a.errors:
        lines = [l for l in lines if "[error]" in l or "[warn]" in l]
    if a.grep:
        lines = [l for l in lines if re.search(a.grep, l)]
    total = len(lines)
    shown = lines if a.full else lines[-a.last:]
    o = Out()
    o.kv("log", str(log))
    o.items("lines", shown, "none match" if (a.errors or a.grep) else "the log is empty")
    if not a.full and total > len(shown):
        o.kv("truncated", f"last {len(shown)} of {total} lines")
        o.help(f"{CLI} log {a.which} --full" + (" --errors" if a.errors else ""))
    o.emit()


def cmd_setup_hook(a):
    """Registers `sandbox.py hook` as a SessionStart hook (Claude Code), so a session starts with the dashboard."""
    import json
    settings = Path(a.settings)
    command = f'python "{Path(__file__).resolve()}" hook'
    try:
        data = json.loads(settings.read_text(encoding="utf-8")) if settings.exists() else {}
    except ValueError:
        fail(f"{settings} isn't valid JSON: nothing changed", f"{CLI} setup-hook --settings <settings.json>")
    hooks = data.setdefault("hooks", {}).setdefault("SessionStart", [])
    mine = [h for group in hooks for h in group.get("hooks", []) if h.get("command", "").endswith("sandbox.py\" hook")
            or h.get("command", "").endswith("sandbox.py hook")]
    o = Out()
    o.kv("settings", str(settings))
    if mine and all(h["command"] == command for h in mine):
        o.kv("result", "hook already set up (no-op)")
        o.emit()
        return
    if mine:
        for h in mine:
            h["command"] = command
        o.kv("result", "hook's path repaired")
    else:
        hooks.append({"hooks": [{"type": "command", "command": command}]})
        o.kv("result", "hook added: each session starts with the sandbox dashboard")
    settings.parent.mkdir(parents=True, exist_ok=True)
    settings.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    o.kv("command", command)
    o.emit()


# ---------------------------------------------------------------------------------------------------- arguments


class Parser(argparse.ArgumentParser):
    """Usage errors as TOON on stdout, with the command's valid flags, exit 2."""

    def error(self, message):
        flags = sorted({s for a in self._actions for s in a.option_strings if s.startswith("--") and s != "--help"})
        m = re.search(r"unrecognized arguments: (.*)", message)
        o = Out()
        o.kv("error", f"unknown argument {m.group(1)} for `{self.prog.split()[-1]}`" if m else message)
        o.kv("help", f"valid flags for `{self.prog.split()[-1]}`: " + (", ".join(flags) if flags else "none") +
             f" (--help always allowed); {self.prog} --help")
        o.emit()
        sys.exit(2)

    def print_help(self, file=None):
        super().print_help(sys.stdout)


PARSERS = {}


def build():
    ap = Parser(prog=CLI, description=DESCRIPTION, allow_abbrev=False)
    sub = ap.add_subparsers(dest="cmd", parser_class=Parser)

    def add(name, helptext, examples, fn):
        p = sub.add_parser(name, help=helptext, description=helptext, allow_abbrev=False,
                           epilog="examples:\n" + "\n".join(f"  {CLI} {e}" for e in examples),
                           formatter_class=argparse.RawDescriptionHelpFormatter)
        p.set_defaults(fn=fn)
        PARSERS[name] = p
        return p

    p = add("start", "start a sandboxed slot (waits for sandbox complete and the main menu)",
            ["start next --plugin ../ballest-plugins/plugins/checkpoint-finder --map Map_LethTrial_01",
             "start 2 --plugin ../ballest-grind-stats --only"], cmd_start)
    p.add_argument("slot", help="1-9, or next: the first free one")
    p.add_argument("--plugin", action="append", help="a plugin folder you're working on (any number)")
    p.add_argument("--map", help="a map to open once the menu is up (no run starts by itself)")
    p.add_argument("--only", action="store_true", help="every other plugin off")
    p.add_argument("--clean", action="store_true", help="empty the slot's data first")
    p.add_argument("--host", help="a host build (build/version.dll) for this slot only")
    p.add_argument("--timeout", type=int, default=120, help="seconds to wait for the copy (default 120)")

    p = add("stop", "stop a slot (a stopped one is a no-op)", ["stop 1"], cmd_stop)
    p.add_argument("slot", type=int, choices=range(1, 10))

    p = add("run", "send host test commands to a slot; prints only their replies",
            ['run 1 -c "callx GM_Climb_C S_RPC_PlayerWantsToRestart | o:BP_MyPlayerController_C; sleep 4; race"',
             'run 1 -c "press 34; waitstate E2; shot leth-E2"', "run 1 --file commands.txt --full"], cmd_run)
    p.add_argument("slot", type=int, choices=range(1, 10))
    p.add_argument("-c", help="commands separated by ; (also: sleep <s>, wait <regex>, waitstate <regex>, shot <name>, quick <cmd>)")
    p.add_argument("--file", help="a file of commands, one a line")
    p.add_argument("--full", action="store_true", help=f"every reply, uncut (default: {REPLY_LINES} lines of {REPLY_CHARS} chars)")

    p = add("waitstate", "wait (30 s) until a window's text matches: a key press landed, a card updated",
            ['waitstate 1 "E3 ·"'], cmd_waitstate)
    p.add_argument("slot", type=int, choices=range(1, 10))
    p.add_argument("regex")

    p = add("shot", "screenshot a slot's window to <name>.png beside its host.log", ["shot 1 leth-E3"], cmd_shot)
    p.add_argument("slot", type=int, choices=range(1, 10))
    p.add_argument("name")

    p = add("reload", "copy a plugin into a running slot again and restart it (reads its scripts again)",
            ["reload 1 ../ballest-plugins/plugins/checkpoint-finder"], cmd_reload)
    p.add_argument("slot", type=int, choices=range(1, 10))
    p.add_argument("plugin", help="the plugin folder (the slot must already have it)")

    p = add("install-player", "install a plugin into the player's own game: copy only its folder, restart the game "
            "if it runs, wait for the plugin to load, bring the game in front",
            ["install-player ../ballest-plugins/plugins/checkpoint-finder"], cmd_install_player)
    p.add_argument("plugin", help="the plugin folder")
    p.add_argument("--timeout", type=int, default=180, help="seconds to wait for the game (default 180)")

    p = add("front", "bring a slot's window or the player's game in front", ["front 1", "front player"], cmd_front)
    p.add_argument("which", help="1-9 or player")

    add("play", "start the player's own game (works while slots run; Steam's Play button doesn't)", ["play"], cmd_play)

    p = add("log", "a slot's or the player's host log", ["log 1 --errors", "log player --grep checkpoint-finder"], cmd_log)
    p.add_argument("which", help="1-9 or player")
    p.add_argument("--errors", action="store_true", help="only warnings and errors")
    p.add_argument("--grep", help="only lines matching this regex")
    p.add_argument("--last", type=int, default=30, help="lines shown (default 30)")
    p.add_argument("--full", action="store_true", help="every line")

    p = add("setup-hook", "register the dashboard as a Claude Code SessionStart hook (idempotent)",
            ["setup-hook --settings .claude/settings.local.json"], cmd_setup_hook)
    p.add_argument("--settings", default=".claude/settings.local.json", help="the settings file (default .claude/settings.local.json)")

    add("hook", "the compact dashboard a session-start hook prints", ["hook"], cmd_hook)
    return ap


def main():
    argv = sys.argv[1:]
    if len(argv) == 1 and argv[0] in ("-v", "-V", "--version"):
        print(VERSION)
        return
    if not argv:
        return cmd_home(None)
    a, extra = build().parse_known_args(argv)
    if extra:
        (PARSERS.get(getattr(a, "cmd", None)) or build()).error(f"unrecognized arguments: {' '.join(extra)}")
    if not getattr(a, "fn", None):
        return cmd_home(a)
    a.fn(a)


if __name__ == "__main__":
    main()
