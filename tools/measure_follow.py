"""Waits for a replay in the running game, then samples the view in default mode and in follow 3D.

Read-only apart from switching the Replay Manager's camera dropdown through the host's test channel.
"""
import os
import re
import struct
import time
from pathlib import Path

from memread import L, attach, props

DATA = Path(os.environ["LOCALAPPDATA"]) / "Ballest" / "Saved" / "PluginManager"
LOG = DATA / "host.log"


def log_lines():
    return LOG.read_text(encoding="utf-8", errors="replace").splitlines()


def command(text):
    (DATA / "test_command.txt").write_text(text, encoding="utf-8")


start_lines = len(log_lines())
print("waiting for a replay (up to 10 minutes)")
end = time.time() + 600
while time.time() < end:
    new = log_lines()[start_lines:]
    if any("replay started" in l for l in new):
        break
    time.sleep(1)
else:
    print("no replay started")
    raise SystemExit(1)
time.sleep(2)

e = attach()
p = e.p


def prop_info(cls, name):
    for n, kind, off, f in props(e, cls):
        if n == name:
            return off, f
    return None, None


def struct_of(field):
    return p.u64(field + L["kFStructPropertyStructTypeOffset"])


def boolval(obj, cls, name):
    off, f = prop_info(cls, name)
    if off is None:
        return None
    return bool(p.read(obj + off + p.read(f + L["kFBoolPropertyByteIndexOffset"], 1)[0], 1)[0] & p.read(f + L["kFBoolPropertyBitMaskOffset"], 1)[0])


freecam_cls = e.find_class("BP_FreeCam_C")
manager_cls = e.find_class("PlayerCameraManager")
freecam = next(o for _, o in e.objects() if e.class_of(o) == freecam_cls and not e.obj_name(o).startswith("Default__"))
off, _ = prop_info(freecam_cls, "AC_GhostFollowCam")
cam = p.u64(freecam + off)
cam_cls = e.class_of(cam)
off, _ = prop_info(cam_cls, "RuntimeManagedSpringArm")
arm = p.u64(cam + off)
arm_cls = e.class_of(arm)
rr_off, _ = prop_info(arm_cls, "RelativeRotation")
rs_off, _ = prop_info(cam_cls, "RotationSource")

# The live camera manager (the one whose outer chain is in the map) and its cached view.
manager = None
for _, o in e.objects():
    c = e.class_of(o)
    k = c
    while k:
        if k == manager_cls:
            break
        k = p.u64(k + L["kUStructParentStructOffset"])
    if k == manager_cls and not e.obj_name(o).startswith("Default__") and "PersistentLevel" in e.path(o):
        manager = o
        break
cache_off, cache_field = prop_info(e.class_of(manager), "CameraCachePrivate")
cache_struct = struct_of(cache_field)
pov_off, pov_field = [(x[2], x[3]) for x in props(e, cache_struct) if x[0] == "POV"][0]
pov_struct = struct_of(pov_field)
loc_off = [x[2] for x in props(e, pov_struct) if x[0] == "Location"][0]
rot_off = [x[2] for x in props(e, pov_struct) if x[0] == "Rotation"][0]
base = manager + cache_off + pov_off


def sample(label, seconds):
    rows = []
    t0 = time.time()
    while time.time() - t0 < seconds:
        loc = struct.unpack("<3d", p.read(base + loc_off, 24))
        rot = struct.unpack("<3d", p.read(base + rot_off, 24))
        arm_rot = struct.unpack("<3d", p.read(arm + rr_off, 24))
        rows.append((time.time() - t0, loc, rot, arm_rot))
        time.sleep(0.25)
    print(f"--- {label}: RotationSource={p.read(cam + rs_off, 1)[0]} "
          f"mouse={boolval(cam, cam_cls, 'bApplyPlayerControlRotationToManagedSpringArm')} "
          f"armPawnControl={boolval(arm, arm_cls, 'bUsePawnControlRotation')} "
          f"driveOwner={boolval(cam, cam_cls, 'bDriveOwnerActor')}")
    for t, loc, rot, arm_rot in rows[::4]:
        print(f"  t={t:4.1f} view at ({loc[0]:8.0f},{loc[1]:8.0f},{loc[2]:7.0f}) pitch {rot[0]:6.1f} yaw {rot[1]:7.1f}"
              f" | arm rel pitch {arm_rot[0]:6.1f} yaw {arm_rot[1]:6.1f}")
    moved = sum(abs(a - b) for a, b in zip(rows[0][1], rows[-1][1]))
    turned = abs(rows[-1][2][1] - rows[0][2][1])
    print(f"  view moved {moved:.0f} units, yaw changed {turned:.1f} deg")


command("select default 0")
time.sleep(1)
sample("default", 4)
command("select default 1")
time.sleep(1)
sample("follow 3d", 6)
command("select default 0")
print("\nhost log during the test:")
for l in log_lines()[start_lines:]:
    if re.search(r"camera|replay", l):
        print("  " + l[:160])
