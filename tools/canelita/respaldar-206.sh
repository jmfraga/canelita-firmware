#!/bin/bash
# Respaldo de fábrica del reloj Waveshare ESP32-S3-Touch-AMOLED-2.06 (MAC 28:84:85:90:f3:7c).
# 8 trozos de 4 MB con reintento; carpeta PROPIA (no la de canelita 1.75C).
D=~/canelita-respaldos/reloj-206; CH=$((4*1024*1024)); MAC="28:84:85:90:f3:7c"
T=~/.canelita-tools; export PATH="$T/py312bin:$T/buildbin:$PATH"; . ~/esp/esp-idf-v6/export.sh >/dev/null 2>&1
for i in 0 1 2 3 4 5 6 7; do
  f="$D/trozos/trozo-$i.bin"
  [ -f "$f" ] && [ "$(stat -f %z "$f")" -eq $CH ] && { echo "trozo $i: ya estaba"; continue; }
  for intento in 1 2 3 4 5; do
    until ls /dev/cu.usbmodem* >/dev/null 2>&1; do sleep 3; done
    P=$(ls /dev/cu.usbmodem* | head -1)
    # Sólo esta placa: si la MAC no coincide (p. ej. conectaron canelita), no se lee.
    python -m esptool --port "$P" chip-id 2>/dev/null | grep -qi "$MAC" || { echo "trozo $i: la placa en $P no es el reloj"; sleep 10; continue; }
    if python -m esptool --port "$P" --baud 921600 read-flash $((i*CH)) $CH "$f.tmp" >> "$D/respaldo.log" 2>&1 \
       && [ "$(stat -f %z "$f.tmp")" -eq $CH ]; then mv "$f.tmp" "$f"; echo "trozo $i: OK (intento $intento)"; break; fi
    echo "trozo $i: falló intento $intento"; rm -f "$f.tmp"; sleep 5
  done
  [ -f "$f" ] || { echo "ABORTADO en trozo $i"; exit 1; }
done
cat "$D"/trozos/trozo-{0..7}.bin > "$D/waveshare-206-fabrica.bin"
shasum -a 256 "$D/waveshare-206-fabrica.bin" > "$D/waveshare-206-fabrica.bin.sha256"
echo "RESPALDO COMPLETO"; cat "$D/waveshare-206-fabrica.bin.sha256"
