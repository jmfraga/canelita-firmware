"""Pixel art de Canelita (mascota de bolsillo) → canela_mascota_sprites.h + vista previa PNG.

Se dibuja la mitad izquierda (12 columnas) y se refleja: la perrita mira de frente.
Cada cara cambia solo ojos, cejas y boca sobre la base. Correr:
    python3 sprites.py            # escribe el .h junto a canela_mascota.c y preview.png aquí
"""
from pathlib import Path

W, H = 24, 22
PALETA = {  # RGB888; '.' = transparente (fondo negro)
    "o": 0x3B2314,  # contorno
    "c": 0xC8743A,  # canela
    "d": 0x8A4A22,  # orejas
    "l": 0xF2C48D,  # hocico y panza
    "e": 0x141414,  # ojos
    "w": 0xFFFFFF,  # brillo
    "n": 0x1A1A1A,  # nariz
    "p": 0xFF6F91,  # lengua / mejillas
    "r": 0xD9383A,  # collar
    "y": 0xF2C230,  # placa / dorado
    "b": 0x3A7BD9,  # pañoleta
    "g": 0x4CAF6A,  # gorro
    "h": 0xF4F4F4,  # banda del gorro
    "k": 0x101010,  # armazón de lentes
}

BASE_IZQ = [
    "......oooooo",   # 0
    "....oocccccc",   # 1
    "..oodccccccc",   # 2
    ".oddddcccccc",   # 3
    "odddddcccccc",   # 4
    "odddddcccccc",   # 5
    "odddddcccccc",   # 6  ojos en 7-8
    "odddddcccccc",   # 7
    "oddddocccccc",   # 8
    ".oddo.ccllll",   # 9
    "..oo..clllln",   # 10 nariz en 11 (y su reflejo)
    "......clllll",   # 11 boca
    "......ocllll",   # 12
    ".......occcc",   # 13
    "........oooo",   # 14
    ".......occcc",   # 15 cuerpo
    "......occlll",   # 16
    "......occlll",   # 17
    "......occlll",   # 18
    "......occcll",   # 19
    "......occocc",   # 20 patas
    "......oooooo",   # 21
]

# (fila, col_izq, texto) que se escribe sobre la mitad izquierda y se refleja.
OJOS = {
    "abiertos": [(6, 7, "we"), (7, 7, "ee")],
    "felices":  [(6, 7, "ee"), (7, 6, "e..")],
    "cerrados": [(7, 7, "ee")],
    "tristes":  [(5, 8, "o"), (6, 7, "we"), (7, 7, "ee")],
}
BOCAS = {
    "sonrisa": [(11, 9, "o.."), (12, 10, "oo")],
    "lengua":  [(11, 9, "o.."), (12, 10, "op"), (13, 11, "p")],
    "chica":   [(12, 11, "o")],
    "triste":  [(11, 10, "oo"), (12, 9, "o")],
    "abierta": [(11, 10, "oo"), (12, 10, "op")],
}
CARAS = {  # cara del servidor → (ojos, boca, mejillas)
    "feliz":      ("felices", "sonrisa", True),
    "emocionada": ("felices", "lengua", True),
    "tranquila":  ("abiertos", "sonrisa", False),
    "hambrienta": ("tristes", "abierta", False),
    "aburrida":   ("tristes", "chica", False),
    "extrana":    ("tristes", "triste", False),
    "dormida":    ("cerrados", "chica", False),
}
COLA = {  # cola a la derecha del cuerpo (sin reflejar), dos cuadros: arriba y abajo
    0: [(13, 20, "oo"), (14, 19, "occo"), (15, 18, "occo"), (16, 18, "oo")],
    1: [(15, 18, "oo"), (16, 18, "occo"), (17, 19, "occo"), (18, 20, "oo")],
}


# Accesorios: capas sobre la perrita, en coordenadas de la rejilla completa (24x22): (fila, col, texto).
ACCESORIOS = {
    "collar":   [(15, 7, "rrrrrrrrrr"), (16, 11, "yy")],
    "panoleta": [(15, 7, "bbbbbbbbbb"), (16, 8, "bbbbbbbb"), (17, 10, "bbbb"), (18, 11, "bb")],
    "gorro":    [(0, 9, "gggggg"), (1, 7, "gggggggggg"), (2, 5, "gggggggggggggg"), (3, 4, "hhhhhhhhhhhhhhhh")],
    "lentes":   [(5, 6, "kkkk"), (5, 14, "kkkk"), (6, 6, "k"), (6, 9, "kkkkkk"), (6, 17, "k"),
                 (7, 6, "k"), (7, 9, "k"), (7, 14, "k"), (7, 17, "k"), (8, 6, "kkkk"), (8, 14, "kkkk")],
}


def capa(acc: str) -> list[tuple[int, int, str]]:
    return [(f, c + i, ch) for f, c, t in ACCESORIOS[acc] for i, ch in enumerate(t) if ch != "."]


def dibujar(cara: str, cuadro: int) -> list[str]:
    ojos, boca, mejillas = CARAS[cara]
    izq = [list(r) for r in BASE_IZQ]
    for f, c, t in OJOS[ojos] + BOCAS[boca]:
        for i, ch in enumerate(t):
            if ch != ".":
                izq[f][c + i] = ch
            elif t != "e.." or i:   # '.' en una sobreescritura borra a canela
                izq[f][c + i] = "c" if f < 9 else izq[f][c + i]
    if mejillas:
        izq[9][6] = "p"
    filas = []
    for r in izq:
        der = "".join(reversed(r))
        filas.append(list("".join(r) + der))
    for f, c, t in COLA[cuadro]:
        for i, ch in enumerate(t):
            if 0 <= c + i < W and filas[f][c + i] == ".":
                filas[f][c + i] = ch
    return ["".join(r) for r in filas]


def main() -> None:
    aqui = Path(__file__).parent
    destino = aqui.parents[2] / "esp32" / "components" / "muse" / "canela_mascota_sprites.h"
    claves = list(PALETA)
    lineas = [
        "/* Generado por tools/canelita/mascota/sprites.py: no editar a mano. */",
        "#pragma once",
        "#include <stdint.h>",
        f"#define MASCOTA_W {W}",
        f"#define MASCOTA_H {H}",
        f"#define MASCOTA_N_CARAS {len(CARAS)}",
        "static const uint32_t MASCOTA_PALETA[] = { 0x000000, "
        + ", ".join(f"0x{PALETA[k]:06X}" for k in claves) + " };",
        "static const char *const MASCOTA_CARAS[] = { " + ", ".join(f'"{c}"' for c in CARAS) + " };",
        "/* [cara][cuadro de la cola][fila*W+col]: 0 = transparente, n = MASCOTA_PALETA[n]. */",
        f"static const uint8_t MASCOTA_PX[{len(CARAS)}][2][{W * H}] = {{",
    ]
    for cara in CARAS:
        lineas.append("    {")
        for cuadro in (0, 1):
            px = [0 if ch == "." else claves.index(ch) + 1 for fila in dibujar(cara, cuadro) for ch in fila]
            lineas.append("        { " + ",".join(map(str, px)) + " },")
        lineas.append("    },")
    lineas.append("};")
    lineas.append(f"#define MASCOTA_N_ACC {len(ACCESORIOS)}")
    lineas.append("static const char *const MASCOTA_ACC[] = { " + ", ".join(f'"{a}"' for a in ACCESORIOS) + " };")
    lineas.append("/* Capa de cada accesorio: {fila, columna, color} y termina en {255,255,0}. */")
    lineas.append("static const uint8_t MASCOTA_ACC_PX[][3] = {")
    inicio = []
    for a in ACCESORIOS:
        inicio.append(sum(len(capa(x)) + 1 for x in list(ACCESORIOS)[:list(ACCESORIOS).index(a)]))
        for f, c, ch in capa(a):
            lineas.append(f"    {{{f},{c},{claves.index(ch) + 1}}},")
        lineas.append("    {255,255,0},")
    lineas.append("};")
    lineas.append("static const uint16_t MASCOTA_ACC_INICIO[] = { " + ", ".join(map(str, inicio)) + " };")
    destino.write_text("\n".join(lineas) + "\n")

    try:
        from PIL import Image
    except ImportError:
        return
    esc = 8
    img = Image.new("RGB", (len(CARAS) * (W + 2) * esc, 2 * (H + 2) * esc), (0, 0, 0))
    for i, cara in enumerate(CARAS):
        for cuadro in (0, 1):
            for y, fila in enumerate(dibujar(cara, cuadro)):
                for x, ch in enumerate(fila):
                    if ch == ".":
                        continue
                    v = PALETA[ch]
                    for dy in range(esc):
                        for dx in range(esc):
                            img.putpixel(((i * (W + 2) + x + 1) * esc + dx, (cuadro * (H + 2) + y + 1) * esc + dy),
                                         (v >> 16, (v >> 8) & 255, v & 255))
    # Tercera fila: cada accesorio puesto sobre la cara «feliz».
    img2 = Image.new("RGB", (img.width, img.height + (H + 2) * esc), (0, 0, 0))
    img2.paste(img, (0, 0))
    for i, acc in enumerate(ACCESORIOS):
        filas = [list(r) for r in dibujar("feliz", 0)]
        for f, c, ch in capa(acc):
            filas[f][c] = ch
        for y, fila in enumerate(filas):
            for x, ch in enumerate(fila):
                if ch == ".":
                    continue
                v = PALETA[ch]
                for dy in range(esc):
                    for dx in range(esc):
                        img2.putpixel(((i * (W + 2) + x + 1) * esc + dx, (2 * (H + 2) + y + 1) * esc + dy),
                                      (v >> 16, (v >> 8) & 255, v & 255))
    img2.save(aqui / "preview.png")


if __name__ == "__main__":
    main()
