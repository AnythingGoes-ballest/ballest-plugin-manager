"""Names the host's frames in the newest Ballest crash report.

    python tools/symbolize.py [crash folder]

Crash reports list host frames as "VERSION 0x<base> + <offset>". Each offset is mapped to a function and line in
build/version.sym.dll, the unstripped twin of the installed DLL (both come from the same ./build.sh run, so this
is only right for crashes of the build that is currently built).
"""
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CRASHES = Path(os.environ["LOCALAPPDATA"]) / "Ballest" / "Saved" / "Crashes"
IMAGE_BASE = 0x180000000        # the DLL's preferred base; crash offsets are relative to wherever it loaded


def main():
    folder = Path(sys.argv[1]) if len(sys.argv) > 1 else max(CRASHES.glob("UECC-*"), key=lambda d: d.stat().st_mtime)
    report = next(folder.glob("*.runtime-xml")).read_text(encoding="utf-8", errors="replace")
    message = re.search(r"<ErrorMessage>(.*?)</ErrorMessage>", report, re.S)
    print(folder.name)
    print(message.group(1).strip() if message else "(no message)")
    stack = re.search(r"<PCallStack>(.*?)</PCallStack>", report, re.S).group(1)
    symbolizer = ROOT / "tools" / "llvm-mingw" / "bin" / "llvm-addr2line"
    for line in stack.splitlines():
        m = re.match(r"\s*(\S+)\s+0x[0-9a-fA-F]+\s*\+\s*([0-9a-fA-F]+)", line)
        if not m:
            continue
        module, offset = m.group(1), int(m.group(2), 16)
        if module.upper() == "VERSION":
            out = subprocess.run([str(symbolizer), "-f", "-C", "-e", str(ROOT / "build" / "version.sym.dll"), hex(IMAGE_BASE + offset)],
                                 capture_output=True, text=True).stdout.split("\n")
            print(f"  host  {out[0]}  {Path(out[1]).name if len(out) > 1 else ''}")
        else:
            print(f"  {module} +{offset:x}")


if __name__ == "__main__":
    main()
