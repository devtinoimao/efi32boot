#!/usr/bin/env python3
"""Drive one KVM -cpu host Tiny10 run: inject Enter at the cdboot prompt
and capture screendumps for the whole boot, so we can see exactly what the
guest displays and when."""
import atexit
import os
import signal
import socket
import subprocess
import sys
import time

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(PROJ)

MON_SOCK = "kvm_mon_py.sock"
SERIAL_LOG = "kvm_host_py.log"
STATUS = "kvm_host_drive.status"
if os.path.exists(MON_SOCK):
    os.unlink(MON_SOCK)


def say(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(STATUS, "a") as f:
        f.write(line + "\n")


cmd = [
    "qemu-system-x86_64",
    "-accel", "kvm", "-cpu", "host",
    "-m", "2048", "-smp", "2",
    "-display", "none",
    "-monitor", f"unix:{MON_SOCK},server,nowait",
    "-serial", f"file:{SERIAL_LOG}",
    "-boot", "order=cd",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd",
    "-drive", "if=pflash,format=raw,file=vars/OVMF_VARS.fd",
    "-drive", "format=raw,file=esp_tiny_host.img",
    "-cdrom", os.path.expanduser("~/Downloads/Tiny10 B2.iso"),
]

mon = None
proc = None


def cleanup():
    global proc
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


atexit.register(cleanup)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))


def mon_cmd(c):
    global mon
    if mon is None:
        for _ in range(50):
            if os.path.exists(MON_SOCK):
                break
            time.sleep(0.2)
        mon = socket.socket(socket.AF_UNIX)
        mon.connect(MON_SOCK)
        time.sleep(0.5)
        try:
            mon.setblocking(False)
            mon.recv(65536)  # banner
            mon.setblocking(True)
        except BlockingIOError:
            mon.setblocking(True)
    mon.sendall((c + "\n").encode())
    time.sleep(0.25)
    try:
        mon.setblocking(False)
        r = mon.recv(65536)
        mon.setblocking(True)
        return r
    except BlockingIOError:
        mon.setblocking(True)
        return b""


proc = subprocess.Popen(cmd)
say(f"qemu pid={proc.pid}")

t0 = time.time()
marker_seen = False
shots = 0
# Phase 1: whole-boot screendumps every 3s until the bootmgr marker appears
while time.time() - t0 < 90:
    try:
        data = open(SERIAL_LOG, "rb").read()
    except FileNotFoundError:
        data = b""
    if b"BaudRate/Actual" in data:
        marker_seen = True
        break
    mon_cmd(f"screendump shot_p{shots:02d}.ppm")
    shots += 1
    time.sleep(3)

say(f"marker_seen={marker_seen} after {time.time()-t0:.0f}s")

# Phase 2: hammer Enter + screenshot every 2s for 30s (cdboot prompt window)
for i in range(15):
    mon_cmd("sendkey kp_enter")
    time.sleep(1)
    mon_cmd(f"screendump shot_k{i:02d}.ppm")
    time.sleep(1)

say("enter hammering done")

# Phase 3: let Windows boot; screendump every 15s for 4 more minutes
for i in range(16):
    time.sleep(15)
    mon_cmd(f"screendump shot_w{i:02d}.ppm")
    if proc.poll() is not None:
        say("qemu exited")
        break

say(f"DONE total={time.time()-t0:.0f}s")
