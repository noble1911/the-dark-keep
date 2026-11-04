#!/usr/bin/env python3
"""Draws filled and hollow stars (U+2605, U+2606) into a font page whose frames for them are blank.

The campaign list shows a level's stars with these. They use the font's own gold ramp, so UI colours tint them.
Running it again gives the same file. Usage: draw_star_glyphs.py assets/fonts/12-26.clx
"""
import struct
import sys

BRIGHT = 197
SHADE = 198
TRANSPARENT = None

FILLED = """
.....#.....
.....#.....
....#o#....
....#o#....
####ooo####
.#ooooooo#.
..#ooooo#..
...#ooo#...
...#ooo#...
..#oo#oo#..
..##...##..
"""

HOLLOW = """
.....#.....
.....#.....
....#.#....
....#.#....
####...####
.#.......#.
..#.....#..
...#...#...
...#.#.#...
..#.#.#.#..
..##...##..
"""


def encode_frame(art, width, height, top):
    """A CLX frame: rows run bottom to top, each as transparent runs and literal pixel runs."""
    rows = art.strip().splitlines()
    pixels = [[TRANSPARENT] * width for _ in range(height)]
    for y, row in enumerate(rows):
        for x, mark in enumerate(row):
            if mark != '.':
                pixels[top + y][x] = SHADE if mark == 'o' else BRIGHT
    data = bytearray(struct.pack('<HHH', 6, width, height))
    for row in reversed(pixels):
        x = 0
        while x < width:
            start = x
            if row[x] is TRANSPARENT:
                while x < width and row[x] is TRANSPARENT:
                    x += 1
                data.append(x - start)
            else:
                while x < width and row[x] is not TRANSPARENT:
                    x += 1
                data.append(256 - (x - start))
                data.extend(row[start:x])
    return bytes(data)


def main(path):
    with open(path, 'rb') as file:
        data = file.read()
    count = struct.unpack_from('<I', data, 0)[0]
    offsets = struct.unpack_from(f'<{count + 1}I', data, 4)
    frames = [data[offsets[i]:offsets[i + 1]] for i in range(count)]
    _, width, height = struct.unpack_from('<HHH', frames[0], 0)
    frames[0x05] = encode_frame(FILLED, width, height, top=2)
    frames[0x06] = encode_frame(HOLLOW, width, height, top=2)

    out = bytearray(struct.pack('<I', count))
    position = 4 + 4 * (count + 1)
    for frame in frames:
        out += struct.pack('<I', position)
        position += len(frame)
    out += struct.pack('<I', position)
    for frame in frames:
        out += frame
    with open(path, 'wb') as file:
        file.write(out)


if __name__ == '__main__':
    main(sys.argv[1])
