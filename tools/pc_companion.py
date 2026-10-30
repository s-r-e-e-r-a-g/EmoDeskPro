#!/usr/bin/env python3
"""EmoDesk PC companion: sends CPU, RAM, GPU, temperatures, now playing and
idle/lock state to EmoDesk every few seconds (the "PC" card, and the screen
turns off while your PC is locked).

    pip install psutil          # recommended (CPU, RAM, temperatures)
    python3 pc_companion.py --host emo.local [--pin 1234]

Works on Windows, macOS and Linux. Everything except CPU/RAM is best effort:
a missing tool simply leaves that field empty.
"""
import argparse
import json
import platform
import shutil
import subprocess
import sys
import time
import urllib.request

try:
    import psutil
except ImportError:  # still works, just without CPU/RAM numbers
    psutil = None

SYSTEM = platform.system()


def run(cmd, timeout=2):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout.strip()
    except Exception:
        return ""


def cpu_temp():
    if not psutil or not hasattr(psutil, "sensors_temperatures"):
        return 0
    try:
        temps = psutil.sensors_temperatures() or {}
        for key in ("coretemp", "k10temp", "cpu_thermal", "acpitz", "zenpower"):
            if temps.get(key):
                return max(t.current for t in temps[key])
    except Exception:
        pass
    return 0


def gpu():
    if not shutil.which("nvidia-smi"):
        return 0, 0
    out = run(["nvidia-smi", "--query-gpu=utilization.gpu,temperature.gpu", "--format=csv,noheader,nounits"])
    try:
        u, t = out.splitlines()[0].split(",")
        return float(u), float(t)
    except Exception:
        return 0, 0


def now_playing():
    """(title, artist, playing)"""
    if SYSTEM == "Linux" and shutil.which("playerctl"):
        status = run(["playerctl", "status"])
        if status in ("Playing", "Paused"):
            return run(["playerctl", "metadata", "title"]), run(["playerctl", "metadata", "artist"]), status == "Playing"
    if SYSTEM == "Darwin":
        for app in ("Spotify", "Music"):
            script = (f'if application "{app}" is running then tell application "{app}" to '
                      'if player state is playing or player state is paused then '
                      'return (name of current track) & "\\n" & (artist of current track) & "\\n" & (player state as string)')
            out = run(["osascript", "-e", script])
            parts = out.split("\n")
            if len(parts) == 3:
                return parts[0], parts[1], parts[2] == "playing"
    if SYSTEM == "Windows":
        try:  # pip install winsdk
            import asyncio
            from winsdk.windows.media.control import GlobalSystemMediaTransportControlsSessionManager as M

            async def get():
                s = (await M.request_async()).get_current_session()
                if not s:
                    return "", "", False
                p = await s.try_get_media_properties_async()
                playing = s.get_playback_info().playback_status == 4
                return p.title or "", p.artist or "", playing
            return asyncio.run(get())
        except Exception:
            pass
    return "", "", False


def idle_and_locked():
    """(idle seconds, screen locked)"""
    try:
        if SYSTEM == "Windows":
            import ctypes

            class LII(ctypes.Structure):
                _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]
            lii = LII()
            lii.cbSize = ctypes.sizeof(LII)
            ctypes.windll.user32.GetLastInputInfo(ctypes.byref(lii))
            idle = (ctypes.windll.kernel32.GetTickCount() - lii.dwTime) // 1000
            locked = ctypes.windll.user32.OpenInputDesktop(0, False, 0x0100) == 0
            return idle, locked
        if SYSTEM == "Darwin":
            out = run(["ioreg", "-c", "IOHIDSystem"])
            idle = 0
            for line in out.splitlines():
                if "HIDIdleTime" in line:
                    idle = int(line.split("=")[-1]) // 1_000_000_000
                    break
            locked = "CGSSessionScreenIsLocked" in run(["python3", "-c",
                "import Quartz;print(Quartz.CGSessionCopyCurrentDictionary())"])
            return idle, locked
        if SYSTEM == "Linux":
            idle = 0
            if shutil.which("xprintidle"):
                idle = int(run(["xprintidle"]) or 0) // 1000
            locked = "true" in run(["loginctl", "show-session", "self", "-p", "LockedHint"]).lower() or \
                     "true" in run(["gdbus", "call", "--session", "--dest", "org.gnome.ScreenSaver", "--object-path",
                                    "/org/gnome/ScreenSaver", "--method", "org.gnome.ScreenSaver.GetActive"])
            return idle, locked
    except Exception:
        pass
    return 0, False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="emo.local", help="EmoDesk address (default emo.local)")
    ap.add_argument("--pin", default="", help="dashboard PIN, if you set one")
    ap.add_argument("--every", type=float, default=3, help="seconds between updates")
    a = ap.parse_args()
    url = f"http://{a.host}/api/pc"
    if psutil:
        psutil.cpu_percent(None)
    print(f"Sending to {url} every {a.every:g}s. Ctrl+C to stop.")
    fails = 0
    while True:
        g, gt = gpu()
        song, artist, playing = now_playing()
        idle, locked = idle_and_locked()
        body = {
            "cpu": psutil.cpu_percent(None) if psutil else 0,
            "ram": psutil.virtual_memory().percent if psutil else 0,
            "cpuT": cpu_temp(), "gpu": g, "gpuT": gt,
            "song": song[:47], "artist": artist[:31], "playing": playing,
            "idle": idle, "locked": locked,
        }
        req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json", "X-Emo-Pin": a.pin})
        try:
            urllib.request.urlopen(req, timeout=4).read()
            if fails:
                print("Connected again.")
            fails = 0
        except Exception as e:
            fails += 1
            if fails in (1, 10) or fails % 100 == 0:
                print(f"Could not reach EmoDesk at {a.host}: {e}", file=sys.stderr)
        time.sleep(a.every)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
