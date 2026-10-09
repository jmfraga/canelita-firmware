#!/bin/bash
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
