#!/usr/bin/env python3
"""Draws the 20x20 browser and operating-system icons shown next to a blog
comment (develop_docs/reference/comments.md) into html/assets/icons/clients/.

Boat Rudder's own pixel drawings, made to be recognizable at a glance - not
the vendors' logos. GIF89a with a transparent background, which every
browser that can show the comment form can display. Every drawing is
centered in the square, and dark ones carry a gray outline so they show on
dark themes too.

    python3 scripts/generate-client-icons.py [--preview out.png]

The icon names are what src/utils/client_icon.c maps browsers and systems to;
adding one here means mapping it there too. The size is also in the
comments/comment-client_epoch*.html templates (width/height).
"""
import math
import os
import sys

from PIL import Image

SIZE = 20
C0 = SIZE / 2                      # the center, in pixel coordinates
OUT = os.path.join(os.path.dirname(__file__), "..", "html", "assets", "icons", "clients")

PALETTE = {
    "K": (0, 0, 0), "W": (255, 255, 255), "D": (85, 85, 85), "L": (170, 170, 170), "E": (215, 215, 215),
    "R": (229, 57, 53), "r": (160, 20, 20), "G": (67, 160, 71), "g": (27, 94, 32), "A": (164, 198, 57),
    "B": (30, 136, 229), "b": (13, 71, 161), "C": (79, 195, 247), "Y": (253, 216, 53), "y": (230, 170, 0),
    "O": (251, 140, 0), "o": (200, 80, 0), "P": (106, 27, 154), "N": (16, 32, 96), "T": (0, 150, 136),
    "S": (232, 224, 200), "s": (190, 180, 150), "p": (171, 71, 188),
}


def blank():
    return [[None] * SIZE for _ in range(SIZE)]


def from_map(rows):
    assert len(rows) == SIZE and all(len(r) == SIZE for r in rows), [(i, len(r), r) for i, r in enumerate(rows) if len(r) != SIZE] or len(rows)
    return [[None if ch == "." else ch for ch in row] for row in rows]


def put(px, x, y, color):
    if 0 <= x < SIZE and 0 <= y < SIZE:
        px[y][x] = color


def disc(px, cx, cy, r, color, r_in=0.0):
    for y in range(SIZE):
        for x in range(SIZE):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            if r_in <= d <= r:
                px[y][x] = color


def rect(px, x0, y0, x1, y1, color):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            put(px, x, y, color)


def line(px, x0, y0, x1, y1, color):
    steps = max(abs(x1 - x0), abs(y1 - y0)) * 2 or 1
    for i in range(steps + 1):
        t = i / steps
        put(px, round(x0 + (x1 - x0) * t), round(y0 + (y1 - y0) * t), color)


def angle(x, y, cx=C0, cy=C0):
    """Degrees, 0 at 12 o'clock, clockwise."""
    return (math.degrees(math.atan2(x + 0.5 - cx, cy - (y + 0.5))) + 360) % 360


def centered(px):
    """The drawing moved to the middle of the square (its bounding box centered)."""
    rows = [y for y in range(SIZE) if any(px[y])]
    cols = [x for x in range(SIZE) if any(px[y][x] for y in range(SIZE))]
    if not rows:
        return px
    dy = (SIZE - (rows[-1] - rows[0] + 1)) // 2 - rows[0]
    dx = (SIZE - (cols[-1] - cols[0] + 1)) // 2 - cols[0]
    out = blank()
    for y in range(SIZE):
        for x in range(SIZE):
            if px[y][x]:
                put(out, x + dx, y + dy, px[y][x])
    return out


# ---- Browsers --------------------------------------------------------------------

def chrome():
    px = blank()
    for y in range(SIZE):
        for x in range(SIZE):
            d = math.hypot(x + 0.5 - C0, y + 0.5 - C0)
            if d > 9.6:
                continue
            a = angle(x, y)
            px[y][x] = ("B" if d <= 3.3 else "W" if d <= 4.6 else
                        "R" if a >= 300 or a < 60 else "Y" if a < 180 else "G")
    return px


def firefox():
    # The fox curled around the planet: a "C" of orange from its head and
    # ear at the top left, round the bottom, to its tail on the right - lit
    # yellow where it touches the globe, darker on its outer edge. The globe
    # peeks out at the top right, purple on top.
    px = blank()
    globe = blank()
    disc(globe, 12.0, 8.5, 6.6, "B")
    for y in range(SIZE):
        for x in range(SIZE):
            d = math.hypot(x + 0.5 - C0, y + 0.5 - C0)
            if globe[y][x]:
                px[y][x] = "P" if y < 5 else "b" if x + y > 25 else "B"
            elif d <= 9.6 and not (x >= 10 and y < 8):
                px[y][x] = "o" if d > 8.6 else "O"
    for x, y in ((3, 0), (4, 0), (2, 1), (3, 1), (4, 1), (5, 1)):   # the ear
        put(px, x, y, "O")
    for y in range(SIZE):                     # the glow along the globe
        for x in range(SIZE):
            if px[y][x] == "O" and any(0 <= x + dx < SIZE and 0 <= y + dy < SIZE and globe[y + dy][x + dx]
                                       for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                px[y][x] = "Y"
    return px


def edge():
    # A wave of blue and green with an "e"-like hole.
    px = blank()
    for y in range(SIZE):
        for x in range(SIZE):
            d = math.hypot(x + 0.5 - C0, y + 0.5 - C0)
            if d <= 9.6:
                px[y][x] = "T" if (x - y) > 3 or (y < 6 and x > 6) else "B"
                if d > 8.6 and y > 12:
                    px[y][x] = "b"
    disc(px, 10.5, 11.0, 5.0, "W", r_in=3.0)
    for x in range(7, 16):
        put(px, x, 10, "W")
        put(px, x, 11, "W")
    for y in range(SIZE):                      # the e's opening, lower right
        for x in range(SIZE):
            if px[y][x] == "W" and y >= 12 and x >= 12:
                px[y][x] = "B"
    return px


def safari():
    px = blank()
    disc(px, C0, C0, 9.6, "L")
    disc(px, C0, C0, 8.6, "C")
    for a in range(0, 360, 30):               # the dial's ticks
        r = math.radians(a)
        put(px, int(C0 + 7.4 * math.sin(r)), int(C0 - 7.4 * math.cos(r)), "W")
    for t in range(-6, 7):                    # the needle: upper right red, lower left white
        x, y = C0 + t * 0.62, C0 - t * 0.62
        for w in (-0.6, 0, 0.6):
            put(px, int(x + w), int(y + w), "R" if t > 0 else "W")
    put(px, 9, 9, "D"); put(px, 10, 10, "D")
    return px


def opera():
    px = blank()
    for y in range(SIZE):
        for x in range(SIZE):
            e = math.hypot((x + 0.5 - C0) / 7.8, (y + 0.5 - C0) / 9.6)
            i = math.hypot((x + 0.5 - C0) / 3.2, (y + 0.5 - C0) / 6.6)
            if e <= 1.0 and i > 1.0:
                px[y][x] = "r" if x >= 14 else "R"
    return px


def ie():
    # A big blue "e" with a flat yellow ring orbiting it: behind it at the
    # top, in front of it at the bottom left.
    px = blank()
    disc(px, 10.0, 10.5, 7.6, "B", r_in=4.0)
    for x in range(4, 17):
        put(px, x, 9, "B")
        put(px, x, 10, "B")
    for y in range(SIZE):                     # the e's opening, lower right
        for x in range(SIZE):
            if px[y][x] == "B" and 11 <= y <= 13 and x >= 13:
                px[y][x] = None
    disc(px, 10.0, 10.5, 7.6, "b", r_in=7.0)  # its rim, darker
    for y in range(SIZE):
        for x in range(SIZE):
            if px[y][x] == "b" and 11 <= y <= 13 and x >= 13:
                px[y][x] = None
    th = math.radians(-24)
    for i in range(720):
        t = 2 * math.pi * i / 720
        ex, ey = 9.8, 2.6
        x = int(10 + ex * math.cos(t) * math.cos(th) - ey * math.sin(t) * math.sin(th))
        y = int(10.5 + ex * math.cos(t) * math.sin(th) + ey * math.sin(t) * math.cos(th))
        front = math.sin(t) > 0.3 and math.cos(t) < 0
        if 0 <= x < SIZE and 0 <= y < SIZE and (px[y][x] is None or front):
            px[y][x] = "Y"
    return px


def netscape():
    px = blank()
    rect(px, 0, 0, 19, 19, "N")
    disc(px, C0, 30.0, 14.6, "b")             # the planet's horizon
    for y in range(2, 18):                    # a big white N
        put(px, 4, y, "W"); put(px, 5, y, "W")
        put(px, 14, y, "W"); put(px, 15, y, "W")
        dx = 4 + round((y - 2) * 10 / 15)
        put(px, dx, y, "W"); put(px, dx + 1, y, "W")
    for x, y in ((18, 1), (17, 2), (16, 3), (17, 1), (18, 2)):   # a comet
        put(px, x, y, "C")
    put(px, 2, 3, "W"); put(px, 11, 1, "W")
    return px


def mosaic():
    px = blank()
    disc(px, C0, C0, 9.6, "B")
    land = blank()
    for cx, cy, r in ((6, 6, 2.6), (7.5, 8.5, 1.8), (13, 9, 2.4), (14, 12.5, 1.8), (6, 14, 2.0), (12, 4, 1.2)):
        disc(land, cx, cy, r, "G")
    for y in range(SIZE):
        for x in range(SIZE):
            if px[y][x] and land[y][x]:
                px[y][x] = "G"
            if px[y][x] and (x == 10 or y == 10):    # meridian and equator
                px[y][x] = "C"
    return px


def text_browser():
    return from_map([
        "....................",
        "DDDDDDDDDDDDDDDDDDDD",
        "DKKKKKKKKKKKKKKKKKKD",
        "DKGKKKKKKKKKKKKKKKKD",
        "DKKGKKKKKKKKKKKKKKKD",
        "DKKKGKKKKKKKKKKKKKKD",
        "DKKGKKKKKKKKKKKKKKKD",
        "DKGKKKGGGGKKKKKKKKKD",
        "DKKKKKKKKKKKKKKKKKKD",
        "DKGGGKGGGGGKGGGKKKKD",
        "DKKKKKKKKKKKKKKKKKKD",
        "DKGGKGGGGKGGGGGGKKKD",
        "DKKKKKKKKKKKKKKKKKKD",
        "DKGGGGKGGGKKKKKKKKKD",
        "DKKKKKKKKKKKKKKKKKKD",
        "DDDDDDDDDDDDDDDDDDDD",
        "........DDDD........",
        "....DDDDDDDDDDDD....",
        "....................",
        "....................",
    ])


def wap():
    return from_map([
        ".....D..............",
        ".....D..............",
        ".....D..............",
        ".....DDDDDDDDDD.....",
        "....DLLLLLLLLLLD....",
        "....DLAAAAAAAALD....",
        "....DLAKKKKKAALD....",
        "....DLAAAAAAAALD....",
        "....DLAKKKKAAALD....",
        "....DLAAAAAAAALD....",
        "....DLLLLLLLLLLD....",
        "....DLLLDDDDLLLD....",
        "....DLLLLLLLLLLD....",
        "....DLKLLKLLKLLD....",
        "....DLLLLLLLLLLD....",
        "....DLKLLKLLKLLD....",
        "....DLLLLLLLLLLD....",
        "....DLKLLKLLKLLD....",
        "....DLLLLLLLLLLD....",
        ".....DDDDDDDDDD.....",
    ])


def arachne():
    # A spider: the DOS graphical browser named after one.
    px = blank()
    legs = (((8, 8), (4, 5), (2, 1)), ((8, 9), (4, 8), (1, 6)), ((8, 11), (4, 12), (1, 15)),
            ((8, 12), (5, 15), (3, 19)))
    for (ax, ay), (bx, by), (cx, cy) in legs:
        for flip in (False, True):
            f = (lambda x: 19 - x) if flip else (lambda x: x)
            line(px, f(ax), ay, f(bx), by, "L")
            line(px, f(bx), by, f(cx), cy, "L")
    disc(px, C0, 13.0, 4.0, "R")
    disc(px, C0, 13.0, 4.0, "r", r_in=3.2)
    disc(px, C0, 7.5, 2.4, "r")
    put(px, 9, 7, "W"); put(px, 10, 7, "W")
    return px


def browser():
    px = blank()
    disc(px, C0, C0, 9.6, "b")
    disc(px, C0, C0, 8.6, "C")
    for y in range(SIZE):
        for x in range(SIZE):
            if px[y][x] == "C":
                d = abs(x + 0.5 - C0)
                if y in (5, 10, 15) or x == 10 or abs(d - 4.6) < 0.6:
                    px[y][x] = "b"
    return px


# ---- Operating systems ---------------------------------------------------------------

def windows():
    px = blank()
    rect(px, 1, 1, 8, 8, "B"); rect(px, 11, 1, 18, 8, "B")
    rect(px, 1, 11, 8, 18, "B"); rect(px, 11, 11, 18, 18, "B")
    return px


def windows_xp():
    # The four panes waving.
    px = blank()
    for x in range(1, 19):
        off = round(1.3 * math.sin((x - 1) / 17 * math.pi))
        left = x <= 8
        if 9 <= x <= 10:
            continue
        for y in range(2, 9):
            put(px, x, y - off, "R" if left else "G")
        for y in range(11, 18):
            put(px, x, y - off, "B" if left else "Y")
    return px


def windows_classic():
    # The flag of Windows 3.x to 2000, flying, with its trail.
    px = blank()
    for x in range(7, 20):
        off = round(sin_wave(x))
        if x == 13:
            continue
        left = x < 13
        for y in range(2, 9):
            put(px, x, y + off, "R" if left else "G")
        for y in range(10, 17):
            put(px, x, y + off, "B" if left else "Y")
    for x, y in ((5, 3), (3, 5), (5, 7), (1, 8), (4, 10), (2, 12), (5, 14), (0, 15), (3, 16)):
        put(px, x, y, "L")
    return px


def sin_wave(x):
    return 1.0 * math.sin((x - 7) / 12 * math.pi)


def mac():
    return from_map([
        "............LL......",
        "...........LLL......",
        "..........LLL.......",
        "..........LL........",
        "....LLLL....LLLLL...",
        "..LLLLLLLLLLLLLLLLL.",
        ".LLLLLLLLLLLLLLLLL..",
        ".LLLLLLLLLLLLLLLL...",
        "LLLLLLLLLLLLLLLL....",
        "LLLLLLLLLLLLLLLL....",
        "LLLLLLLLLLLLLLLL....",
        "LLLLLLLLLLLLLLLLL...",
        "LLLLLLLLLLLLLLLLLL..",
        ".LLLLLLLLLLLLLLLLLL.",
        ".LLLLLLLLLLLLLLLLLL.",
        "..LLLLLLLLLLLLLLLL..",
        "..LLLLLLLLLLLLLLLL..",
        "...LLLLLL..LLLLLL...",
        "....LLLL....LLLL....",
        "....................",
    ])


def mac_classic():
    # A Macintosh 128K, smiling.
    return from_map([
        "...DDDDDDDDDDDDDD...",
        "...DSSSSSSSSSSSSD...",
        "...DSDDDDDDDDDDSD...",
        "...DSDCCCCCCCCDSD...",
        "...DSDCCKCCKCCDSD...",
        "...DSDCCKCCKCCDSD...",
        "...DSDCCCCCCCCDSD...",
        "...DSDCKCCCCKCDSD...",
        "...DSDCCKKKKCCDSD...",
        "...DSDCCCCCCCCDSD...",
        "...DSDDDDDDDDDDSD...",
        "...DSSSSSSSSSSSSD...",
        "...DSSSSSSSDDDSSD...",
        "...DSSSSSSSSSSSSD...",
        "...DSSSSSSSSSSSSD...",
        "...DDDDDDDDDDDDDD...",
        "...DssssssssssssD...",
        "...DDDDDDDDDDDDDD...",
        "....................",
        "....................",
    ])


def linux():
    # Tux: black and white, yellow beak and feet, a gray outline for dark themes.
    return from_map([
        "....................",
        "......DDDDDDD.......",
        ".....DKKKKKKKD......",
        ".....DKWWKWWKD......",
        ".....DKWKKWKKD......",
        ".....DKyyyyyKD......",
        "....DKKKyyyKKKD.....",
        "....DKWWWWWWWKD.....",
        "...DKWWWWWWWWWKD....",
        "...DKWWWWWWWWWKD....",
        "..DKKWWWWWWWWWKKD...",
        "..DKKWWWWWWWWWKKD...",
        "..DKKWWWWWWWWWKKD...",
        "..DKKWWWWWWWWWKKD...",
        "...DKWWWWWWWWWKD....",
        "..yyyKWWWWWWWKyyy...",
        ".yyyyyKKKKKKKyyyyy..",
        ".yyyyyy.....yyyyyy..",
        "....................",
        "....................",
    ])


def android():
    return from_map([
        "....................",
        ".....A........A.....",
        "......A......A......",
        ".....AAAAAAAAAA.....",
        "....AAAAAAAAAAAA....",
        "....AAAWAAAAWAAA....",
        "....AAAAAAAAAAAA....",
        "....................",
        ".AA.AAAAAAAAAAAA.AA.",
        ".AA.AAAAAAAAAAAA.AA.",
        ".AA.AAAAAAAAAAAA.AA.",
        ".AA.AAAAAAAAAAAA.AA.",
        ".AA.AAAAAAAAAAAA.AA.",
        "....AAAAAAAAAAAA....",
        "....AAAAAAAAAAAA....",
        "......AAA..AAA......",
        "......AAA..AAA......",
        "......AAA..AAA......",
        "....................",
        "....................",
    ])


def ios():
    # An iPhone: speaker, two columns of apps, the home button.
    return from_map([
        "......DDDDDDDD......",
        ".....DDDDLLDDDD.....",
        ".....DCCCCCCCCD.....",
        ".....DCWWCCWWCD.....",
        ".....DCWWCCWWCD.....",
        ".....DCCCCCCCCD.....",
        ".....DCWWCCWWCD.....",
        ".....DCWWCCWWCD.....",
        ".....DCCCCCCCCD.....",
        ".....DCWWCCWWCD.....",
        ".....DCWWCCWWCD.....",
        ".....DCCCCCCCCD.....",
        ".....DCWWCCWWCD.....",
        ".....DCWWCCWWCD.....",
        ".....DCCCCCCCCD.....",
        ".....DCCCCCCCCD.....",
        ".....DDDDDDDDDD.....",
        ".....DDDDLLDDDD.....",
        ".....DDDDDDDDDD.....",
        "......DDDDDDDD......",
    ])


def bsd():
    # The daemon's head: horns, eyes, a grin.
    return from_map([
        "..r..............r..",
        "..rr............rr..",
        "...rr..........rr...",
        "....RR.RRRRRR.RR....",
        "....RRRRRRRRRRRR....",
        "...RRRRRRRRRRRRRR...",
        "...RRWWWRRRRWWWRR...",
        "...RRWWKRRRRKWWRR...",
        "...RRRRRRRRRRRRRR...",
        "...RRRRRRRRRRRRRR...",
        "...RRRRRRRRRRRRRR...",
        "....RRKRRRRRRKRR....",
        "....RRRKKKKKKRRR....",
        ".....RRRRRRRRRR.....",
        "......RRRRRRRR......",
        ".......RRRRRR.......",
        "........RRRR........",
        "....................",
        "....................",
        "....................",
    ])


def unix():
    # A workstation's monitor: an X and a prompt.
    return from_map([
        "....................",
        "DDDDDDDDDDDDDDDDDDDD",
        "DLLLLLLLLLLLLLLLLLLD",
        "DLKKKKKKKKKKKKKKKKLD",
        "DLKWKKKWKKKKKKKKKKLD",
        "DLKKWKWKKKKKKKKKKKLD",
        "DLKKKWKKKKKKKKKKKKLD",
        "DLKKWKWKKKKKKKKKKKLD",
        "DLKWKKKWKWWWKKKKKKLD",
        "DLKKKKKKKKKKKKKKKKLD",
        "DLKKKKKKKKKKKKKKKKLD",
        "DLKKKKKKKKKKKKKKKKLD",
        "DLLLLLLLLLLLLLLLLLLD",
        "DDDDDDDDDDDDDDDDDDDD",
        ".......DDDDDD.......",
        ".......DDDDDD.......",
        "....DDDDDDDDDDDD....",
        "....................",
        "....................",
        "....................",
    ])


def dos():
    # A beige PC monitor at the C:>_ prompt.
    return from_map([
        "....................",
        "DDDDDDDDDDDDDDDDDDDD",
        "DSSSSSSSSSSSSSSSSSSD",
        "DSKKKKKKKKKKKKKKKKSD",
        "DSKKKKKKKKKKKKKKKKSD",
        "DSKLLLKKKLKKKKKKKKSD",
        "DSKLKKKLKKLKKKKKKKSD",
        "DSKLKKKKKKKLKKKKKKSD",
        "DSKLKKKLKKLKKKKKKKSD",
        "DSKLLLKKKLKKKLLLKKSD",
        "DSKKKKKKKKKKKKKKKKSD",
        "DSKKKKKKKKKKKKKKKKSD",
        "DSSSSSSSSSSSSSSSSSSD",
        "DSSSSSSSSSSSSSSSGSSD",
        "DDDDDDDDDDDDDDDDDDDD",
        ".......DDDDDD.......",
        "....DDDDDDDDDDDD....",
        "....................",
        "....................",
        "....................",
    ])


def palm():
    # A Palm PDA: the green screen, the Graffiti area, the four buttons.
    return from_map([
        "....................",
        "....DDDDDDDDDDDD....",
        "...DLLLLLLLLLLLLD...",
        "...DLDDDDDDDDDDLD...",
        "...DLDAAAAAAAADLD...",
        "...DLDAKKKKKAADLD...",
        "...DLDAAAAAAAADLD...",
        "...DLDAKKKAAAADLD...",
        "...DLDAAAAAAAADLD...",
        "...DLDAKKKKAAADLD...",
        "...DLDAAAAAAAADLD...",
        "...DLDDDDDDDDDDLD...",
        "...DLEEEEEEEEEELD...",
        "...DLELLLLLLLLELD...",
        "...DLEEEEEEEEEELD...",
        "...DLLLLLLLLLLLLD...",
        "...DLKKLKLLKLKKLD...",
        "...DLLLLLLLLLLLLD...",
        "....DDDDDDDDDDDD....",
        "....................",
    ])


def os2():
    # OS/2: its name in white on IBM blue, over a Warp-colored stripe.
    glyphs = {
        "O": ("WWW", "W.W", "W.W", "W.W", "WWW"), "S": ("WWW", "W..", "WWW", "..W", "WWW"),
        "/": ("..W", "..W", ".W.", "W..", "W.."), "2": ("WWW", "..W", "WWW", "W..", "WWW"),
    }
    px = blank()
    rect(px, 0, 2, 19, 17, "D")
    rect(px, 1, 3, 18, 16, "b")
    for i, ch in enumerate("OS/2"):
        for r, row in enumerate(glyphs[ch]):
            for c, v in enumerate(row):
                if v == "W":
                    put(px, 3 + i * 4 + c, 5 + r, "W")
    for x in range(3, 17):
        color = "R" if x < 7 else "Y" if x < 11 else "G" if x < 14 else "C"
        put(px, x, 12, color)
        put(px, x, 13, color)
    return px


def os_unknown():
    return from_map([
        "....................",
        "DDDDDDDDDDDDDDDDDDDD",
        "DLLLLLLLLLLLLLLLLLLD",
        "DLBBBBBBBBBBBBBBBBLD",
        "DLBBBBBBWWWWBBBBBBLD",
        "DLBBBBBWWBBWWBBBBBLD",
        "DLBBBBBBBBBWWBBBBBLD",
        "DLBBBBBBBBWWBBBBBBLD",
        "DLBBBBBBBWWBBBBBBBLD",
        "DLBBBBBBBWWBBBBBBBLD",
        "DLBBBBBBBBBBBBBBBBLD",
        "DLBBBBBBBWWBBBBBBBLD",
        "DLLLLLLLLLLLLLLLLLLD",
        "DDDDDDDDDDDDDDDDDDDD",
        ".......DDDDDD.......",
        "....DDDDDDDDDDDD....",
        "....................",
        "....................",
        "....................",
        "....................",
    ])


# ---- Marks -------------------------------------------------------------------------

def verified():
    # The team's mark next to a name: a white check on a blue badge.
    px = blank()
    disc(px, C0, C0, 9.6, "b")
    disc(px, C0, C0, 8.6, "B")
    for dx, dy in ((0, 0), (0, 1), (1, 0)):
        line(px, 5 + dx, 10 + dy, 8 + dx, 13 + dy, "W")
        line(px, 8 + dx, 13 + dy, 14 + dx, 6 + dy, "W")
    return px


ICONS = {
    # browsers
    "chrome": chrome, "firefox": firefox, "edge": edge, "safari": safari, "opera": opera,
    "ie": ie, "netscape": netscape, "mosaic": mosaic, "text": text_browser, "wap": wap,
    "arachne": arachne, "browser": browser,
    # operating systems
    "windows": windows, "windows-xp": windows_xp, "windows-classic": windows_classic,
    "mac": mac, "mac-classic": mac_classic, "linux": linux, "android": android, "ios": ios,
    "bsd": bsd, "unix": unix, "dos": dos, "os2": os2, "palm": palm, "os": os_unknown,
    # marks
    "verified": verified,
}


def to_image(px):
    colors = sorted({c for row in px for c in row if c})
    pal = [0, 0, 0]  # index 0: transparent
    for c in colors:
        pal += list(PALETTE[c])
    pal += [0, 0, 0] * (256 - len(pal) // 3)
    im = Image.new("P", (SIZE, SIZE), 0)
    im.putpalette(pal)
    index = {c: i + 1 for i, c in enumerate(colors)}
    for y in range(SIZE):
        for x in range(SIZE):
            if px[y][x]:
                im.putpixel((x, y), index[px[y][x]])
    return im


def main():
    os.makedirs(OUT, exist_ok=True)
    images = {}
    for name, draw in ICONS.items():
        im = to_image(centered(draw()))
        im.save(os.path.join(OUT, name + ".gif"), transparency=0)
        images[name] = im
    if "--preview" in sys.argv:
        out = sys.argv[sys.argv.index("--preview") + 1]
        scale, cols = 5, 13
        cell = SIZE * scale + 12
        rows = (len(images) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * cell + 12, rows * 2 * cell + 12), (255, 255, 255))
        for i, (name, im) in enumerate(images.items()):
            im.info["transparency"] = 0
            big = im.convert("RGBA").resize((SIZE * scale, SIZE * scale), Image.NEAREST)
            col, row = i % cols, i // cols
            for band, bg in ((0, (255, 255, 255)), (1, (0, 0, 0))):
                x = 12 + col * cell
                y = 12 + (row * 2 + band) * cell
                sheet.paste(Image.new("RGB", (SIZE * scale, SIZE * scale), bg), (x, y))
                sheet.paste(big, (x, y), big)
        sheet.save(out)
    print(f"{len(images)} icons in {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
