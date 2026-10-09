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

## canelita conversa con Canela (8 de octubre de 2026)

`esp32/components/muse/canela_chat.c` reemplaza la sesión de Muse cuando
`CONFIG_CANELITA_CHAT` está activo (lo está por omisión con PSRAM). El botón
graba, la placa manda el WAV a `https://canela.docfraga.com/v1/agentes/canela/voz`
con los dos secretos de Cloudflare Access y el token del dispositivo, y habla la
respuesta de Canela por su bocina. La cara pasa por ESCUCHANDO → PENSANDO →
HABLANDO. Diseño completo en la especificación de `canela-web` (privada).

### Configurar la placa

Los secretos viven en archivos **locales** de la laptop, nunca en este repo:

```
~/.canelita/cf-access.env   CANELITA_CF_ACCESS_CLIENT_ID / _SECRET
~/.canelita/device-token    token del dispositivo
~/.canelita/wifi.tsv        una red por línea: SSID<TAB>contraseña
```

```sh
python configurar.py        # con el entorno de ESP-IDF (trae pyserial)
```

Graba todo en la NVS de la placa y sólo imprime confirmaciones con el largo de
cada valor. Las redes se guardan por el mismo camino que la pantalla de ajustes
(hasta 8). La configuración sobrevive a reflasheos; `erase-flash` la borra.

### Consola por USB

```sh
python consola.py ">canelita.show"     # estado: configurada, red, IP, lista
python consola.py ">canelita.scan"     # redes que ve la placa (sólo 2.4 GHz)
python prueba_ptt.py "Canela, ¿qué hora es?"   # aprieta el botón por USB y la Mac habla
python consola.py ">canelita.vol 70"   # volumen de la bocina, 0-100
python consola.py ">canelita.trazas"   # pila de todas las tareas (diagnóstico)
```

⚠️ **Un comando debe empezar con `>`.** Fuera de una línea `>…`, la consola toma
cada carácter como una tecla de banco de pruebas: `d`/`u` aprietan y sueltan el
botón de hablar, `a`/`s` mueven y seleccionan en el menú, `z`/`w` duermen y
despiertan. Mandar un secreto sin `>` lo convierte en pulsaciones sueltas.

### Trampas que costaron tiempo

- **TLS con Cloudflare:** el certificado de `canela.docfraga.com` (Google Trust
  Services) viaja con GTS Root R4 firmada por *GlobalSign Root CA* (R1), que ya no
  está en el paquete de ESP-IDF 6. Se resuelve con
  `MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`, que `CANELITA_CHAT` activa con
  `select`. No se fija un certificado a mano: Cloudflare rota de autoridad.
- **Cambiar Kconfig no basta:** el `sdkconfig` generado en `build-175c/` manda.
  Bórralo y recompila.
- **Pila:** minimp3 pone ~16 KB en la pila. Los turnos corren en una tarea
  permanente con 48 KB de pila en PSRAM (el patrón de Muse); una tarea con pila
  en PSRAM no puede borrarse a sí misma.
- **Acentos:** las fuentes Montserrat de LVGL sólo traen ASCII. Los subtítulos se
  transliteran ("Sí" → "Si"); la voz no cambia.
- **Vigilantes apagados = congelamiento mudo.** El SDK trae apagados los
  vigilantes de interrupciones y de tareas. Así, la placa se congeló al cuarto
  turno de una prueba (pantalla y voz) sin decir nada, mientras el latido y la
  consola seguían vivos. Ahora vienen encendidos en
  `devices/sdkconfig.muse-waveshare-s3-175c`. Si vuelve a pasar, la consola muestra
  `Guru Meditation` con la pila de ambos núcleos y la placa se reinicia sola.
  Decodificar con
  `xtensa-esp32s3-elf-addr2line -pfiaC -e build-175c/muse-gadget.elf <direcciones>`.
- **El congelamiento era la pantalla.** El vigilante de tareas lo atrapó: la
  tarea `lvgl` daba vueltas en `wait_for_flushing()` (lv_refr.c). Una franja de
  pantalla cuyo último trozo nunca avisó "enviado" dejaba a LVGL esperando para
  siempre. Pasó al empezar el turno HTTPS, cuando la memoria interna DMA baja a
  ~24 KB. `boards/muse_lcd_bands.c` ahora espera cada franja 1 s como máximo; si
  no llega el aviso, escribe `lcd_bands: band … stuck` en la consola, libera los
  buffers y deja seguir a LVGL. Se pierde un cuadro, no la pantalla.
  **No bastó:** el 9 de octubre se volvió a congelar sin ese mensaje (el envío
  estaba atorado antes, esperando un buffer libre o dentro del driver). Ahora
  la espera del buffer también tiene límite de 1 s, y como último recurso, si
  LVGL lleva más de 3 s esperando a la pantalla o el envío lleva más de 3 s en
  un paso, la placa escribe `lcd_bands: screen stuck … restarting` y se
  reinicia sola. `python consola.py ">canelita.lcd"` dice en qué paso va el
  envío. La causa de fondo sigue abierta: esos mensajes son la pista.
- **`chat=` por consola no suena:** los turnos escritos no pasan por
  `muse_voice`; sólo prueban la red y la decodificación. Para probar la voz usa
  `prueba_ptt.py`.

### Pendiente

- Bloqueo con PIN (especificación §6.1).
- La placa sigue anunciándose por BLE como `MuseGadget-…` para emparejarse con
  Muse: inofensivo (exige apretar el botón), pero hay que apagarlo.
- Fuente con acentos para los subtítulos.
- Latencia: ~11 s en respuestas cortas, más en las largas (la voz se genera
  completa antes de reproducirse).
