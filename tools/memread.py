"""Read-only inspection of a running Ballest process: no injection, no writes.

Used to measure engine layouts before the host relies on them. The offsets are read from src/host/layout.hpp, so
this tool and the host always agree on what was measured.

    from memread import attach
    e = attach()                      # the running game
    for index, obj in e.objects(): ...
"""
import ctypes
import ctypes.wintypes as wt
import re
import struct
import subprocess
from pathlib import Path

LAYOUT_FILE = Path(__file__).resolve().parents[1] / "src" / "host" / "layout.hpp"
# kName = value; from layout.hpp (hex or decimal constants)
L = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"constexpr \w+ (k\w+) = (0x[0-9A-Fa-f]+|\d+);", LAYOUT_FILE.read_text())}

PROCESS = "Ballest-Win64-Shipping.exe"
PROCESS_VM_READ, PROCESS_QUERY_INFORMATION = 0x0010, 0x0400
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def pid_of(name=PROCESS):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == name.lower():
            return int(parts[1])
    return None


def module_base(pid):
    ps = f"(Get-Process -Id {pid}).MainModule.BaseAddress.ToInt64()"
    return int(subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout.strip())


class Proc:
    def __init__(self, pid):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.h:
            raise OSError(ctypes.get_last_error())

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        n = ctypes.c_size_t()
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size, ctypes.byref(n))
        return buf.raw if ok and n.value == size else None

    def _unpack(self, fmt, addr):
        b = self.read(addr, struct.calcsize(fmt))
        return struct.unpack(fmt, b)[0] if b else None

    def u64(self, a): return self._unpack("<Q", a)
    def i32(self, a): return self._unpack("<i", a)
    def u32(self, a): return self._unpack("<I", a)


class Engine:
    """Names, objects and properties of the running game, using the host's measured layout."""

    def __init__(self, proc, base):
        self.p, self.base = proc, base
        self.pool = base + L["kNamePoolOffsetInExe"]
        self.objects_array = base + L["kGlobalObjectArrayOffsetInExe"]
        self._names = {}

    def name(self, index):
        if index in self._names:
            return self._names[index]
        block = self.p.u64(self.pool + L["kNamePoolBlockListOffset"] + (index >> 16) * 8)
        if not block:
            return None
        entry = block + (index & 0xFFFF) * 2
        header = struct.unpack("<H", self.p.read(entry, 2))[0]
        wide, length = header & 1, header >> 6
        raw = self.p.read(entry + 2, length * (2 if wide else 1))
        self._names[index] = raw.decode("utf-16-le" if wide else "latin-1") if raw else None
        return self._names[index]

    def fname(self, addr):
        index, number = struct.unpack("<Ii", self.p.read(addr, 8))
        n = self.name(index)
        return n if number == 0 else f"{n}_{number - 1}"

    def count(self):
        return self.p.i32(self.objects_array + L["kObjectArrayObjectCountOffset"])

    def objects(self):
        chunks = self.p.u64(self.objects_array + L["kObjectArrayChunkListOffset"])
        n, per = self.count(), L["kObjectArrayItemsPerChunk"]
        for c in range((n + per - 1) // per):
            chunk = self.p.u64(chunks + c * 8)
            size = min(per, n - c * per)
            raw = self.p.read(chunk, size * L["kObjectArrayItemSizeBytes"])
            for i in range(size):
                o = struct.unpack_from("<Q", raw, i * L["kObjectArrayItemSizeBytes"] + L["kObjectArrayItemObjectPointerOffset"])[0]
                if o:
                    yield c * per + i, o

    def obj_name(self, o): return self.fname(o + L["kUObjectNameOffset"])
    def class_of(self, o): return self.p.u64(o + L["kUObjectClassOffset"])
    def outer_of(self, o): return self.p.u64(o + L["kUObjectOuterOffset"])

    def path(self, o):
        parts = []
        while o:
            parts.append(self.obj_name(o))
            o = self.outer_of(o)
        return ".".join(reversed(parts))

    def find_class(self, name):
        for _, o in self.objects():
            if self.obj_name(o) == name and (self.obj_name(self.class_of(o)) or "").endswith("Class"):
                return o
        return None


def props(e, structure):
    """(name, kind, offset, field address) for every property of a class or struct and its supers."""
    out = []
    while structure:
        f = e.p.u64(structure + L["kUStructFirstPropertyOffset"])
        while f:
            field_class = e.p.u64(f + L["kFFieldTypeOffset"])
            kind = e.fname(field_class + L["kFFieldTypeNameOffset"]) if field_class else "?"
            out.append((e.fname(f + L["kFFieldNameOffset"]), kind, e.p.i32(f + L["kFPropertyValueLocationOffset"]), f))
            f = e.p.u64(f + L["kFFieldNextFieldOffset"])
        structure = e.p.u64(structure + L["kUStructParentStructOffset"])
    return out


def prop(e, obj, name):
    """(offset, kind, field address) of a property of an object, or None."""
    for n, kind, off, f in props(e, e.class_of(obj)):
        if n == name:
            return off, kind, f
    return None


def attach():
    pid = pid_of()
    if not pid:
        raise SystemExit("Ballest is not running")
    return Engine(Proc(pid), module_base(pid))
