#!/usr/bin/env python3
"""Generic boot capture: run QEMU with a monitor, screendump periodically,
inject Enter presses around the expected cdboot window.
Usage: boot_capture.py <accel> <cpu> <esp> <iso> <tag> [grubfm]
"""
import os
import socket
import subprocess
import sys
import time

accel, cpu, esp, iso, tag = sys.argv[1:6]
grubfm = len(sys.argv) > 6 and sys.argv[6] == "grubfm"

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(PROJ)
MON_SOCK = f"mon_{tag}.sock"
if os.path.exists(MON_SOCK):
    os.unlink(MON_SOCK)

cmd = [
    "qemu-system-x86_64",
    "-accel", accel, "-cpu", cpu,
    "-m", "2048", "-smp", "2",
    "-display", "none",
    "-monitor", f"unix:{MON_SOCK},server,nowait",
    "-serial", f"file:{tag}.log",
    "-boot", "order=cd",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd",
    "-drive", "if=pflash,format=raw,file=vars/OVMF_VARS.fd",
    "-drive", "format=raw,file=" + esp,
    "-cdrom", iso,
]
if grubfm:
    cmd += [
        "-drive", "if=none,id=grubfm,file=/home/abokaborda/Downloads/grubfm_multiarch.iso,media=cdrom",
        "-device", "ide-cd,bus=ide.1,unit=1,drive=grubfm",
    ]

mon = None


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
            mon.recv(65536)
            mon.setblocking(True)
        except BlockingIOError:
            mon.setblocking(True)
    mon.sendall((c + "\n").encode())
    time.sleep(0.2)
    try:
        mon.setblocking(False)
        r = mon.recv(65536)
        mon.setblocking(True)
        return r
    except BlockingIOError:
        mon.setblocking(True)
        return b""


proc = subprocess.Popen(cmd)
print(f"[{time.strftime('%H:%M:%S')}] pid={proc.pid} tag={tag}", flush=True)

try:
    t0 = time.time()
    n = 0
    enter_sent = 0
    while time.time() - t0 < 240:
        time.sleep(5)
        mon_cmd(f"screendump shot_{tag}_{n:03d}.ppm")
        n += 1
        # from 120s onward, send Enter every other cycle (cdboot window)
        if time.time() - t0 > 40 and enter_sent < 20:
            mon_cmd("sendkey kp_enter")
            enter_sent += 1
        if proc.poll() is not None:
            print("qemu exited", flush=True)
            break
finally:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
print(f"[{time.strftime('%H:%M:%S')}] DONE shots={n}", flush=True)
