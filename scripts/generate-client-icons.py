#!/usr/bin/env python3
"""Draws the 16x16 browser and operating-system icons shown next to a blog
comment (develop_docs/reference/comments.md) into html/assets/icons/clients/.

Boat Rudder's own pixel drawings, made to be recognizable at a glance - not
the vendors' logos. GIF89a with a transparent background, which every
browser that can show the comment form can display.

    python3 scripts/generate-client-icons.py [--preview out.png]

The icon names are what src/utils/client_icon.c maps browsers and systems to;
adding one here means mapping it there too.
"""
import math
import os
import sys

from PIL import Image

SIZE = 16
OUT = os.path.join(os.path.dirname(__file__), "..", "html", "assets", "icons", "clients")

PALETTE = {
    "K": (0, 0, 0), "W": (255, 255, 255), "D": (85, 85, 85), "L": (170, 170, 170), "E": (215, 215, 215),
    "R": (229, 57, 53), "r": (160, 20, 20), "G": (67, 160, 71), "g": (27, 94, 32), "A": (164, 198, 57),
    "B": (30, 136, 229), "b": (13, 71, 161), "C": (79, 195, 247), "Y": (253, 216, 53), "y": (230, 170, 0),
    "O": (251, 140, 0), "o": (200, 80, 0), "P": (106, 27, 154), "N": (16, 32, 96), "T": (0, 150, 136),
    "S": (232, 224, 200), "s": (190, 180, 150),
}


def blank():
    return [[None] * SIZE for _ in range(SIZE)]


def from_map(rows):
    assert len(rows) == SIZE and all(len(r) == SIZE for r in rows), rows
    return [[None if ch == "." else ch for ch in row] for row in rows]


def disc(px, cx, cy, r, color, r_in=0.0):
    for y in range(SIZE):
        for x in range(SIZE):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            if r_in <= d <= r:
                px[y][x] = color


def rect(px, x0, y0, x1, y1, color):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px[y][x] = color


def angle(x, y, cx=8, cy=8):
    """Degrees, 0 at 12 o'clock, clockwise."""
    return (math.degrees(math.atan2(x + 0.5 - cx, cy - (y + 0.5))) + 360) % 360


# ---- Browsers --------------------------------------------------------------------

def chrome():
    px = blank()
    for y in range(SIZE):
        for x in range(SIZE):
            d = math.hypot(x + 0.5 - 8, y + 0.5 - 8)
            if d > 7.6:
                continue
            a = angle(x, y)
            px[y][x] = "B" if d <= 2.6 else "W" if d <= 3.7 else ("R" if a >= 300 or a < 60 else "Y" if a < 180 else "G")
    return px


def firefox():
    return from_map([
        "....oo..........",
        "...oOO..PPPP....",
        "..oOO.PPPBBPP...",
        "..oOOPPBBBBBBP..",
        ".oOOOPBBBBBBBBP.",
        ".oOOPBBBBBBBBBP.",
        "oOOOPBBBBBBBBBBP",
        "oOOOOBBBBBBBBOOP",
        "oOOOOOBBBBBBOOOo",
        "oOOOOOOBBBOOOOOo",
        ".oOOOOOOOOOOOOo.",
        ".oOOOOOOOOOOOOo.",
        "..oOOOOOOOOOOo..",
        "...ooOOOOOOoo...",
        ".....oooooo.....",
        "................",
    ])


def edge():
    return from_map([
        "......TTTT......",
        "....TTTTTTTT....",
        "...TTTTTTTTTT...",
        "..TTTBBBBTTTTT..",
        ".TTBBBBBBBBTTTT.",
        ".TBBBBWWWBBBTTT.",
        "TTBBBWWWWWBBTTTT",
        "TBBBWWBBBWWBBTTT",
        "TBBBWWWWWWWBBBTT",
        "TBBBWWBBBBBBBBTT",
        "TBBBWWBBBBBBBBTT",
        ".BBBBWWWWWWBBBT.",
        ".BBBBBBBBBBBBBB.",
        "..BBBBBBBBBBBB..",
        "...bBBBBBBBBb...",
        ".....bbbbbb.....",
    ])


def safari():
    px = blank()
    disc(px, 8, 8, 7.6, "L")
    disc(px, 8, 8, 6.6, "C")
    disc(px, 8, 8, 5.6, "W", r_in=5.0)
    for t in range(-4, 5):         # the needle, upper right red, lower left white
        x, y = 8 + t * 0.7, 8 - t * 0.7
        for w in (-0.5, 0, 0.5):
            xi, yi = int(x + w), int(y + w)
            if 0 <= xi < SIZE and 0 <= yi < SIZE:
                px[yi][xi] = "R" if t > 0 else "W"
    px[7][7] = px[8][8] = "D"
    return px


def opera():
    px = blank()
    for y in range(SIZE):
        for x in range(SIZE):
            dx, dy = (x + 0.5 - 8) / 6.2, (y + 0.5 - 8) / 7.6
            e = math.hypot(dx, dy)
            ix, iy = (x + 0.5 - 8) / 2.6, (y + 0.5 - 8) / 5.4
            if e <= 1.0 and math.hypot(ix, iy) > 1.0:
                px[y][x] = "R" if x < 11 else "r"
    return px


def ie():
    px = from_map([
        "................",
        "............YY..",
        "..........YY..Y.",
        ".....BBBBYB...Y.",
        "...BBBBBBBBB..Y.",
        "..BBB.....BBBY..",
        ".BBY.......BBY..",
        "YBB.BBBBBBBBBB..",
        "BBY.BBBBBBBBBB..",
        "BB..BB..........",
        "BB...BB.....BB..",
        ".BB...BBBBBBB...",
        "Y.BBB..........Y",
        ".Y..BBBBBBBB..Y.",
        "..YY........YY..",
        "....YYYYYYYY....",
    ])
    return px


def netscape():
    px = blank()
    rect(px, 0, 0, 15, 15, "N")
    rect(px, 0, 11, 15, 15, "b")
    for y in range(1, 15):         # a big white N
        px[y][3] = px[y][4] = "W"
        px[y][11] = px[y][12] = "W"
        dx = 3 + int((y - 1) * 8 / 13)
        px[y][dx] = px[y][min(dx + 1, 12)] = "W"
    px[1][14] = px[2][13] = "C"   # a comet
    px[3][12] = "C"
    return px


def mosaic():
    px = blank()
    disc(px, 8, 8, 7.6, "B")
    land = [(4, 3), (5, 3), (6, 3), (3, 4), (4, 4), (5, 4), (6, 4), (7, 4), (4, 5), (5, 5), (6, 5),
            (5, 6), (6, 6), (10, 6), (11, 6), (9, 7), (10, 7), (11, 7), (12, 7), (10, 8), (11, 8),
            (12, 8), (10, 9), (11, 9), (11, 10), (4, 10), (5, 10), (4, 11), (5, 11), (6, 11), (5, 12)]
    for x, y in land:
        px[y][x] = "G"
    for y in range(SIZE):          # meridian and equator
        for x in range(SIZE):
            if px[y][x] and (x == 8 or y == 8):
                px[y][x] = "C"
    return px


def text_browser():
    return from_map([
        "................",
        "DDDDDDDDDDDDDDDD",
        "DKKKKKKKKKKKKKKD",
        "DKKKKKKKKKKKKKKD",
        "DKGKKKKKKKKKKKKD",
        "DKKGKKKKKKKKKKKD",
        "DKKKGKKKKKKKKKKD",
        "DKKGKKKKKKKKKKKD",
        "DKGKKKGGGGKKKKKD",
        "DKKKKKKKKKKKKKKD",
        "DKKKKKKKKKKKKKKD",
        "DKGGGKGGKGGGGKKD",
        "DKKKKKKKKKKKKKKD",
        "DDDDDDDDDDDDDDDD",
        "......DDDD......",
        "....DDDDDDDD....",
    ])


def wap():
    return from_map([
        "....D...........",
        "....D...........",
        "....DDDDDDDD....",
        "...DLLLLLLLLD...",
        "...DLAAAAAALD...",
        "...DLAKKKKALD...",
        "...DLAAAAAALD...",
        "...DLAKKKAALD...",
        "...DLAAAAAALD...",
        "...DLLLLLLLLD...",
        "...DLKLKLKLLD...",
        "...DLLLLLLLLD...",
        "...DLKLKLKLLD...",
        "...DLLLLLLLLD...",
        "...DLKLKLKLLD...",
        "....DDDDDDDD....",
    ])


def arachne():
    # A spider: the DOS graphical browser named after one.
    return from_map([
        "................",
        "..L..........L..",
        "...L........L...",
        "L...L..rr..L...L",
        ".L...LrRRrL...L.",
        "..LLLLrRRrLLLL..",
        "......rrrr......",
        "..LLLrRRRRrLLL..",
        ".L..LrRRRRrL..L.",
        "L..L.rRRRRr.L..L",
        "...L.rRRRRr.L...",
        "..L...rRRr...L..",
        "..L....rr....L..",
        ".L............L.",
        "................",
        "................",
    ])


def browser():
    px = blank()
    disc(px, 8, 8, 7.6, "b")
    disc(px, 8, 8, 6.6, "C")
    for y in range(SIZE):
        for x in range(SIZE):
            if px[y][x] == "C":
                d = abs(x + 0.5 - 8)
                if y in (4, 8, 12) or x == 8 or abs(d - 3.5) < 0.6:
                    px[y][x] = "b"
    return px


# ---- Operating systems ---------------------------------------------------------------

def windows():
    px = blank()
    rect(px, 1, 1, 7, 7, "B"); rect(px, 9, 1, 15, 7, "B")
    rect(px, 1, 9, 7, 15, "B"); rect(px, 9, 9, 15, 15, "B")
    return px


def windows_xp():
    px = blank()
    rect(px, 1, 2, 7, 7, "R"); rect(px, 9, 1, 15, 6, "G")
    rect(px, 1, 9, 7, 14, "B"); rect(px, 9, 8, 15, 13, "Y")
    return px


def windows_classic():
    return from_map([
        "................",
        "......RRRR.GGGG.",
        "....L.RRRR.GGGG.",
        "..L...RRRR.GGGG.",
        "....L.RRRR.GGGG.",
        ".L....RRRR.GGGG.",
        "................",
        "......BBBB.YYYY.",
        "...L..BBBB.YYYY.",
        ".L....BBBB.YYYY.",
        "...L..BBBB.YYYY.",
        "L.....BBBB.YYYY.",
        "..L...BBBB.YYYY.",
        "................",
        "................",
        "................",
    ])


def mac():
    return from_map([
        "..........L.....",
        ".........LL.....",
        "........LL......",
        "...LLL...LLLL...",
        "..LLLLLLLLLLLL..",
        ".LLLLLLLLLLLL...",
        ".LLLLLLLLLLL....",
        "LLLLLLLLLLL.....",
        "LLLLLLLLLLL.....",
        "LLLLLLLLLLLL....",
        "LLLLLLLLLLLLL...",
        ".LLLLLLLLLLLLLL.",
        ".LLLLLLLLLLLLL..",
        "..LLLLLLLLLLLL..",
        "...LLLL..LLLL...",
        "................",
    ])


def mac_classic():
    return from_map([
        "...DDDDDDDDDD...",
        "...DSSSSSSSSD...",
        "...DSCCCCCCSD...",
        "...DSCKCCKCSD...",
        "...DSCKCCKCSD...",
        "...DSCCCCCCSD...",
        "...DSKCCCCKSD...",
        "...DSCKKKKCSD...",
        "...DSCCCCCCSD...",
        "...DSSSSSSSSD...",
        "...DSSSSSDDSD...",
        "...DSSSSSSSSD...",
        "...DDDDDDDDDD...",
        "...DssssssssD...",
        "...DDDDDDDDDD...",
        "................",
    ])


def linux():
    return from_map([
        ".....DDDDDD.....",
        "....DKKKKKKD....",
        "....DKWKKWKD....",
        "....DKKYYKKD....",
        "...DKKYYYYKKD...",
        "...DKWWWWWWKD...",
        "..DKWWWWWWWWKD..",
        "..DKWWWWWWWWKD..",
        ".DKKWWWWWWWWKKD.",
        ".DKKWWWWWWWWKKD.",
        ".DKKWWWWWWWWKKD.",
        "..DKWWWWWWWWKD..",
        "..YYKWWWWWWKYY..",
        ".YYYYKKKKKKYYYY.",
        ".YYYY......YYYY.",
        "................",
    ])


def android():
    return from_map([
        "................",
        "....A......A....",
        ".....A....A.....",
        "....AAAAAAAA....",
        "...AAAAAAAAAA...",
        "...AAWAAAAWAA...",
        "...AAAAAAAAAA...",
        "................",
        ".A.AAAAAAAAAA.A.",
        "AA.AAAAAAAAAA.AA",
        "AA.AAAAAAAAAA.AA",
        "AA.AAAAAAAAAA.AA",
        ".A.AAAAAAAAAA.A.",
        "....AAA..AAA....",
        "....AAA..AAA....",
        "................",
    ])


def ios():
    return from_map([
        "....DDDDDDDD....",
        "...DDDDDDDDDD...",
        "...DDDLLLLDDD...",
        "...DCCCCCCCCD...",
        "...DCWWCWWCCD...",
        "...DCWWCWWCCD...",
        "...DCCCCCCCCD...",
        "...DCWWCWWCCD...",
        "...DCWWCWWCCD...",
        "...DCCCCCCCCD...",
        "...DCWWCWWCCD...",
        "...DCWWCWWCCD...",
        "...DCCCCCCCCD...",
        "...DDDDLLDDDD...",
        "...DDDDDDDDDD...",
        "....DDDDDDDD....",
    ])


def bsd():
    return from_map([
        "..r..........r..",
        "..rr........rr..",
        "...RR.RRRR.RR...",
        "...RRRRRRRRRR...",
        "..RRRRRRRRRRRR..",
        "..RRWWRRRRWWRR..",
        "..RRWKRRRRKWRR..",
        "..RRRRRRRRRRRR..",
        "..RRRRRRRRRRRR..",
        "...RRKRRRRKRR...",
        "...RRRKKKKRRR...",
        "....RRRRRRRR....",
        ".....RRRRRR.....",
        "......RRRR......",
        "................",
        "................",
    ])


def unix():
    return from_map([
        "................",
        "DDDDDDDDDDDDDDDD",
        "DLLLLLLLLLLLLLLD",
        "DLKKKKKKKKKKKKLD",
        "DLKWKKKWKKKKKKLD",
        "DLKKWKWKKKKKKKLD",
        "DLKKKWKKKKKKKKLD",
        "DLKKWKWKKKKKKKLD",
        "DLKWKKKWKWWWKKLD",
        "DLKKKKKKKKKKKKLD",
        "DLLLLLLLLLLLLLLD",
        "DDDDDDDDDDDDDDDD",
        ".....DDDDDD.....",
        "...DDDDDDDDDD...",
        "................",
        "................",
    ])


def dos():
    # A beige PC monitor at the C:> prompt.
    return from_map([
        "................",
        "DDDDDDDDDDDDDDDD",
        "DSSSSSSSSSSSSSSD",
        "DSKKKKKKKKKKKKSD",
        "DSLLLKKKLKKKKKSD",
        "DSLKKKLKKLKKKKSD",
        "DSLKKKKKKKLKKKSD",
        "DSLKKKLKKLKKKKSD",
        "DSLLLKKKLKKKLLSD",
        "DSKKKKKKKKKKKKSD",
        "DSKKKKKKKKKKKKSD",
        "DSKKKKKKKKKKKKSD",
        "DSSSSSSSSSSSSSSD",
        "DDDDDDDDDDDDDDDD",
        ".....DDDDDD.....",
        "...DDDDDDDDDD...",
    ])


def os_unknown():
    return from_map([
        "................",
        "DDDDDDDDDDDDDDDD",
        "DLLLLLLLLLLLLLLD",
        "DLBBBBBBBBBBBBLD",
        "DLBBBBWWWBBBBBLD",
        "DLBBBWBBBWBBBBLD",
        "DLBBBBBBBWBBBBLD",
        "DLBBBBBBWBBBBBLD",
        "DLBBBBBWBBBBBBLD",
        "DLBBBBBBBBBBBBLD",
        "DLBBBBBWBBBBBBLD",
        "DLLLLLLLLLLLLLLD",
        "DDDDDDDDDDDDDDDD",
        ".....DDDDDD.....",
        "...DDDDDDDDDD...",
        "................",
    ])


ICONS = {
    # browsers
    "chrome": chrome, "firefox": firefox, "edge": edge, "safari": safari, "opera": opera,
    "ie": ie, "netscape": netscape, "mosaic": mosaic, "text": text_browser, "wap": wap,
    "arachne": arachne, "browser": browser,
    # operating systems
    "windows": windows, "windows-xp": windows_xp, "windows-classic": windows_classic,
    "mac": mac, "mac-classic": mac_classic, "linux": linux, "android": android, "ios": ios,
    "bsd": bsd, "unix": unix, "dos": dos, "os": os_unknown,
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
        im = to_image(draw())
        im.save(os.path.join(OUT, name + ".gif"), transparency=0)
        images[name] = im
    if "--preview" in sys.argv:
        out = sys.argv[sys.argv.index("--preview") + 1]
        scale, cols = 6, 11
        rows = (len(images) + cols - 1) // cols
        sheet = Image.new("RGB", (cols * (SIZE * scale + 12) + 12, rows * 2 * (SIZE * scale + 12) + 12), (255, 255, 255))
        for i, (name, im) in enumerate(images.items()):
            im.info["transparency"] = 0
            big = im.convert("RGBA").resize((SIZE * scale, SIZE * scale), Image.NEAREST)
            col, row = i % cols, i // cols
            for band, bg in ((0, (255, 255, 255)), (1, (0, 0, 0))):
                x = 12 + col * (SIZE * scale + 12)
                y = 12 + (row * 2 + band) * (SIZE * scale + 12)
                sheet.paste(Image.new("RGB", (SIZE * scale, SIZE * scale), bg), (x, y))
                sheet.paste(big, (x, y), big)
        sheet.save(out)
    print(f"{len(images)} icons in {os.path.normpath(OUT)}")


if __name__ == "__main__":
    main()
