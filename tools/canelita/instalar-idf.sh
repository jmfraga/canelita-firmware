#!/bin/bash
set -e
T=~/.canelita-tools
echo "[1] uv"; command -v uv >/dev/null || [ -x ~/.local/bin/uv ] || curl -LsSf https://astral.sh/uv/install.sh | sh
UV=$(command -v uv || echo ~/.local/bin/uv)
echo "[2] Python 3.12"; "$UV" python install 3.12
P312=$("$UV" python find 3.12); mkdir -p "$T/py312bin"; ln -sf "$P312" "$T/py312bin/python3"; ln -sf "$P312" "$T/py312bin/python"
"$T/py312bin/python3" --version
echo "[3] cmake + ninja"; "$T/bin/pip" install -q cmake ninja
mkdir -p "$T/buildbin"; ln -sf "$T/bin/cmake" "$T/buildbin/cmake"; ln -sf "$T/bin/ninja" "$T/buildbin/ninja"
export PATH="$T/py312bin:$T/buildbin:$PATH"
cmake --version | head -1; ninja --version
echo "[4] ESP-IDF install esp32s3"; ~/esp/esp-idf-v6/install.sh esp32s3
echo "IDF INSTALADO"
