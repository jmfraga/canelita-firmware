#!/usr/bin/env python3
# Copyright (c) 2026 Juan Manuel Fraga.
# SPDX-License-Identifier: Apache-2.0
"""Graba en la placa, por USB, la configuración de canelita.

Lee los secretos de archivos LOCALES (nunca del repo, que es público):
  ~/.canelita/cf-access.env   CANELITA_CF_ACCESS_CLIENT_ID / _SECRET
  ~/.canelita/device-token    token del dispositivo (una línea)
  ~/.canelita/wifi.tsv        una red por línea: SSID<TAB>contraseña
y los manda con los comandos de consola canelita.* (canela_chat.c). La placa
guarda todo en su NVS y sólo confirma el largo de cada valor; este script
imprime únicamente esas confirmaciones, nunca un secreto.

Uso (con el entorno de ESP-IDF, que trae pyserial):
  python configurar.py [--puerto /dev/cu.usbmodem1101] [--url https://canela.docfraga.com]
"""
import argparse, glob, os, re, sys, time

import serial

D = os.path.expanduser("~/.canelita")


def leer_env(p):
    out = {}
    for line in open(p, encoding="utf-8"):
        if "=" in line:
            k, v = line.rstrip("\n").split("=", 1)
            # Cloudflare copia los valores con el encabezado delante; se quita.
            out[k.strip()] = re.sub(r"^\s*cf-access-client-(id|secret)\s*:\s*", "", v, flags=re.I).strip()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--puerto", default=None)
    ap.add_argument("--url", default="https://canela.docfraga.com")
    a = ap.parse_args()
    puerto = a.puerto or (sorted(glob.glob("/dev/cu.usbmodem*")) or [None])[0]
    if not puerto:
        sys.exit("no encuentro la placa (/dev/cu.usbmodem*)")

    cf = leer_env(os.path.join(D, "cf-access.env"))
    token = open(os.path.join(D, "device-token"), encoding="utf-8").read().strip()
    redes = [l.rstrip("\n").split("\t", 1) for l in open(os.path.join(D, "wifi.tsv"), encoding="utf-8") if "\t" in l]

    comandos = [
        ("url", f"canelita.set url {a.url}"),
        ("cf_id", f"canelita.set cf_id {cf['CANELITA_CF_ACCESS_CLIENT_ID']}"),
        ("cf_secret", f"canelita.set cf_secret {cf['CANELITA_CF_ACCESS_CLIENT_SECRET']}"),
        ("token", f"canelita.set token {token}"),
    ] + [(f"wifi {s}", f"canelita.wifi {s}\t{p}") for s, p in redes]

    with serial.Serial(puerto, 115200, timeout=0.3) as s:
        time.sleep(0.5)
        s.reset_input_buffer()
        for nombre, cmd in comandos:
            # ">" marca una línea de comando. SIN él, la consola toma cada
            # carácter como una tecla de banco de pruebas (d/u = botón de hablar,
            # a/s = menú, z/w = dormir/despertar): mandaría los secretos como
            # pulsaciones sueltas.
            s.write((">" + cmd + "\n").encode("utf-8"))
            fin, resp = time.time() + 6, None
            while time.time() < fin and resp is None:
                for line in s.read(4096).decode("utf-8", "replace").splitlines():
                    if line.startswith("@canelita"):
                        resp = line
                        break
            print(f"  {nombre:<22} → {resp or 'SIN RESPUESTA'}")
        s.write(b">canelita.show\n")
        fin = time.time() + 6
        while time.time() < fin:
            for line in s.read(4096).decode("utf-8", "replace").splitlines():
                if line.startswith("@canelita {"):
                    print("  estado:", line[len("@canelita "):])
                    return
    print("  (la placa no reportó su estado)")


if __name__ == "__main__":
    main()
