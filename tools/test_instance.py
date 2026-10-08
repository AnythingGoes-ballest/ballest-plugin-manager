r"""Sandboxed test copies of Ballest, any number at once, next to the player's own game.

    python tools/test_instance.py start N|next [--plugin DIR ...] [--only] [--clean] [--host DLL]
                                    [--plugins DIR|--shared-plugins] [--reseed] [-- extra game args]
    python tools/test_instance.py stop N
    python tools/test_instance.py status [N]
    python tools/test_instance.py play
    python tools/test_instance.py front N|player
    python tools/test_instance.py reload N DIR
    python tools/test_instance.py install-player DIR

`play` starts the player's own game, as normal (not sandboxed, their own saves, online), for when test copies are
running: Steam counts each copy as Ballest running, so Steam's Play button waits for them and then does nothing
(measured: Steam's log goes WaitingPrevProcess -> Completed). Started this way it's the same game, Steam overlay
included.

`front` brings slot N's window, or the player's game, in front of everything (a slot opens behind everything).

`reload N DIR` puts a plugin you're working on into running slot N again: copies the folder in and turns the plugin
off and on (the test command `enable`), which reads its scripts from disk again. It waits for the plugin's "loaded"
line or its errors in the slot's log. The plugin must already be in the slot (started with --plugin DIR).

`install-player DIR` installs a plugin into the player's own game: copies that one folder into the game's plugins
folder (nothing else changes), then, if the game is running, closes it the way its close button does and starts it
again (as `play`), since the game reads plugins only when it starts. It waits for the plugin's "loaded" line or its
errors in the player's host log, then for the main menu, and brings the game in front.

Slot N (1-9) gets:
  * its own data folder, %LOCALAPPDATA%\BallestTest<N> (the copy's LOCALAPPDATA, so the host's
    Ballest\Saved\PluginManager: host.log, test_command.txt, storage, settings) with sandbox.txt in it, so the host
    starts in sandbox mode: the game can't reach the internet, write the player's saves or ghosts, or upload to Steam;
  * its own game saves: the game runs with -userdir=<that folder>\UserDir, so its Saved (SaveGames, Ghosts, Config,
    logs, crashes) is there, seeded the first time (or with --reseed) with a copy of the player's SaveGames, Ghosts,
    UserSavedMaps and UserSavedPlaylists. Without a profile the sandboxed game crashes at start (measured: a stack
    overflow right after its first profile write is refused), so it always gets one. The sandbox lets the copy write
    records only in there;
  * optionally a host build of its own: --host DLL (a build/version.dll) is copied into the slot and the installed host
    loads it in its place (host 0.24.0 and newer), so a host change is tried while the player's game and the other
    slots run the installed host. Without --host the slot runs the installed host;
  * a window that opens behind everything and never takes focus (sandbox mode), 1280x720, windowed, its sound muted;
  * its own plugins: PluginManager\plugins in that folder (copied from the game's plugins folder the first time, or
    from --plugins DIR); --plugin DIR (any number) installs one plugin you're working on into it, as the id its
    folder is named for (a repo folder's "ballest-" left off: ballest-replay-manager is replay-manager);
    --only turns every other plugin off for the session (off.txt), so only yours (and the plugin manager) run;
    --shared-plugins runs the game's own plugins folder instead. A slot keeps its data (storage, runs, settings,
    screenshots) from one use to the next; --clean empties it first (its plugins and the sandbox flag stay);
  * its own launcher lock. Ballest.exe refuses to start while another holds Local\BallestLauncherSingleInstanceV1,
    and the file can't be edited (Steam's DRM stub rejects a changed exe), so it's started suspended and the name is
    changed in its memory (plain UTF-16 in .rdata) to ...T<N> before it runs;
  * its process ids in instance.json, so `stop N` ends that copy and nothing else.

`start` waits until the host log says the sandbox is complete (all of its blocks in place) and exits non-zero, with
the copy stopped, if it doesn't say so.
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import subprocess
import sys
import time
from ctypes import wintypes as wt
from pathlib import Path

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Ballest of Them All")
WIN64 = GAME / "Ballest" / "Binaries" / "Win64"
APP_ID = "3339810"
OLD = "BallestLauncherSingleInstanceV1"
PLAYER_SAVED = Path(os.environ["LOCALAPPDATA"]) / "Ballest" / "Saved"


def slot_root(n):
    return Path(os.environ["LOCALAPPDATA"]) / f"BallestTest{n}"


def data_dir(n):
    return slot_root(n) / "Ballest" / "Saved" / "PluginManager"


k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE),
                ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


class PROCESS_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("Reserved1", ctypes.c_void_p), ("PebBaseAddress", ctypes.c_void_p),
                ("Reserved2", ctypes.c_void_p * 2), ("UniqueProcessId", ctypes.c_void_p), ("Reserved3", ctypes.c_void_p)]


CREATE_SUSPENDED = 0x4
CREATE_UNICODE_ENVIRONMENT = 0x400
k32.CreateProcessW.argtypes = [wt.LPCWSTR, wt.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wt.BOOL, wt.DWORD,
                               ctypes.c_void_p, wt.LPCWSTR, ctypes.POINTER(STARTUPINFOW),
                               ctypes.POINTER(PROCESS_INFORMATION)]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = k32.ReadProcessMemory.argtypes
k32.VirtualProtectEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, ctypes.POINTER(wt.DWORD)]
k32.ResumeThread.argtypes = [wt.HANDLE]
k32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
k32.CloseHandle.argtypes = [wt.HANDLE]
ntdll.NtQueryInformationProcess.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.ULONG, ctypes.POINTER(wt.ULONG)]


def read(h, addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) or got.value != n:
        raise OSError(ctypes.get_last_error(), f"read {addr:#x}")
    return buf.raw


def name_rva(exe):
    """Where the lock's name is once loaded: found in the file (an RVA, so it holds wherever the image goes)."""
    import pefile
    data = exe.read_bytes()
    needle = OLD.encode("utf-16le")
    off = data.find(needle)
    if off < 0 or data.find(needle, off + 1) >= 0:
        sys.exit("the lock's name isn't in Ballest.exe exactly once: the game changed, measure again")
    return pefile.PE(data=data, fast_load=True).get_rva_from_offset(off)


def processes():
    """Every Ballest process: {pid: (parent pid, image name)}."""
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'Ballest*' } | "
                          "ForEach-Object { \"$($_.ProcessId) $($_.ParentProcessId) $($_.Name)\" }"],
                         capture_output=True, text=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) == 3:
            found[int(parts[0])] = (int(parts[1]), parts[2])
    return found


def instance(n):
    try:
        return json.loads((slot_root(n) / "instance.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def command_lines():
    """Every Ballest process's command line: {pid: command line}."""
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'Ballest*' } | "
                          "ForEach-Object { \"$($_.ProcessId) $($_.CommandLine)\" }"],
                         capture_output=True, text=True).stdout
    found = {}
    for line in out.splitlines():
        parts = line.split(maxsplit=1)
        if parts and parts[0].isdigit():
            found[int(parts[0])] = parts[1] if len(parts) > 1 else ""
    return found


def alive(n):
    """The slot's processes still running. A pid alone isn't enough: if the copy died and Windows gave its pid to the
    player's own game, stop() would close it. So the process must also carry this slot's own -userdir (the player's
    game never does)."""
    info = instance(n)
    if not info:
        return []
    now = processes()
    pids = [pid for pid, name in ((info.get("launcher"), "Ballest.exe"), (info.get("game"), "Ballest-Win64-Shipping.exe"))
            if pid and pid in now and now[pid][1].lower() == name.lower()]
    if not pids:
        return []
    lines = command_lines()
    mark = f"ballesttest{n}\\".lower()
    return [pid for pid in pids if mark in lines.get(pid, "").lower().replace("/", "\\")]


user32 = ctypes.WinDLL("user32", use_last_error=True)
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [EnumProc, wt.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]


def close_windows(pid):
    """Asks a process to quit the way the window's close button does (WM_CLOSE to its top-level windows)."""
    sent = []

    def each(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
            sent.append(hwnd)
        return True

    user32.EnumWindows(EnumProc(each), 0)
    return bool(sent)


# The launcher keeps one launch state for every copy (Saved\BallestLaunchState.ini and BallestLauncher\, in the
# player's own LOCALAPPDATA: it doesn't follow the variable or -userdir), the player's game included. A copy ended by
# force is recorded there as ReadyAbnormalExit, one closed before its main menu was up as Aborted with ready=0
# (measured), the kind of start the launcher may answer by changing the graphics mode the next time. So a copy is
# only closed once it's ready (the game writes DirectLightingShadowReadyStatus.txt with its pid at MainMenuReady),
# and closed the way its window's close button does it.
LAUNCHER_STATE = PLAYER_SAVED / "BallestLauncher"


def ready(n, pid):
    """The game's main menu is up: its ready status (one file for every copy, so a later copy's overwrites it) or
    the host's footer buttons on the main menu (placed once the menu's widgets exist)."""
    try:
        text = (LAUNCHER_STATE / "DirectLightingShadowReadyStatus.txt").read_text(encoding="utf-8", errors="replace")
        if f"ProcessId={pid}" in text and "Phase=MainMenuReady" in text:
            return True
    except OSError:
        pass
    try:
        return "footer button 'plugins' placed" in (data_dir(n) / "host.log").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False


# Sandbox mode mutes a copy's sound in Windows (its own audio session), and Windows saves per-app mute by program
# path: the slots run the same Ballest-Win64-Shipping.exe as the user's game, so the user's game started muted too
# (measured 2026-10-03: VT_BOOL true in property 5 of the exe's PolicyConfig entries). Cleared when a slot stops.
AUDIO_STORE = r"Software\Microsoft\Internet Explorer\LowRegistry\Audio\PolicyConfig\PropertyStore"
AUDIO_PROPS = "{219ED5A0-9CBF-4F3A-B927-37C9E5C5F14F}"


def unmute_saved():
    """Clears the mute Windows saved for the game's exe (on every output device); returns how many were cleared."""
    import winreg
    cleared = 0
    try:
        store = winreg.OpenKey(winreg.HKEY_CURRENT_USER, AUDIO_STORE)
    except OSError:
        return 0
    with store:
        for i in range(winreg.QueryInfoKey(store)[0]):
            name = winreg.EnumKey(store, i)
            try:
                with winreg.OpenKey(store, name) as entry:
                    if "\\ballest-win64-shipping.exe" not in str(winreg.QueryValueEx(entry, "")[0]).lower():
                        continue
                with winreg.OpenKey(store, name + "\\" + AUDIO_PROPS, 0, winreg.KEY_READ | winreg.KEY_SET_VALUE) as props:
                    value, kind = winreg.QueryValueEx(props, "5")
                    if kind == winreg.REG_BINARY and len(value) >= 10 and value[0] == 11 and value[8:10] != b"\0\0":
                        winreg.SetValueEx(props, "5", 0, winreg.REG_BINARY, value[:8] + b"\0\0" + value[10:])
                        cleared += 1
            except OSError:
                continue
    return cleared


def stop(n, grace=20, wait_ready=90):
    """Ends the slot: once its game is ready, closed as a player would; forced only if it hangs."""
    pids = alive(n)
    info = instance(n) or {}
    if info.get("game") in pids and not info.get("ready"):
        end = time.time() + wait_ready
        while time.time() < end and info["game"] in alive(n) and not ready(n, info["game"]):
            time.sleep(0.5)
    if info.get("game") in pids and close_windows(info["game"]):
        end = time.time() + grace
        while time.time() < end and alive(n):
            time.sleep(0.25)
    for pid in alive(n):
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
    for _ in range(40):
        if not alive(n):
            break
        time.sleep(0.25)
    left = alive(n)
    if not left:
        release(n)
        unmute_saved()
    print(f"slot {n}: " + ("stopped " + ", ".join(map(str, pids)) if pids and not left else
                           "still running " + ", ".join(map(str, left)) if left else "not running"))
    return not left


def user_dir(n):
    return slot_root(n) / "UserDir"


SEEDED = ("SaveGames", "Ghosts", "UserSavedMaps", "UserSavedPlaylists")


def prepare(n, plugins_from, shared, reseed):
    data = data_dir(n)
    data.mkdir(parents=True, exist_ok=True)
    saved = user_dir(n) / "Saved"
    if reseed or not (saved / "SaveGames").exists():
        for name in SEEDED:
            if (saved / name).exists():
                shutil.rmtree(saved / name)
            if (PLAYER_SAVED / name).exists():
                shutil.copytree(PLAYER_SAVED / name, saved / name, ignore=shutil.ignore_patterns("steam_autocloud.vdf"))
    (data / "sandbox.txt").write_text("sandbox mode for test copy %d (tools/test_instance.py)\n" % n, encoding="utf-8")
    own = data / "plugins"
    if shared:
        if own.exists():
            sys.exit(f"--shared-plugins: slot {n} has its own plugins folder ({own}); remove it first")
        return
    if plugins_from:
        if own.exists():
            shutil.rmtree(own)
        shutil.copytree(plugins_from, own)
    elif not own.exists():
        shutil.copytree(WIN64 / "plugins", own, ignore=shutil.ignore_patterns("DISABLED"))


def clean(n):
    data = data_dir(n)
    for item in data.iterdir() if data.exists() else []:
        if item.name in ("plugins", "sandbox.txt"):
            continue
        shutil.rmtree(item) if item.is_dir() else item.unlink()


def only(n, ids):
    """Every plugin in the slot but these (and the plugin manager, which can't be turned off) turned off."""
    folder = data_dir(n) / "plugins"
    off = sorted(p.name for p in folder.iterdir() if p.is_dir() and p.name not in set(ids) | {"plugin-manager"})
    (data_dir(n) / "off.txt").write_text("".join(i + "\n" for i in off), encoding="utf-8")


def install_plugins(n, folders):
    ids = []
    for folder in folders or []:
        src, pid = plugin_id(folder)
        copy_plugin(src, data_dir(n) / "plugins" / pid)
        print(f"slot {n}: plugin {pid} from {src}")
        ids.append(pid)
    return ids


def launch(n, args):
    exe = GAME / "Ballest.exe"
    rva = name_rva(exe)
    new = (OLD[:-2] + f"T{n}").encode("utf-16le")
    env = dict(os.environ, LOCALAPPDATA=str(slot_root(n)), SteamAppId=APP_ID, SteamGameId=APP_ID)
    block = "".join(f"{k}={v}\0" for k, v in sorted(env.items(), key=lambda kv: kv[0].upper())) + "\0"
    cmd = ctypes.create_unicode_buffer(" ".join([f'"{exe}"'] + [f'"{a}"' if " " in a else a for a in args]))
    si = STARTUPINFOW(cb=ctypes.sizeof(STARTUPINFOW))
    pi = PROCESS_INFORMATION()
    if not k32.CreateProcessW(str(exe), cmd, None, None, False, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                              ctypes.create_unicode_buffer(block), str(GAME), ctypes.byref(si), ctypes.byref(pi)):
        raise OSError(ctypes.get_last_error(), "CreateProcessW")
    try:
        pbi = PROCESS_BASIC_INFORMATION()
        if ntdll.NtQueryInformationProcess(pi.hProcess, 0, ctypes.byref(pbi), ctypes.sizeof(pbi), None) != 0:
            raise OSError("NtQueryInformationProcess")
        base = int.from_bytes(read(pi.hProcess, pbi.PebBaseAddress + 0x10, 8), "little")  # PEB.ImageBaseAddress
        addr = base + rva
        if read(pi.hProcess, addr, len(new)) != OLD.encode("utf-16le"):
            raise OSError("the lock's name isn't where the file says")
        old_prot = wt.DWORD()
        if not k32.VirtualProtectEx(pi.hProcess, ctypes.c_void_p(addr), len(new), 0x04, ctypes.byref(old_prot)):
            raise OSError(ctypes.get_last_error(), "VirtualProtectEx")
        if not k32.WriteProcessMemory(pi.hProcess, ctypes.c_void_p(addr), new, len(new), None):
            raise OSError(ctypes.get_last_error(), "WriteProcessMemory")
        k32.VirtualProtectEx(pi.hProcess, ctypes.c_void_p(addr), len(new), old_prot.value, ctypes.byref(wt.DWORD()))
        if read(pi.hProcess, addr, len(new)) != new:
            raise OSError("the new name didn't stick")
    except Exception:
        k32.TerminateProcess(pi.hProcess, 1)
        raise
    k32.ResumeThread(pi.hThread)
    k32.CloseHandle(pi.hThread)
    k32.CloseHandle(pi.hProcess)
    return pi.dwProcessId


DEFAULT_ARGS = ["-windowed", "-ResX=1280", "-ResY=720"]      # sound plays, muted by the sandbox (Draw::Sound works)


def claim_file(n):
    return Path(os.environ["LOCALAPPDATA"]) / f"BallestTest{n}.claim"


def claim(n):
    """Takes slot n for this session, atomically (two sessions starting at once can't both get it). A claim left
    behind by a session that never stopped its slot is stale once the slot isn't running and it's 5 minutes old."""
    f = claim_file(n)
    try:
        if f.exists() and not alive(n) and time.time() - f.stat().st_mtime > 300:
            f.unlink()
        fd = os.open(f, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError:
        return False
    except OSError:
        return False
    os.write(fd, str(os.getpid()).encode())
    os.close(fd)
    return True


def release(n):
    claim_file(n).unlink(missing_ok=True)


def set_host(n, dll):
    own = data_dir(n) / "host" / "ballest-host.dll"
    if dll:
        own.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(dll, own)
        print(f"slot {n}: its own host build from {Path(dll).resolve()}")
    elif own.exists():
        own.unlink()


def start(n, args, plugins_from, shared, timeout, reseed=False, plugin_dirs=None, claimed=False, wipe=False, only_mine=False,
          host=None):
    if not claimed and (alive(n) or not claim(n)):
        sys.exit(f"slot {n} is in use ({', '.join(map(str, alive(n))) or 'being started'}); pick another (status, or start next)")
    try:
        code = start_claimed(n, args, plugins_from, shared, timeout, reseed, plugin_dirs, wipe, only_mine, host)
    except BaseException:
        if not alive(n):
            release(n)
        raise
    if code != 0:
        release(n)
    return code


def start_claimed(n, args, plugins_from, shared, timeout, reseed, plugin_dirs, wipe, only_mine, host):
    if not (WIN64 / "version.dll").exists():
        sys.exit("no plugin host installed (Win64\\version.dll)")
    if wipe:
        clean(n)
    prepare(n, plugins_from, shared, reseed)
    if (plugin_dirs or only_mine) and shared:
        sys.exit("--plugin and --only work on the slot's own plugins folder; not with --shared-plugins")
    set_host(n, host)
    ids = install_plugins(n, plugin_dirs)
    if only_mine:
        only(n, ids)
    elif (data_dir(n) / "off.txt").exists() and plugin_dirs:
        # a plugin you're testing is never left turned off by an earlier session's --only
        kept = [l for l in (data_dir(n) / "off.txt").read_text(encoding="utf-8").split() if l not in ids]
        (data_dir(n) / "off.txt").write_text("".join(i + "\n" for i in kept), encoding="utf-8")
    log = data_dir(n) / "host.log"
    if log.exists():
        log.unlink()
    args = DEFAULT_ARGS + [a for a in args if not a.lower().startswith("-userdir")] + [f"-userdir={user_dir(n)}"]
    launcher = launch(n, args)
    info = {"slot": n, "launcher": launcher, "game": None, "started": time.time(), "args": args}
    (slot_root(n) / "instance.json").write_text(json.dumps(info), encoding="utf-8")

    deadline = time.time() + timeout
    status = None
    while time.time() < deadline:
        now = processes()
        if launcher not in now:
            print(f"slot {n}: the launcher ({launcher}) exited before the game was up")
            stop(n)
            return 1
        if not info["game"]:
            game = [pid for pid, (parent, name) in now.items() if parent == launcher and name.lower() == "ballest-win64-shipping.exe"]
            if game:
                info["game"] = game[0]
                (slot_root(n) / "instance.json").write_text(json.dumps(info), encoding="utf-8")
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        # Any sandbox error (a block not in place, an interface not seen) stops the copy. Then the host's own verdict,
        # its full status: the blocks in the game's own Steam interfaces included (2026-10-03: a teleported finish
        # reached the real leaderboard while the old check, which only read the flat accessors, said complete).
        if "Steam never started" in text or re.search(r"\[error\] \[host\] sandbox:", text):
            status = "incomplete (" + (re.findall(r"\[error\] \[host\] (sandbox:[^\n]*)", text) or ["Steam never started"])[0][:160] + ")"
            break
        # The verdict is logged again as the game asks for its interfaces: wait for complete (errors stop it above).
        m = re.search(r"sandbox: status on, (complete)", text)
        if host and "sandbox mode is on" in text and "running the copy's own host build" not in text and "plugins from" in text:
            status = "running the installed host, not --host (the installed one is older than 0.24.0's loader)"
            break
        if m:
            status = "complete" if m.group(1) == "complete" else "incomplete: " + re.search(r"sandbox: status ([^\n]*)", text).group(1)[:300]
            break
        if info["game"] and "first frame on the game thread" in text and "sandbox mode is on" not in text:
            status = "off"
            break
        time.sleep(0.5)
    if status != "complete":
        print(f"slot {n}: sandbox {status or 'not confirmed in time'}; stopping it")
        stop(n)
        return 1
    while time.time() < deadline and info["game"] in alive(n) and not ready(n, info["game"]):
        time.sleep(0.5)
    info["ready"] = bool(info["game"]) and ready(n, info["game"])
    (slot_root(n) / "instance.json").write_text(json.dumps(info), encoding="utf-8")
    if not info["ready"]:
        print(f"slot {n}: the game's main menu didn't come up in time; stopping it")
        stop(n)
        return 1
    print(f"slot {n}: started: sandbox complete, main menu up")
    print(json.dumps({"slot": n, "launcher": launcher, "game": info["game"], "data": str(data_dir(n)),
                      "plugins": str(WIN64 / "plugins" if shared else data_dir(n) / "plugins")}))
    return 0


PLAYER_LOG = PLAYER_SAVED / "PluginManager" / "host.log"


def plugin_id(folder):
    """The id a plugin folder installs as: its name, a repo folder's "ballest-" left off."""
    src = Path(folder).resolve()
    if not (src / "info.toml").exists():
        sys.exit(f"{folder}: no info.toml there")
    return src, src.name[len("ballest-"):] if src.name.startswith("ballest-") else src.name


def copy_plugin(src, dest):
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns(".git", ".github", "*.md"))


def player_pids():
    """The player's own game: {pid: image name} of the Ballest processes that aren't any slot's."""
    slots = {pid for n in range(1, 10) for pid in alive(n)}
    return {pid: name for pid, (_, name) in processes().items() if pid not in slots}


def front(pid):
    """Brings the process's window in front (WScript's AppActivate gets past Windows' foreground lock)."""
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          f"(New-Object -ComObject WScript.Shell).AppActivate({int(pid)})"], capture_output=True, text=True)
    return out.stdout.strip() == "True"


def plugin_lines(text, pid):
    """The log's lines about the plugin: its own, and its compiler errors."""
    return [l for l in text.splitlines() if f"[{pid}]" in l or ("[compiler]" in l and pid in l)]


def wait_plugin(log, pid, since_line, timeout):
    """Waits for the plugin's "loaded" line or an error of its after line since_line of the log; prints its lines.
    Returns whether it loaded without errors."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            lines = plugin_lines("\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[since_line:]), pid)
        except OSError:
            lines = []
        errors = [l for l in lines if "[error]" in l]
        if errors or any(f"[{pid}] loaded" in l for l in lines):
            time.sleep(2)                   # its errors right after loading, too
            lines = plugin_lines("\n".join(log.read_text(encoding="utf-8", errors="replace").splitlines()[since_line:]), pid)
            for l in lines:
                print("  " + l)
            return not any("[error]" in l for l in lines)
        time.sleep(0.5)
    print(f"  nothing from {pid} in {log} in {timeout} s")
    return False


def reload(n, folder):
    if not alive(n):
        sys.exit(f"slot {n} isn't running")
    src, pid = plugin_id(folder)
    dest = data_dir(n) / "plugins" / pid
    if not dest.exists():
        sys.exit(f"slot {n} doesn't have {pid}: start it with --plugin {folder}")
    copy_plugin(src, dest)
    log = data_dir(n) / "host.log"
    before = len(log.read_text(encoding="utf-8", errors="replace").splitlines())
    for on in ("0", "1"):
        (data_dir(n) / "test_command.txt").write_text(f"enable {pid} {on}", encoding="utf-8")
        deadline = time.time() + 15
        reply = ""
        while time.time() < deadline and not reply:
            time.sleep(0.25)
            text = log.read_text(encoding="utf-8", errors="replace")
            reply = next((l for l in text.splitlines()[before:] if f"enable {pid} {on}" in l), "")
        if not reply:
            sys.exit(f"slot {n}: no answer to enable {pid} {on}")
        if "refused" in reply:
            sys.exit(f"slot {n}: {reply.strip()}")
    ok = wait_plugin(log, pid, before, 30)
    print(f"slot {n}: {pid} reloaded from {src}" + ("" if ok else " (see its errors above)"))
    return 0 if ok else 1


def install_player(folder, timeout=180):
    src, pid = plugin_id(folder)
    dest = WIN64 / "plugins" / pid
    running = player_pids()
    game = [p for p, name in running.items() if name.lower() == "ballest-win64-shipping.exe"]
    if game:
        print(f"closing the player's game ({game[0]}) to install {pid}")
        close_windows(game[0])
        deadline = time.time() + 30
        while time.time() < deadline and player_pids():
            time.sleep(0.5)
        for p in player_pids():
            subprocess.run(["taskkill", "/PID", str(p), "/F"], capture_output=True)
        time.sleep(2)
    copy_plugin(src, dest)
    print(f"installed {pid} from {src} into {dest}")
    if not game:
        print("the player's game wasn't running: it loads the plugin the next time it starts")
        return 0
    started = time.time()
    play()
    while time.time() < started + timeout:      # the host starts a new log
        try:
            if PLAYER_LOG.stat().st_mtime > started and "plugin host" in PLAYER_LOG.read_text(encoding="utf-8", errors="replace"):
                break
        except OSError:
            pass
        time.sleep(0.5)
    ok = wait_plugin(PLAYER_LOG, pid, 0, max(10, int(started + timeout - time.time())))
    while time.time() < started + timeout and "footer button 'plugins' placed" not in PLAYER_LOG.read_text(encoding="utf-8", errors="replace"):
        time.sleep(0.5)
    game = [p for p, name in player_pids().items() if name.lower() == "ballest-win64-shipping.exe"]
    if game:
        front(game[0])
    print(f"the player's game is up again with {pid}" + ("" if ok else " (see its errors above)"))
    return 0 if ok else 1


def play():
    now = processes()
    slots = {pid for n in range(1, 10) for pid in alive(n)}
    if any(pid not in slots and name.lower() == "ballest.exe" for pid, (_, name) in now.items()):
        sys.exit("the player's game is already running")
    env = dict(os.environ, SteamAppId=APP_ID, SteamGameId=APP_ID)
    proc = subprocess.Popen([str(GAME / "Ballest.exe")], cwd=str(GAME), env=env, creationflags=0x00000008)  # detached
    print(f"started the player's game (launcher {proc.pid})")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("start")
    s.add_argument("slot", help="1-9, or next: the first free one")
    s.add_argument("--plugins", help="copy this folder in as the slot's plugins (replacing what it had)")
    s.add_argument("--plugin", action="append", help="a plugin folder you're working on, installed into the slot")
    s.add_argument("--only", action="store_true", help="turn every other plugin off (only --plugin ones run)")
    s.add_argument("--clean", action="store_true", help="empty the slot's data first (storage, runs, settings, shots)")
    s.add_argument("--host", help="a host build (build/version.dll) for this slot only")
    s.add_argument("--shared-plugins", action="store_true", help="run the game's own plugins folder")
    s.add_argument("--timeout", type=int, default=120)
    s.add_argument("--reseed", action="store_true", help="copy the player's saves in again")
    s.add_argument("args", nargs="*", help="extra game arguments (after --)")
    t = sub.add_parser("stop")
    t.add_argument("slot", type=int, choices=range(1, 10))
    sub.add_parser("play")
    f = sub.add_parser("front")
    f.add_argument("which", help="a slot (1-9), or player")
    r = sub.add_parser("reload")
    r.add_argument("slot", type=int, choices=range(1, 10))
    r.add_argument("plugin", help="the plugin folder")
    i = sub.add_parser("install-player")
    i.add_argument("plugin", help="the plugin folder")
    u = sub.add_parser("status")
    u.add_argument("slot", type=int, nargs="?", choices=range(1, 10))
    a = ap.parse_args()
    if a.cmd == "start":
        if a.slot == "next":
            for n in range(1, 10):
                if not alive(n) and claim(n):
                    sys.exit(start(n, a.args, a.plugins, a.shared_plugins, a.timeout, a.reseed, a.plugin, claimed=True,
                                   wipe=a.clean, only_mine=a.only, host=a.host))
            sys.exit("no free slot")
        if not a.slot.isdigit() or not 1 <= int(a.slot) <= 9:
            sys.exit("slot: 1-9 or next")
        sys.exit(start(int(a.slot), a.args, a.plugins, a.shared_plugins, a.timeout, a.reseed, a.plugin,
                       wipe=a.clean, only_mine=a.only, host=a.host))
    if a.cmd == "play":
        return play()
    if a.cmd == "front":
        if a.which == "player":
            game = [p for p, name in player_pids().items() if name.lower() == "ballest-win64-shipping.exe"]
        elif a.which.isdigit() and 1 <= int(a.which) <= 9:
            game = [(instance(int(a.which)) or {}).get("game")] if alive(int(a.which)) else []
        else:
            sys.exit("front: a slot (1-9) or player")
        if not game or not game[0]:
            sys.exit(f"{a.which}: no game running")
        sys.exit(0 if front(game[0]) else 1)
    if a.cmd == "reload":
        sys.exit(reload(a.slot, a.plugin))
    if a.cmd == "install-player":
        sys.exit(install_player(a.plugin))
    if a.cmd == "stop":
        sys.exit(0 if stop(a.slot) else 1)
    for n in ([a.slot] if a.slot else range(1, 10)):
        pids = alive(n)
        state = "running " + ", ".join(map(str, pids)) if pids else "being started" if claim_file(n).exists() else "free"
        print(f"slot {n}: {state}  ({data_dir(n)})")


if __name__ == "__main__":
    main()
