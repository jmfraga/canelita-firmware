#!/bin/bash
# Respaldo resistente a desconexiones: 8 trozos de 4 MB, reintento por trozo.
D=~/canelita-respaldos; PY=~/.canelita-tools/bin/python3; CH=$((4*1024*1024))
for i in 0 1 2 3 4 5 6 7; do
  f="$D/trozos/trozo-$i.bin"
  [ -f "$f" ] && [ "$(stat -f %z "$f")" -eq $CH ] && { echo "trozo $i: ya estaba"; continue; }
  for intento in 1 2 3 4 5; do
    until ls /dev/cu.usbmodem* >/dev/null 2>&1; do sleep 3; done      # espera a que vuelva el USB
    P=$(ls /dev/cu.usbmodem* | head -1)
    if "$PY" -m esptool --port "$P" --baud 921600 read_flash $((i*CH)) $CH "$f.tmp" >> "$D/respaldo.log" 2>&1 \
       && [ "$(stat -f %z "$f.tmp")" -eq $CH ]; then mv "$f.tmp" "$f"; echo "trozo $i: OK (intento $intento)"; break; fi
    echo "trozo $i: falló intento $intento"; rm -f "$f.tmp"; sleep 5
  done
  [ -f "$f" ] || { echo "ABORTADO en trozo $i"; exit 1; }
done
cat "$D"/trozos/trozo-{0..7}.bin > "$D/waveshare-175c-fabrica.bin"
shasum -a 256 "$D/waveshare-175c-fabrica.bin" > "$D/waveshare-175c-fabrica.bin.sha256"
echo "RESPALDO COMPLETO"; cat "$D/waveshare-175c-fabrica.bin.sha256"
