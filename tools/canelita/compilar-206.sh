#!/bin/bash
T=~/.canelita-tools; export PATH="$T/py312bin:$T/buildbin:$PATH"
. ~/esp/esp-idf-v6/export.sh >/dev/null
cd ~/Projects/canelita-firmware/esp32
idf.py -B build-206 -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-206/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-206" \
  build && echo "COMPILADO OK"
