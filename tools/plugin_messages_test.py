"""Exchange plugin-scoped lobby messages between two sandboxed Ballest copies."""

import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_instance  # noqa: E402

PLUGIN = HERE / "test-plugins" / "plugin-message-test"
HOST = HERE.parent / "build" / "version.dll"
PASS = "PLUGIN_MESSAGE_TEST PASS"
FAIL = "PLUGIN_MESSAGE_TEST FAIL"


def log(slot):
    path = test_instance.data_dir(slot) / "host.log"
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def main():
    if not HOST.exists():
        raise SystemExit("build/version.dll is missing; run build.sh first")
    slots = []
    for slot in range(1, 10):
        if not test_instance.alive(slot) and test_instance.claim(slot):
            slots.append(slot)
            if len(slots) == 2:
                break
    if len(slots) != 2:
        for slot in slots:
            test_instance.release(slot)
        raise SystemExit("two free test slots are required")
    started = []
    try:
        for slot in slots:
            code = test_instance.start(
                slot,
                [],
                None,
                False,
                180,
                plugin_dirs=[PLUGIN],
                wipe=True,
                only_mine=True,
                host=HOST,
                claimed=True,
            )
            if code != 0:
                raise RuntimeError(f"slot {slot} did not start")
            started.append(slot)
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            texts = [log(slot) for slot in slots]
            failures = [line for text in texts for line in text.splitlines() if FAIL in line]
            if failures:
                raise RuntimeError("\n".join(failures))
            if all(PASS in text for text in texts):
                print("plugin messages: two-copy exchange passed")
                return 0
            time.sleep(0.5)
        raise RuntimeError("timed out\n" + "\n".join(log(slot)[-2000:] for slot in slots))
    finally:
        for slot in started:
            test_instance.stop(slot)
        for slot in slots:
            test_instance.release(slot)


if __name__ == "__main__":
    raise SystemExit(main())
