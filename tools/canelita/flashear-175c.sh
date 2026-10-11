#!/bin/bash
# Guarda: este script es de la 1.75C (canelita de cuello). Con el reloj 2.06 conectado no se flashea.
_P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
_ID=$(export PATH="$HOME/.canelita-tools/py312bin:$PATH"; . ~/esp/esp-idf-v6/export.sh >/dev/null 2>&1; [ -n "$_P" ] && python -m esptool --port "$_P" chip-id 2>&1)
if [ -n "$_P" ] && ! echo "$_ID" | grep -qi "MAC:"; then echo "NO PUDE LEER LA PLACA: no flasheo."; exit 1; fi
if echo "$_ID" | grep -qi "28:84:85:90:f3:7c"; then
  echo "ES EL RELOJ 2.06: usa flashear-206.sh. No flasheo."; exit 1
fi
set -e
T=~/.canelita-tools; export PATH="$T/py312bin:$T/buildbin:$PATH"
. ~/esp/esp-idf-v6/export.sh >/dev/null
cd ~/Projects/canelita-firmware/esp32
P=$(ls /dev/cu.usbmodem* | head -1)
idf.py -B build-175c -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-175c/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c" \
  -p "$P" erase-flash flash
echo "FLASHEADO OK"
sleep 2
P=$(ls /dev/cu.usbmodem* | head -1)
python - "$P" <<'PY'
import serial,sys,time
s=serial.Serial(sys.argv[1],115200,timeout=0.5); t=time.time(); out=open('/tmp/canelita-arranque.log','wb')
while time.time()-t<35: out.write(s.read(4096))
out.close()
PY
echo "ARRANQUE CAPTURADO"
