#!/usr/bin/env python3
"""Generate src/gfx.h and src/gfx.c: procedural 8x8 tiles for GB-Slingshot.

Pixel values: 0 = white, 1 = light grey, 2 = dark grey, 3 = black.
Tile indices 0..5 line up with the material enum in sim.h (empty, wood,
stone, ice, target, ground) so the renderer can use a material as a tile.
"""
import sys

TILES = [
    ("BLANK", [
        "00000000", "00000000", "00000000", "00000000",
        "00000000", "00000000", "00000000", "00000000"]),
    ("WOOD", [
        "33333333", "31111113", "32222223", "31111113",
        "31111113", "32222223", "31111113", "33333333"]),
    ("STONE", [
        "33333333", "32212223", "32222123", "32122223",
        "32222213", "31222223", "32221223", "33333333"]),
    ("ICE", [
        "22222222", "20001002", "20010002", "20100002",
        "20000002", "20000102", "20001002", "22222222"]),
    ("TARGET", [
        "00333300", "03222230", "32322323", "32222223",
        "32333323", "32222223", "03222230", "00333300"]),
    ("GROUND", [
        "12121212", "21212121", "12121212", "21212121",
        "12121212", "21212121", "12121212", "21212121"]),
    ("GROUND_TOP", [
        "33333333", "22322322", "12121212", "21212121",
        "12121212", "21212121", "12121212", "21212121"]),
    ("BALL", [
        "00333300", "03222230", "32233223", "32233223",
        "32222223", "32222223", "03222230", "00333300"]),
    ("DOT", [
        "00000000", "00000000", "00000000", "00033000",
        "00033000", "00000000", "00000000", "00000000"]),
]

# The slingshot is centred on the launch point at x = 24, which is a tile
# boundary, so it is drawn as a left tile and a mirrored right tile.
_FORK_L = [
    "03300000", "03300000", "00330000", "00330000",
    "00033000", "00003300", "00000330", "00000033"]
_POST_L = ["00000033"] * 8
TILES += [
    ("FORK_L", _FORK_L),
    ("FORK_R", [r[::-1] for r in _FORK_L]),
    ("POST_L", _POST_L),
    ("POST_R", [r[::-1] for r in _POST_L]),
]


def glyph(rows, ink="3"):
    """A 3x5 glyph into an 8x8 tile: ink at columns 1-3, rows 1-5."""
    assert len(rows) == 5 and all(len(r) == 3 for r in rows), rows
    out = []
    for y in range(8):
        if 1 <= y <= 5:
            out.append("0" + "".join(ink if c == "#" else "0" for c in rows[y - 1]) + "0000")
        else:
            out.append("00000000")
    return out


# 3x5 font. Kept as the front end's only text source so the HUD and the
# splash screens spell what they need instead of drawing icons for it.
FONT = {
    "A": ".#. #.# ### #.# #.#", "B": "##. #.# ##. #.# ##.",
    "C": ".## #.. #.. #.. .##", "D": "##. #.# #.# #.# ##.",
    "E": "### #.. ##. #.. ###", "F": "### #.. ##. #.. #..",
    "G": ".## #.. #.# #.# .##", "H": "#.# #.# ### #.# #.#",
    "I": "### .#. .#. .#. ###", "J": "..# ..# ..# #.# .##",
    "K": "#.# #.# ##. #.# #.#", "L": "#.. #.. #.. #.. ###",
    "M": "#.# ### ### #.# #.#", "N": "#.# ### ### ### #.#",
    "O": ".#. #.# #.# #.# .#.", "P": "##. #.# ##. #.. #..",
    "Q": ".#. #.# #.# ##. .##", "R": "##. #.# ##. #.# #.#",
    "S": ".## #.. .#. ..# ##.", "T": "### .#. .#. .#. .#.",
    "U": "#.# #.# #.# #.# .##", "V": "#.# #.# #.# #.# .#.",
    "W": "#.# #.# ### ### #.#", "X": "#.# #.# .#. #.# #.#",
    "Y": "#.# #.# .#. .#. .#.", "Z": "### ..# .#. #.. ###",
    "0": "### #.# #.# #.# ###", "1": ".#. ##. .#. .#. ###",
    "2": "### ..# ### #.. ###", "3": "### ..# .## ..# ###",
    "4": "#.# #.# ### ..# ..#", "5": "### #.. ### ..# ###",
    "6": "### #.. ### #.# ###", "7": "### ..# ..# ..# ..#",
    "8": "### #.# ### #.# ###", "9": "### #.# ### ..# ###",
    "-": "... ... ### ... ...", ":": "... .#. ... .#. ...",
}
# Order matters: gfx.h numbers these, and main.c's char_tile() walks A-Z then
# 0-9 by adding an offset, so the alphabet and the digits must stay contiguous.
_FONT_ORDER = [chr(c) for c in range(ord("A"), ord("Z") + 1)]
_FONT_ORDER += [str(d) for d in range(10)] + ["-", ":"]
TILES += [("FONT_" + (c if c.isalnum() else "DASH" if c == "-" else "COLON"),
           glyph(FONT[c].split())) for c in _FONT_ORDER]

# Shade 1 is light grey, which is nearly invisible on the white background,
# so the win-screen rating stars are drawn in black like the font.
TILES += [("STAR", [
    "00033000", "00033000", "00333300", "33333333",
    "03333330", "00333300", "03300330", "33000033"])]


def cracked(rows, ink):
    """Overlay one crack so a cell that has taken damage looks damaged.
    Without it a shot that only chips a cell is invisible, and two identical
    shots appear to do different things."""
    return ["".join(ink if c == "#" else b for c, b in zip(m, r))
            for m, r in zip(_CRACK, rows)]


# A jagged line down the middle of the tile. The ink is picked per material so
# the crack contrasts with whatever the cell is made of: black on wood and ice
# (light interiors), white on stone and the target (dark ones).
_CRACK = [
    "...#....", "...#....", "..##....", "..#.....",
    "..#.....", ".##.....", "..#.....", "...#...."]

# Appended after the material tiles, so indices 0..5 still line up with the
# material enum. main.c maps a damaged cell to one of these.
TILES += [
    ("WOOD_BROKEN", cracked(TILES[1][1], "3")),
    ("STONE_BROKEN", cracked(TILES[2][1], "0")),
    ("ICE_BROKEN", cracked(TILES[3][1], "3")),
    ("TARGET_BROKEN", cracked(TILES[4][1], "0")),
]

# The puff a shot kicks up where it lands. Appended last so nothing above it
# shifts: material ids double as tile ids and the cracked variants are found by
# index. Shade 2 rather than 1, because shade 1 is near-invisible on the sky.
TILES += [("PUFF", [
    "00000000", "00000000", "00333000", "03322300",
    "00322300", "00333000", "00000000", "00000000"])]


def encode(rows):
    out = []
    for row in rows:
        lo = hi = 0
        for ch in row:
            v = int(ch)
            lo = (lo << 1) | (v & 1)
            hi = (hi << 1) | ((v >> 1) & 1)
        out += [lo, hi]
    return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "src"
    for name, rows in TILES:
        assert len(rows) == 8 and all(len(r) == 8 for r in rows), name

    with open(outdir + "/gfx.h", "w") as f:
        f.write("/* GENERATED by tools/mkgfx.py - do not edit. */\n")
        f.write("#ifndef GFX_H\n#define GFX_H\n\n")
        for i, (name, _) in enumerate(TILES):
            f.write("#define TILE_%s %d\n" % (name, i))
        f.write("#define GFX_TILE_COUNT %d\n\n" % len(TILES))
        f.write("extern const unsigned char gfx_tiles[GFX_TILE_COUNT * 16];\n\n#endif\n")

    with open(outdir + "/gfx.c", "w") as f:
        f.write("/* GENERATED by tools/mkgfx.py - do not edit. */\n")
        f.write('#include "gfx.h"\n\n')
        f.write("const unsigned char gfx_tiles[GFX_TILE_COUNT * 16] = {\n")
        for name, rows in TILES:
            data = encode(rows)
            f.write("    /* %s */ " % name)
            f.write(", ".join("0x%02X" % b for b in data))
            f.write(",\n")
        f.write("};\n")


if __name__ == "__main__":
    main()
