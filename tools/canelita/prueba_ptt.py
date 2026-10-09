#!/usr/bin/env python3
# Copyright (c) 2026 Juan Manuel Fraga.
# SPDX-License-Identifier: Apache-2.0
"""Prueba del push-to-talk SIN dedo: aprieta el botón por USB (d), la Mac habla
con say, lo suelta (u) y muestra el log del turno. Uso: python prueba_ptt.py "frase"."""
import glob, re, subprocess, sys, time
import serial
frase = sys.argv[1] if len(sys.argv) > 1 else "Hola Canela, ¿qué hora es?"
p = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
F = re.compile(r"canelita|muse\.voice|voice|reply|PTT|record|heard|E \(|W \(|overflow|Guru|LISTEN", re.I)
with serial.Serial(p, 115200, timeout=0.2) as s:
    s.reset_input_buffer()
    s.write(b"d"); time.sleep(0.6)                       # aprieta el botón
    subprocess.run(["say", "-v", "Paulina", "-r", "175", frase])
    time.sleep(0.6); s.write(b"u")                       # suelta
    buf, fin = b"", time.time() + 50
    while time.time() < fin:
        buf += s.read(4096)
        *ls, buf = buf.split(b"\n")
        for l in ls:
            t = re.sub(r"\x1b\[[0-9;]*m", "", l.decode("utf-8", "replace")).strip()
            if t and F.search(t) and "heartbeat" not in t and "high water" not in t: print("  " + t[:200])
