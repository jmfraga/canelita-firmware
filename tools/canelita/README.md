# canelita — guía de la placa

canelita es Canela en un wearable de cuello, sobre la
**Waveshare ESP32-S3-Touch-AMOLED-1.75C**. Esta carpeta documenta cómo se
respaldó la placa, cómo se prepara una Mac para compilar y cómo se flashea.

Rama de trabajo: `canela`. La rama `main` sigue a
[`facebookincubator/muse-gadget-sdk`](https://github.com/facebookincubator/muse-gadget-sdk)
sin cambios.

## La placa

Datos leídos del chip con `esptool flash_id` el 8 de octubre de 2026:

| | |
|---|---|
| Chip | ESP32-S3 (QFN56), revisión 0.2 |
| PSRAM | 8 MB integrada |
| Flash | 32 MB (fabricante `0xc8`, dispositivo `0x4019`), quad, 3.3 V |
| USB | USB-Serial/JTAG nativo (`303a:1001`), aparece como `/dev/cu.usbmodem*` |
| Pantalla | AMOLED redondo 466×466, táctil CST9217 |

## Respaldo del firmware de fábrica

Antes de flashear nada se leyeron los 32 MB completos.

- **SHA-256:** `2b783370fea81e70b275c9c0509248a138d8ac5460e4d6aee27d617de9952776`
- **Dónde:** dos copias privadas verificadas, en `~/canelita-respaldos/` de la
  laptop y del M4. **El binario no se sube a este repo:** es firmware de
  Waveshare y el repo es público.
- **Verificación:** 32 MB exactos, bootloader válido en `0x0`, tabla de
  particiones legible en `0x8000`, y una relectura de control de la placa
  idéntica al archivo.

Tabla de particiones de fábrica, para referencia:

| Partición | Tipo | Offset | Tamaño |
|---|---|---|---|
| nvsfactory | datos/nvs | `0x0009000` | 200 KB |
| nvs | datos/nvs | `0x003b000` | 840 KB |
| otadata | datos/ota | `0x010d000` | 8 KB |
| phy_init | datos/phy | `0x010f000` | 4 KB |
| factory | app | `0x0110000` | 9216 KB |
| ota_0 | app | `0x0a10000` | 4032 KB |
| ota_1 | app | `0x0e00000` | 4032 KB |
| assets | datos | `0x11f0000` | 9216 KB |
| storage | datos | `0x1af0000` | 5120 KB |

### Cómo se respaldó

`respaldar.sh` lee la flash en **8 trozos de 4 MB** y reintenta cada trozo si
falla. Hace falta porque por este USB la lectura es lenta (unos 7 KB/s, más de
una hora) y una desconexión momentánea tiraba el respaldo entero: la primera
lectura de una sola pieza se cayó al 16 %. Con trozos, sólo se repite el que
falló y los que ya están bien no se vuelven a leer.

### Cómo regresar la placa a como llegó

```sh
python -m esptool --port /dev/cu.usbmodem1101 write_flash 0x0 waveshare-175c-fabrica.bin
```

Si no conecta: mantén **BOOT**, toca **RESET**, suelta **BOOT** y reintenta.

## Preparar una Mac para compilar

ESP-IDF **v6.0.1** exige Python 3.10 o mayor. La laptop no tenía Homebrew y su
Python del sistema es 3.9, así que `instalar-idf.sh` lo resuelve sin tocar el
sistema ni pedir contraseña:

1. `uv` instala un Python 3.12 en la carpeta de usuario.
2. `cmake` y `ninja` se instalan con pip en `~/.canelita-tools`.
3. `install.sh esp32s3` de ESP-IDF, sólo para este chip.

ESP-IDF se clona aparte:

```sh
git clone -b v6.0.1 --depth 1 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6
```

Dos problemas que salieron y ya están resueltos en los scripts:

- El instalador de `uv` intenta editar `~/.bash_profile` y `~/.config/fish`; si
  no tiene permiso, falla aunque `uv` sí quede instalado. Basta con volver a
  correr el script.
- Homebrew se atoró en macOS 27 en el M4 (15 min en `sandbox_operation.rb
  extract`). Por eso `cmake` y `ninja` van por pip.

## Compilar y flashear

```sh
./compilar-175c.sh    # build en esp32/build-175c
./flashear-175c.sh    # borra la flash, graba y captura 35 s del arranque
```

El comando de fondo, por si se corre a mano desde `esp32/`:

```sh
idf.py -B build-175c -DIDF_TARGET=esp32s3 -DSDKCONFIG=build-175c/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-175c" \
  build
```

`flashear-175c.sh` hace `erase-flash` antes de grabar porque la distribución
de particiones de fábrica es distinta a la de este firmware.

No hace falta token del SDK de Muse para compilar: sin él la placa arranca y
muestra la interfaz, pero no se empareja con la nube de Meta, que es justo lo
que vamos a reemplazar.

## Primer arranque (8 de octubre de 2026)

Firmware de 2.1 MB (queda libre 48 % de la partición de 4 MB). Escrituras
verificadas por hash. Del log de arranque:

```
CST9217: Resolution X: 466, Y: 466
muse_ui: UI up: 466x466, 320 px Muse, 40 ms frames
```

La interfaz corre con la cara de Canela a 320 px y 25 cuadros por segundo. El
aviso `gpio_install_isr_service(540): GPIO isr service already installed`
viene del SDK original y no tiene efecto.

## Siguiente paso

Reemplazar la capa de Muse (nube `api.muse.ai`) por un push-to-talk contra
`canela-web`, y traducir los letreros de la interfaz.
