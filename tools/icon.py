"""Build Terminal's two launcher icons from their native pixel grids."""
import struct
import sys
from pathlib import Path


GRID_ICON = (
    "......................",
    "..##################..",
    ".#hhhhhhhhhhhhhhhhhh#.",
    ".#hccccccccccccccccd#.",
    ".#hc##############cd#.",
    ".#hc#bbbbbbbbbbbb#cd#.",
    ".#hc#bggbbbbbbbbb#cd#.",
    ".#hc#bbbggbbbbbbb#cd#.",
    ".#hc#bbbbbggbbbbb#cd#.",
    ".#hc#bbbggbbbbbbb#cd#.",
    ".#hc#bggbbgggggbb#cd#.",
    ".#hc#bbbbbbbbbbbb#cd#.",
    ".#hc##############cd#.",
    ".#hccccccccccccgllcd#.",
    ".#dddddddddddddddddd#.",
    "..##################..",
    ".......#dcccc#........",
    ".......#dcccc#........",
    "......##dddddd##......",
    "....###hhhhhhhh###....",
    "....##############....",
    "......................",
)

LIST_ICON = (
    ".#############.",
    "#hhhhhhhhhhhhh#",
    "#cbbbbbbbbbbbd#",
    "#cbgbbbbbbbbbd#",
    "#cbbgbbbbbbbbd#",
    "#cbgbggggbbbbd#",
    "#ddddddddddddd#",
    ".#############.",
    ".....#####.....",
)

COLOR = {
    ".": (255, 0, 255),  # keyed out by PilRC
    "#": (21, 29, 39),
    "h": (239, 240, 222),
    "c": (188, 194, 181),
    "d": (96, 111, 118),
    "b": (27, 46, 57),
    "g": (112, 255, 155),
    "l": (140, 152, 145),
}
MONO = {pixel: (0, 0, 0) if pixel in "#bd" else (255, 255, 255) for pixel in COLOR}


def write_bitmap(path, grid, palette, scale):
    width, height = len(grid[0]) * scale, len(grid) * scale
    padding = bytes((-width * 3) % 4)
    body = bytearray()
    for line in reversed(grid):
        row = b"".join(bytes(palette[pixel][::-1]) * scale for pixel in line)
        body.extend((row + padding) * scale)

    header = struct.pack("<2sIHHI", b"BM", 54 + len(body), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(body), 0, 0, 0, 0)
    path.write_bytes(header + info + body)


if __name__ == "__main__":
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    for name, grid in (("launcher", GRID_ICON), ("list", LIST_ICON)):
        for mode, palette in (("color", COLOR), ("mono", MONO)):
            for scale in (1, 2):
                write_bitmap(directory / f"{name}-{mode}-{scale}x.bmp", grid, palette, scale)
