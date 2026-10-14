#!/usr/bin/env python3
"""Generate art/border_sgb.png: the Super Game Boy border for GB-Slingshot.

256x224, the size of the SNES screen. The 160x144 Game Boy screen sits at
(48, 40) and has to stay completely transparent there -- anything drawn inside
it would be painted over the game. Everything outside that rectangle is ours,
and it is what a player sees framing the title, the level select and play.

The scene is a launch rig at dusk: a banded sunset sky with a few stars and
clouds, a wooden frame around the screen, the slingshot standing to its left, a
stack of crates on the right with a flag over them, a rolling hill and a fence
along the bottom, and SLINGSHOT across the top.

The frame matters: it is what makes the screen read as a window onto the game
rather than as a hole cut in a picture. The hill deliberately does not try to
meet the game's own ground line -- the two are drawn by different machines in
different palettes and could never match -- so the frame is put there to own
the join instead.

png2asset turns this into src/border_data.c with -pack_mode sgb. Twenty-three
colours, which fits the SGB's four 16-colour palettes with room to spare.

Written with zlib and struct rather than Pillow, to match mkgfx.py and
mklevels.py, which need nothing but the standard library.

    tools/mkborder.py [art/border_sgb.png]
"""
import math
import os
import struct
import sys
import zlib

W, H = 256, 224
GB_X, GB_Y, GB_W, GB_H = 48, 40, 160, 144

CLEAR = (0, 0, 0, 0)

# The sunset, band by band from the top of the sky down to the horizon.
SKY0 = (31, 26, 64, 255)
SKY1 = (52, 32, 96, 255)
SKY2 = (85, 40, 111, 255)
SKY3 = (124, 51, 119, 255)
SKY4 = (165, 65, 111, 255)
SKY5 = (203, 90, 100, 255)
SKY6 = (228, 122, 88, 255)
SKY7 = (240, 154, 88, 255)
SKY8 = (245, 184, 101, 255)
SKY9 = (247, 207, 128, 255)
SKY = [(0, 10, SKY0), (10, 10, SKY1), (20, 10, SKY2), (30, 12, SKY3),
       (42, 14, SKY4), (56, 16, SKY5), (72, 20, SKY6), (92, 28, SKY7),
       (120, 30, SKY8), (150, 40, SKY9)]

CREAM = (255, 241, 201, 255)   # stars, and the wordmark
CLOUD = (247, 196, 162, 255)
CLOUD_HI = (251, 217, 176, 255)

# Four small clouds, each a wide bar with a narrower one resting on it.
CLOUDS = [(6, 66, 20, 3, CLOUD), (11, 63, 10, 3, CLOUD),
          (26, 96, 16, 2, CLOUD_HI), (30, 94, 8, 2, CLOUD_HI),
          (214, 54, 24, 3, CLOUD), (220, 51, 12, 3, CLOUD),
          (222, 104, 18, 2, CLOUD_HI), (226, 102, 9, 2, CLOUD_HI)]

WOOD_DK = (85, 50, 26, 255)    # the frame, the fence, every crate's outline
WOOD_MID = (122, 74, 36, 255)  # crate braces and the slingshot's post
WOOD_LIT = (168, 105, 58, 255)  # crate faces, and the post's lit edge
BAND = (122, 42, 42, 255)      # the sling's rubber
BALL = (141, 141, 160, 255)
BALL_HI = (196, 196, 210, 255)
HILL = (61, 90, 56, 255)
GRASS = (37, 51, 31, 255)
FLAG = (192, 57, 43, 255)
SHADOW = (58, 26, 46, 255)

FRAME = 2                     # the wooden frame's thickness
Crate = 16                    # a crate is 16x16 with a 1px outline

# The wordmark's font: a bold 3x5, one pixel of gap, four pixels to the letter.
# It is not mkgfx.py's FONT, which is drawn for the HUD at 1x and the title at
# 2x; at 3x its thin diagonals read weakly, and this one is what the border was
# designed around. Only the letters the wordmark needs are here.
WORDMARK = "SLINGSHOT"
GLYPH = {
    "S": ["###", "#..", "###", "..#", "###"],
    "L": ["#..", "#..", "#..", "#..", "###"],
    "I": ["###", ".#.", ".#.", ".#.", "###"],
    "N": ["###", "#.#", "#.#", "#.#", "#.#"],
    "G": ["###", "#..", "#.#", "#.#", "###"],
    "H": ["#.#", "#.#", "###", "#.#", "#.#"],
    "O": ["###", "#.#", "#.#", "#.#", "###"],
    "T": ["###", ".#.", ".#.", ".#.", ".#."],
}
WORD_SCALE = 3
WORD_PITCH = 4 * WORD_SCALE   # 3px of letter plus a 1px gap
WORD_X, WORD_Y = 76, 12       # 105px wide, so this centres it on 256
SHADOW_OFF = 2                # the wordmark's drop shadow, down and to the right


def in_gb(x, y):
    return GB_X <= x < GB_X + GB_W and GB_Y <= y < GB_Y + GB_H


class Canvas:
    """A 256x224 RGBA canvas that refuses to draw over the Game Boy screen."""

    def __init__(self):
        # The sky is the base coat, but the game area has to be punched out of
        # it: put() refuses to draw there, so leaving it as sky would leave an
        # opaque block exactly where the Game Boy screen goes, and the game
        # would never be seen.
        self.px = [[CLEAR if in_gb(x, y) else SKY0 for x in range(W)] for y in range(H)]

    def put(self, x, y, c):
        if 0 <= x < W and 0 <= y < H and not in_gb(x, y):
            self.px[y][x] = c

    def rect(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.put(xx, yy, c)

    def line(self, x0, y0, x1, y1, c):
        n = max(abs(x1 - x0), abs(y1 - y0)) * 2 or 1
        for i in range(n + 1):
            t = i / n
            self.put(int(round(x0 + (x1 - x0) * t)), int(round(y0 + (y1 - y0) * t)), c)

    def disc(self, cx, cy, r, c):
        for dy in range(-int(r) - 1, int(r) + 2):
            for dx in range(-int(r) - 1, int(r) + 2):
                if math.hypot(dx, dy) <= r:
                    self.put(cx + dx, cy + dy, c)

    def poly(self, pts, c):
        """Even-odd scanline fill, sampling at pixel centres."""
        ys = [p[1] for p in pts]
        for y in range(int(math.floor(min(ys))), int(math.ceil(max(ys))) + 1):
            yc = y + 0.5
            xs = []
            for i in range(len(pts)):
                (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % len(pts)]
                if (y0 <= yc < y1) or (y1 <= yc < y0):
                    xs.append(x0 + (yc - y0) * (x1 - x0) / (y1 - y0))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(math.ceil(xs[i] - 0.5)), int(math.floor(xs[i + 1] - 0.5)) + 1):
                    self.put(x, y, c)

    def quads(self, start, segs, ink, width=0, fill_to=None):
        """Stroke or fill a chain of quadratics.

        segs is [(cx, cy, x1, y1), ...], and a control point of None means
        "reflect the previous one", which is what SVG's T command does.
        """
        samples = []
        p0, ctrl = start, None
        for cx, cy, x1, y1 in segs:
            ctrl = (cx, cy) if cx is not None else \
                (2 * p0[0] - ctrl[0], 2 * p0[1] - ctrl[1])
            # Sampled evenly in t, not in x: a quadratic covers ground faster
            # near the end of a segment than at the start, so a step count
            # taken from the chord length alone lets x advance by more than a
            # pixel late in the curve and skip a column -- which shows up as
            # pinholes of sky through the hill. Oversample and be done.
            steps = max(2, 16 * int(abs(x1 - p0[0]) + abs(y1 - p0[1])))
            for s in range(steps + 1):
                t = s / steps
                samples.append(((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * ctrl[0] + t * t * x1,
                                (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * ctrl[1] + t * t * y1))
            p0 = (x1, y1)

        if width:                                   # stroke: stamp a square along it
            for x, y in samples:
                self.rect(int(round(x)) - width // 2, int(round(y)) - width // 2,
                          width, width, ink)
        else:                                       # fill: the curve is its top edge
            top = {}
            for x, y in samples:
                xi = int(round(x))
                top[xi] = min(top.get(xi, H), y)
            for x, y in top.items():
                self.rect(x, int(math.ceil(y)), 1, fill_to - int(math.ceil(y)), ink)


def wordmark(cv, ox, oy, ink):
    for i, ch in enumerate(WORDMARK):
        rows = GLYPH[ch]
        for gy, row in enumerate(rows):
            for gx, bit in enumerate(row):
                if bit != "#":
                    continue
                cv.rect(ox + i * WORD_PITCH + gx * WORD_SCALE,
                        oy + gy * WORD_SCALE, WORD_SCALE, WORD_SCALE, ink)


def crate(cv, x, y):
    cv.rect(x, y, Crate, Crate, WOOD_DK)
    cv.rect(x + 1, y + 1, Crate - 2, Crate - 2, WOOD_LIT)
    cv.line(x + 2, y + 2, x + Crate - 2, y + Crate - 2, WOOD_MID)
    cv.line(x + Crate - 2, y + 2, x + 2, y + Crate - 2, WOOD_MID)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "art/border_sgb.png"
    cv = Canvas()

    for y, h, c in SKY:
        cv.rect(0, y, W, h, c)

    for x, y in ((12, 4), (30, 9), (8, 22), (40, 26),
                 (200, 3), (222, 7), (242, 16), (190, 24)):
        cv.put(x, y, CREAM)

    for x, y, w, h, c in CLOUDS:
        cv.rect(x, y, w, h, c)

    # The frame, before anything it is meant to sit in front of.
    cv.rect(GB_X - FRAME, GB_Y - FRAME, GB_W + 2 * FRAME, FRAME, WOOD_DK)
    cv.rect(GB_X - FRAME, GB_Y + GB_H, GB_W + 2 * FRAME, FRAME, WOOD_DK)
    cv.rect(GB_X - FRAME, GB_Y - FRAME, FRAME, GB_H + 2 * FRAME, WOOD_DK)
    cv.rect(GB_X + GB_W, GB_Y - FRAME, FRAME, GB_H + 2 * FRAME, WOOD_DK)

    # The hill, then the flat the fence stands on.
    cv.quads((0, 188), [(24, 182, 48, 187), (None, None, 96, 189), (None, None, 160, 186),
                        (None, None, 208, 188), (None, None, 256, 185)], HILL, fill_to=H)
    cv.rect(0, 208, W, H - 208, GRASS)
    cv.rect(0, 202, W, 3, WOOD_MID)
    for x in range(6, W - 6, 16):
        cv.rect(x, 196, 4, 16, WOOD_DK)

    # The slingshot, standing on the hill to the left of the screen. The game
    # draws its own a few pixels inside the frame, so there are two of them
    # side by side; that is the reference's arrangement and it reads as a
    # diorama -- the rig out here, the play area inset between sling and crates.
    cv.rect(22, 150, 6, 40, WOOD_MID)
    cv.rect(22, 150, 2, 40, WOOD_LIT)
    cv.poly([(22, 154), (26, 150), (16, 114), (11, 117)], WOOD_MID)
    cv.poly([(24, 150), (28, 154), (39, 117), (34, 114)], WOOD_MID)
    cv.rect(10, 114, 5, 3, WOOD_LIT)
    cv.rect(34, 114, 5, 3, WOOD_LIT)
    cv.quads((13, 117), [(16, 126, 23, 127), (31, 126, 37, 117)], BAND, width=2)
    cv.disc(24, 126, 4, BALL)
    cv.rect(22, 124, 2, 2, BALL_HI)

    # The crate stack on the right, and the flag over it. Kept clear of the
    # screen: the art has to be transparent across the Game Boy area, so a
    # crate straddling that edge would come back with its outline cut off.
    for x, y in ((208, 172), (224, 172), (240, 172),
                 (216, 156), (232, 156), (224, 140)):
        crate(cv, x, y)
    cv.rect(231, 124, 2, 16, WOOD_DK)
    cv.poly([(233, 124), (244, 128), (233, 132)], FLAG)

    wordmark(cv, WORD_X + SHADOW_OFF, WORD_Y + SHADOW_OFF, SHADOW)
    wordmark(cv, WORD_X, WORD_Y, CREAM)

    write_png(out, cv.px)
    print("%s  %dx%d" % (out, W, H))


def write_png(path, px):
    """A minimal RGBA PNG writer: zlib for the stream, struct for the chunks."""
    dirname = os.path.dirname(path)
    if dirname:
        os.makedirs(dirname, exist_ok=True)
    raw = b"".join(b"\x00" + b"".join(bytes(p) for p in row) for row in px)

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
