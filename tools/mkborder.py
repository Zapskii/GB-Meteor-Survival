#!/usr/bin/env python3
"""Generate art/border_sgb.png -- the Super Game Boy border for Meteor Survival.

    python3 tools/mkborder.py      ->  art/border_sgb.png
    make border                    ->  border_data.c / border_data.h

THE ONE HARD CONSTRAINT: the SGB border is drawn out of the Game Boy's own
256-tile CHR block, so the art may use at most 256 distinct 8x8 tiles -- and
NOT exactly 128, which is the count CHR_TRN fails on silently. That single
limit is why every asteroid here is a stamp pasted at a grid-aligned position
rather than drawn freehand: the same bitmap at the same cell offsets reuses its
tiles. A rock placed at a non-multiple-of-8 coordinate would generate a fresh
tile set every time and blow the budget. The stamps are opaque (space-filled)
for the same reason -- a transparent stamp would let the starfield show through
and make two copies of one stamp differ.

Everything else here is ordinary: a 5-pattern starfield so the sky costs five
tiles instead of one per cell, and the text on an 8px grid over a cleared strip.

The window rect is fully transparent. That is where the Game Boy screen shows.
"""
import math
import random
from PIL import Image, ImageDraw

OUT = "art/border_sgb.png"
W, H = 256, 224
WIN = (48, 40, 208, 184)          # the GB screen: half-open, must stay clear

# --- palette: 15 opaque + transparent = the 16 an SGB palette holds --------
SPACE   = (8, 8, 24)
SPACE_L = (24, 24, 56)
STAR    = (255, 255, 255)
STAR_D  = (120, 130, 180)
ROCK_L  = (150, 140, 130)
ROCK_D  = (78, 70, 66)
ROCK_X  = (40, 36, 34)
GOLD    = (250, 200, 60)
BYLINE  = (150, 160, 210)
HULL    = (190, 195, 205)
HULL_D  = (105, 112, 128)
ACCENT  = (220, 70, 60)
FLAME   = (250, 190, 70)
FLAME_D = (200, 90, 40)

# 5x7 glyphs. GBDK's font_min ships only as a compiled .lib, so unlike the
# sibling projects there is no FONT dict to parse out of a generator -- the
# handful of letters this border needs are spelled out here instead.
FONT = {
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "I": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
}


def text_w(s, scale):
    return len(s) * 6 * scale - 2 * scale


def draw_glyph(d, x, y, ch, scale, col):
    for r, row in enumerate(FONT[ch]):
        for c, px in enumerate(row):
            if px == "#":
                d.rectangle([x + c * scale, y + r * scale,
                             x + (c + 1) * scale - 1, y + (r + 1) * scale - 1],
                            fill=col)


def text(d, y, s, scale, col, cx=W // 2):
    """Centred text, snapped to the 8px grid over a cleared strip.

    Clearing matters for the tile budget as much as for legibility: glyph
    pixels landing on top of arbitrary stars would combine into one-off tiles.
    """
    w = text_w(s, scale)
    x = (cx - w // 2) & ~7
    y = y & ~7
    d.rectangle([x - 2, y - 1, x + w + 1, y + 7 * scale], fill=SPACE)
    for ch in s:
        if ch != " ":
            draw_glyph(d, x, y, ch, scale, col)
        x += 6 * scale


def starfield(im):
    """Sky on the 8px grid: empty, or one of four star placements. Five tiles."""
    rng = random.Random(7)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, W - 1, H - 1], fill=SPACE)
    offs = [(1, 2), (5, 1), (3, 5), (6, 4)]
    for cy in range(0, H, 8):
        for cx in range(0, W, 8):
            if rng.random() < 0.14:
                ox, oy = offs[rng.randrange(4)]
                d.point((cx + ox, cy + oy), fill=STAR)
                d.point((cx + ox + 1, cy + oy), fill=STAR_D)
                d.point((cx + ox, cy + oy + 1), fill=STAR_D)


def rock_stamp(size, seed):
    """One asteroid as a pasteable bitmap, opaque so it overwrites cleanly.

    Irregular outline from a jittered radius, lit from the upper left, with a
    few craters. Deterministic from `seed`, so a rerun gives the same art.
    """
    rng = random.Random(seed)
    im = Image.new("RGBA", (size, size), SPACE + (255,))
    d = ImageDraw.Draw(im)
    n = 9 + seed % 4
    c = size / 2.0
    r = size / 2.0 - 1
    jit = [0.76 + rng.random() * 0.26 for _ in range(n)]

    def pts(rad, dx=0.0, dy=0.0):
        return [(c + dx + math.cos(i * 2 * math.pi / n) * rad * jit[i],
                 c + dy + math.sin(i * 2 * math.pi / n) * rad * jit[i])
                for i in range(n)]

    d.polygon(pts(r + 1), fill=SPACE)                   # rim against the stars
    d.polygon(pts(r), fill=ROCK_D)                      # shadowed body
    d.polygon(pts(r - 2.0, -1.0, -1.0), fill=ROCK_L)    # lit face, upper left
    for _ in range(1 + seed % 3):
        a = rng.random() * 2 * math.pi
        rr = rng.random() * r * 0.45
        px, py = c + math.cos(a) * rr, c + math.sin(a) * rr
        s = 1.5 + rng.random() * 1.6
        d.ellipse([px - s, py - s, px + s, py + s], fill=ROCK_X)
    return im


def ship_stamp():
    """The player's ship, nose up, three times its on-screen size."""
    im = Image.new("RGBA", (24, 24), SPACE + (255,))
    d = ImageDraw.Draw(im)
    d.polygon([(12, 2), (20, 18), (12, 15), (4, 18)], fill=HULL)
    d.polygon([(12, 2), (20, 18), (12, 15)], fill=HULL_D)
    d.rectangle([9, 15, 14, 17], fill=ACCENT)
    d.polygon([(10, 18), (13, 18), (12, 23)], fill=FLAME)
    return im


def main():
    im = Image.new("RGBA", (W, H))
    starfield(im)

    # Asteroids, pasted at grid-aligned positions only. The side strips frame
    # the screen; the bottom strip is the field the ship is flying into.
    r32, r24, r16 = rock_stamp(32, 1), rock_stamp(24, 2), rock_stamp(16, 3)
    for stamp, x, y in [
        (r32,   0,  48), (r24,  16, 104), (r32,   0, 152),      # left strip
        (r32, 224,  56), (r24, 216, 112), (r32, 224, 168),      # right strip
        (r24,  64, 192), (r32, 112, 184), (r16, 160, 200),      # bottom strip
        (r24, 184, 184),
    ]:
        im.paste(stamp, (x, y))
    im.paste(ship_stamp(), (24, 196))

    d = ImageDraw.Draw(im)
    text(d, 4, "METEOR SURVIVAL", 2, GOLD)
    text(d, 28, "BY ZAPSKI", 1, BYLINE)

    # The screen area, last, so nothing above can leak into it.
    d.rectangle([WIN[0], WIN[1], WIN[2] - 1, WIN[3] - 1], fill=(0, 0, 0, 0))

    # --- the three things the SGB transfer punishes ------------------------
    ncol = len({p for p in im.getdata() if p[3]})
    assert ncol <= 16, "palette is %d colours, max 16" % ncol

    opaque = sum(1 for p in im.crop(WIN).getdata() if p[3])
    assert opaque == 0, "%d opaque pixels inside the window rect" % opaque

    tiles = {im.crop((x, y, x + 8, y + 8)).tobytes()
             for y in range(0, H, 8) for x in range(0, W, 8)}
    assert len(tiles) <= 256, "%d tiles, max 256" % len(tiles)
    assert len(tiles) != 128, (
        "exactly 128 tiles -- CHR_TRN moves at most 128 per call and this is "
        "the count that fails silently. Add a star placement to land off it.")

    im.save(OUT)
    print("%s  %dx%d  %d colours  %d tiles" % (OUT, W, H, ncol, len(tiles)))
    print("next: make border && make")


if __name__ == "__main__":
    main()
