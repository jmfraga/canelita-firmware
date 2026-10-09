#!/usr/bin/env python3
# Copyright (c) 2026 Juan Manuel Fraga.
# SPDX-License-Identifier: Apache-2.0
"""Manda comandos a la consola de canelita y muestra lo que contesta.

  python consola.py ">canelita.show"            # líneas que empiezan con '>' son comandos
  python consola.py d --esperar 1 u             # 'd'/'u' = apretar/soltar el botón de hablar
  python consola.py --log 20                    # sólo escuchar 20 s (filtra lo relevante)
Cada argumento se manda tal cual (un comando '>' lleva su salto de línea).
--esperar N pausa N segundos entre envíos. Imprime las líneas '@…' y el log
de canelita/voz; nunca hay secretos en esa salida.
"""
import glob, re, sys, time
import serial

FILTRO = re.compile(r"^@|canelita|muse\.voice|link\.voice|voice|reply|wifi|E \(|W \(", re.I)


def main(argv):
    puerto = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
    log_s, esperar, envios, i = 3.0, 0.0, [], 0
    while i < len(argv):
        if argv[i] == "--log": log_s = float(argv[i + 1]); i += 2; continue
        if argv[i] == "--esperar": envios.append(("pausa", float(argv[i + 1]))); i += 2; continue
        envios.append(("tx", argv[i])); i += 1
    with serial.Serial(puerto, 115200, timeout=0.2) as s:
        buf = b""
        def drenar(seg):
            nonlocal buf
            fin = time.time() + seg
            while time.time() < fin:
                buf += s.read(4096)
                *lineas, buf = buf.split(b"\n")
                for l in lineas:
                    t = re.sub(r"\x1b\[[0-9;]*m", "", l.decode("utf-8", "replace")).strip()
                    if t and FILTRO.search(t): print("  " + t[:220])
        for tipo, v in envios:
            if tipo == "pausa": drenar(v); continue
            s.write((v + "\n" if v.startswith(">") else v).encode("utf-8"))
            drenar(0.3)
        drenar(log_s)


if __name__ == "__main__":
    main(sys.argv[1:])
