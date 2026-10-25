"""Fast control of a sandboxed test copy for agents: the game's state, a picture of the screen and the ball, in about a
frame each, through the host's agent pipe (\\\\.\\pipe\\ballest-test-<game pid>, host 0.31.0+, sandboxed copies only).

    python tools/agent.py <slot> state                      the state as JSON (ball, camera, race, track, UI)
    python tools/agent.py <slot> render [name]              a PNG of the whole screen (HUD and plugin windows too)
    python tools/agent.py <slot> drive <x> <y> <frames> [jump]   steer (x right, y forward, -1..1) for that many frames
    python tools/agent.py <slot> tap <x> <y>                click at a pixel of the render picture (no window needed)
    python tools/agent.py <slot> hover <x> <y> | hover off  the pointer held there (hovers) / the real mouse again
    python tools/agent.py <slot> -c "cmd; cmd; ..."         any test commands, sent together, each one's replies
    python tools/agent.py <slot> repl                       one command per stdin line, one JSON answer per line

As a library (keeps the connection open, so each call is one round trip):

    from agent import Slot
    s = Slot(2)
    s.state()["ball"]["pos"]; s.drive(0, 1, 60); s.render("look"); s.cmd("race"); s.wait(30)

The copy must be started first (tools/sandbox.py start), and only on the test map or your own create-mode maps.
Timing (2026-10-24, headless copy at 30 fps): a command about one frame, render about 120 ms, against about 6 s for one
command through the file channel (sandbox.py run before this).
"""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_instance as ti  # noqa: E402


class NoPipe(Exception):
    """The slot isn't running, isn't a complete sandbox yet, or its host has no agent pipe (older than 0.31.0)."""


class Slot:
    def __init__(self, n, timeout=20):
        info = ti.instance(n) or {}
        pid = info.get("game")
        if not pid or not ti.running_fast(n):
            raise NoPipe(f"slot {n} isn't running")
        self.n, self.path = n, rf"\\.\pipe\ballest-test-{pid}"
        end = time.time() + timeout
        while True:
            try:
                self.pipe = open(self.path, "r+b", buffering=0)
                break
            except FileNotFoundError:
                if time.time() > end:
                    raise NoPipe(f"slot {n}: no agent pipe (its host is older than 0.31.0, or it isn't a complete sandbox)")
                time.sleep(0.05)
            except OSError:             # busy: another client has it; wait for it to let go
                if time.time() > end:
                    raise NoPipe(f"slot {n}: the agent pipe stayed busy (another client is connected)")
                time.sleep(0.05)
        self.buffer = b""

    def close(self):
        self.pipe.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _line(self):
        while b"\n" not in self.buffer:
            chunk = self.pipe.read(65536)
            if not chunk:
                raise NoPipe(f"slot {self.n}: the game closed the pipe")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def send(self, *commands):
        """Sends the commands together (they run on the same frame, in order); one answer each:
        {"id", "frame", "lines": [their replies]}."""
        commands = [c.strip() for c in commands if c.strip()]
        self.pipe.write("".join(c + "\n" for c in commands).encode("utf-8"))
        return [self._line() for _ in commands]

    def cmd(self, command):
        """One test command's reply lines."""
        return self.send(command)[0]["lines"]

    def state(self):
        """The game's state: frame, race (on_track, active, complete, run, checkpoint...), track, ball (pos, vel, speed),
        input, camera (pos, pitch, yaw, roll) and the UI line."""
        for line in self.cmd("agentstate"):
            if line.startswith("agentstate "):
                return json.loads(line[len("agentstate "):])
        raise NoPipe(f"slot {self.n}: no state in the answer")

    def render(self, name="render"):
        """A PNG of the whole screen (HUD and plugin windows included) in the slot's data folder; its path."""
        lines = self.cmd(f"render {name}")
        last = lines[-1] if lines else ""
        if not last.startswith("render -> ") or "failed" in last:
            raise NoPipe(f"slot {self.n}: {last or 'no answer to render'}")
        return last[len("render -> "):]

    def drive(self, x, y, frames, jump=False):
        """Steers the ball (x right, y forward, -1..1) for `frames` frames, then lets go; the state after."""
        lines = self.cmd(f"drive {x} {y} {int(frames)}" + (" jump" if jump else ""))
        last = lines[-1] if lines else ""
        if last.startswith("drive -> done "):
            return json.loads(last[len("drive -> done "):])
        raise NoPipe(f"slot {self.n}: {last or 'no answer to drive'}")

    def hold(self, x, y):
        """Holds the steering until the next drive / hold (hold(0, 0) lets go)."""
        return self.cmd(f"drive {x} {y}")

    def tap(self, x, y):
        """A click at pixel (x, y) of the screen, as a render picture shows it, with no window or mouse needed: the plugin
        manager's window if open, else a plugin window's button, else the game's own button there. What it clicked."""
        lines = self.cmd(f"tap {x} {y}")
        return lines[-1] if lines else ""

    def hover(self, x, y=None):
        """The pointer held at pixel (x, y) (hover effects, tooltips); hover("off") gives the real mouse back."""
        return self.cmd("hover off" if x == "off" else f"hover {x} {y}")

    def seq(self, *commands):
        """The commands one a frame, in order; all their replies once the last has run."""
        return self.cmd("seq " + " || ".join(commands))

    def repeat(self, frames, command):
        """The same command on each of that many frames (sampling); all its replies."""
        return self.cmd(f"repeat {int(frames)} {command}")

    def reload_map(self):
        """The map opened last (openworkshop / openmap / open) opened again the same way; answers once it's open."""
        return self.cmd("reloadmap")

    def editor_camera(self, x=None, y=None, z=None, pitch=None, yaw=None):
        """The track editor's camera as {"pos", "pitch", "yaw", "roll"}; with x, y, z (and pitch, yaw) moved there first."""
        args = "" if x is None else f" {x} {y} {z}" + ("" if pitch is None else f" {pitch} {yaw}")
        for line in self.cmd("editorcam" + args):
            if line.startswith("editorcam {"):
                return json.loads(line[len("editorcam "):])
        raise NoPipe(f"slot {self.n}: not in the track editor")

    def wait(self, frames):
        """Answers once that many frames have run."""
        return self.cmd(f"waitframes {int(frames)}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    n, verb, rest = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
    try:
        with Slot(n) as s:
            if verb == "state":
                print(json.dumps(s.state(), indent=1))
            elif verb == "render":
                print(s.render(rest[0] if rest else "render"))
            elif verb == "drive":
                x, y, frames = float(rest[0]), float(rest[1]), int(rest[2])
                print(json.dumps(s.drive(x, y, frames, "jump" in rest[3:]), indent=1))
            elif verb == "tap":
                print(s.tap(float(rest[0]), float(rest[1])))
            elif verb == "hover":
                for line in s.hover("off") if rest[0] == "off" else s.hover(float(rest[0]), float(rest[1])):
                    print(line)
            elif verb == "-c":
                for answer in s.send(*" ".join(rest).split(";")):
                    for line in answer["lines"]:
                        print(line)
            elif verb == "repl":
                for line in sys.stdin:
                    if line.strip():
                        print(json.dumps(s.send(line)[0]), flush=True)
            else:
                sys.exit(__doc__)
    except NoPipe as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
