#!/usr/bin/env python3
"""Check the Super Game Boy border actually reaches the SGB.

set_sgb_border() is a round trip with no other vantage point: it copies the
border into Game Boy VRAM, draws it on the LCD in a known order, and the SGB
reads the picture back a byte at a time. Get a size or a tile count wrong and
the border is quietly broken on hardware -- which is exactly what the stock
GBDK example did, transferring nothing at all. So this boots the ROM with SGB
processing switched on and compares the bytes PyBoy's SGB ends up holding with
src/border_data.c itself.

It is the shipped ROM, not a special build: sgb_check() gates the border, and
PyBoy answers that probe yes (its MLT_REQ emulation perturbs P1 enough to
satisfy GBDK's handshake), so the transfer runs for real. If a future PyBoy
stops answering yes the first check below fails and says so, rather than
comparing two empty buffers and passing.

What this cannot check: whether a real SGB detects the border, or how the
finished border looks on a SNES. Needs: pip install "pyboy>=2.8.0" pillow

    python3 tests/sgb_border_test.py build/slingshot.gb
"""
import re
import sys
from importlib.metadata import version as package_version

from PIL import Image
from pyboy import PyBoy

# pb.sgb.border_data arrived in PyBoy 2.8.0 (pyboy has no __version__).
_v = package_version("pyboy")
if tuple(int(p) for p in _v.split(".")[:2]) < (2, 8):
    sys.exit("tests/sgb_border_test.py needs PyBoy >= 2.8.0 for pb.sgb.border_data; "
             "found %s. pip install -U 'pyboy>=2.8.0'" % _v)

REG_LCDC = 0xFF40
ART = "art/border_sgb.png"
GB_X, GB_Y, GB_W, GB_H = 48, 40, 160, 144
BORDER_C = "src/border_data.c"
SGB_FLAG = 0x146                        # 0x03 marks the cart SGB-enhanced
OLD_LICENSEE = 0x14B                   # and must be 0x33 for that to mean anything
SGB_FLAG_ON, SGB_LICENSEE = 0x03, 0x33
TILE_BYTES = 32                        # the border is 4bpp: 32 bytes a tile
TILE_CEILING = 255                     # see the block split in src/sgb_border.c
BOOT_FRAMES = 600                     # the GB boot ROM alone takes ~90 here

failures = []


def check(cond, msg):
    print(("ok    " if cond else "FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def hex_bytes(src, name):
    """The byte array called `name` out of the png2asset output."""
    body = re.search(r"\b%s\[\d+\] = \{(.*?)\};" % name, src, re.S).group(1)
    return bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", body))


def palette_bytes(src):
    """border_data_palettes[] as the SGB sees it: little-endian RGB555."""
    body = re.search(r"border_data_palettes\[\d+\] = \{(.*?)\};", src, re.S).group(1)
    out = bytearray()
    for r, g, b in re.findall(r"RGB8\(\s*(\d+),\s*(\d+),\s*(\d+)\)", body):
        word = (((int(b) >> 3) & 0x1F) << 10 | ((int(g) >> 3) & 0x1F) << 5
                | ((int(r) >> 3) & 0x1F))
        out += bytes((word & 0xFF, word >> 8))
    return bytes(out)


def first_diff(got, want):
    for i, (a, b) in enumerate(zip(got, want)):
        if a != b:
            return i
    return None


def compare(label, got, want):
    """got and want are the same length; report where they first disagree."""
    d = first_diff(got, want)
    if d is None:
        check(True, "%s matches border_data.c, %d bytes" % (label, len(want)))
    else:
        check(False, "%s differs at byte %d: SGB has 0x%02X, border_data.c has 0x%02X"
                     % (label, d, got[d], want[d]))


def boot(rom, frames):
    """Boot with SGB processing on; return the emulator and the border it got.

    pyboy.sgb.border_enabled is no use as evidence: PyBoy sets it from the
    header flag and the sgb_border request alone, so it is already true before
    a single byte has been transferred. What says the border arrived is the
    data, which stays empty until the game has booted and run set_sgb_border().
    """
    pb = PyBoy(rom, window="null", sgb_border=True)
    pb.set_emulation_speed(0)
    for _ in range(frames):
        pb.tick(1)
        got = pb.sgb.border_data
        if got[0] and got[1] and any(got[0]) and any(got[1]):
            break
    tiles, bmap, pals = pb.sgb.border_data
    pb.tick(30)                       # let the boot finish and the title draw
    return pb, (bytes(tiles), bytes(bmap), bytes(pals))


def main():
    rom = sys.argv[1] if len(sys.argv) > 1 else "build/slingshot.gb"

    # Nothing is transferred unless the cart announces itself as SGB-enhanced,
    # and a real SGB checks both bytes before it will show a border at all.
    head = open(rom, "rb").read(0x150)
    check(head[SGB_FLAG] == SGB_FLAG_ON,
          "header 0x146 is 0x%02X, the SGB flag (-Wm-ys)" % head[SGB_FLAG])
    check(head[OLD_LICENSEE] == SGB_LICENSEE,
          "header 0x14B is 0x%02X, the old licensee byte" % head[OLD_LICENSEE])

    # The centre of the art has to be a hole for the Game Boy screen. Nothing
    # downstream can see this: the border's tiles, map and palettes come back
    # identical whether that region is transparent or painted, and on an SGB
    # the game frame is overlaid on the centre regardless. So the source image
    # is the only place to check it -- and it is worth checking, because an
    # opaque centre paints over the game in any tool that composites the border
    # on top, which is how the art gets reviewed.
    alpha = Image.open(ART).convert("RGBA").crop(
        (GB_X, GB_Y, GB_X + GB_W, GB_Y + GB_H)).getchannel("A").getextrema()
    check(alpha == (0, 0),
          "the Game Boy area of the art is transparent (alpha range %s)" % (alpha,))

    src = open(BORDER_C).read()
    want_tiles = hex_bytes(src, "border_data_tiles")
    want_map = hex_bytes(src, "border_data_map")
    want_pal = palette_bytes(src)

    n_tiles = len(want_tiles) // TILE_BYTES
    check(n_tiles <= TILE_CEILING,
          "the border is %d tiles, inside the transfer's %d-tile ceiling"
          % (n_tiles, TILE_CEILING))
    # png2asset packs the art into whole 16-colour palettes, 32 bytes each; the
    # SGB has four, and set_sgb_border() loads them at tile 128, where PCT_TRN
    # expects them. A partial or oversized blob would land the palettes on the
    # wrong words and colour the border wrongly.
    check(0 < len(want_pal) <= 128 and len(want_pal) % 32 == 0,
          "the art's palette blob is %d bytes: whole 16-colour palettes, within "
          "the SGB's four" % len(want_pal))
    # The SGB draws colour 0 of every border palette as the backdrop, so it is
    # where the Game Boy screen shows through and nothing may be drawn into it.
    # This is the one fault the byte-for-byte comparisons below cannot see: with
    # a used colour in slot 0 the border still transfers perfectly and only the
    # picture is wrong -- and only on screen, as a band of the art gone. It is
    # what happened when png2asset packed the palettes itself: it reserves slot 0
    # only for palettes whose tiles carry a transparent pixel, and this border's
    # second palette never does. mkborder.py reserves the slot and the rule's
    # -keep_palette_order makes png2asset keep its packing, so a reserved slot 0
    # in every palette is the shape of that arrangement still holding.
    for i in range(len(want_pal) // 32):
        check(want_pal[i * 32:i * 32 + 2] == b"\x00\x00",
              "palette %d keeps slot 0 reserved for the backdrop" % i)

    pb, (tiles, bmap, pals) = boot(rom, BOOT_FRAMES)
    landed = bool(any(tiles) and any(bmap))
    check(landed,
          "the ROM booted and handed the SGB a border -- if this fails, PyBoy "
          "has stopped answering sgb_check() yes (run make sgb-test)")

    if landed:
        # CHR_TRN comes in two blocks, 256 tiles then the rest, each read back
        # off the screen; PCT_TRN follows with the map and then the palettes.
        # Only the region that was actually loaded is meaningful -- the tail of
        # each buffer keeps whatever VRAM the GB left behind, which is why the
        # comparisons are against the source arrays' own lengths.
        compare("the border tiles", tiles[:len(want_tiles)], want_tiles)
        compare("the border map", bmap[:len(want_map)], want_map)
        compare("the border palettes", pals[:len(want_pal)], want_pal)

        # And the game still has to take input with the SGB in play. This is the
        # one configuration the replay suite never covers -- make emu-test runs
        # with SGB processing switched off -- so it is where a border install
        # that wrecks the tile bank, or leaves the SGB multiplexing pads, shows
        # up as a game that boots and then ignores the pad. The window layer
        # only comes up once the title is left behind.
        pb.button_press("start")
        pb.tick(4)
        pb.button_release("start")
        pb.tick(40)
        check(pb.memory[REG_LCDC] >> 5 & 1,
              "START still gets past the title with a border installed")

    pb.stop()


if __name__ == "__main__":
    main()
    print("\n%d failed" % len(failures))
    sys.exit(1 if failures else 0)
