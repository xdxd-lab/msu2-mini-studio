#!/usr/bin/env python3
"""Create deterministic, pixel-exact 160x80 PPM display diagnostics.

The images are code-generated because the exact region boundaries and RGB565
colors are part of the test stimulus. No external imaging library is needed.
"""
from pathlib import Path
import hashlib
import struct
import zlib

WIDTH, HEIGHT = 160, 80
OUT = Path(__file__).resolve().parent / "images"


def canvas(color=(0, 0, 0)):
    return bytearray(bytes(color) * (WIDTH * HEIGHT))


def put(buf, x, y, color):
    if 0 <= x < WIDTH and 0 <= y < HEIGHT:
        at = (y * WIDTH + x) * 3
        buf[at:at + 3] = bytes(color)


def rect(buf, x, y, w, h, color):
    for yy in range(max(0, y), min(HEIGHT, y + h)):
        for xx in range(max(0, x), min(WIDTH, x + w)):
            put(buf, xx, yy, color)


def line(buf, x0, y0, x1, y1, color):
    dx = abs(x1 - x0)
    sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0)
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        put(buf, x0, y0, color)
        if x0 == x1 and y0 == y1:
            return
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def border(buf, color=(255, 255, 255)):
    rect(buf, 0, 0, WIDTH, 2, color)
    rect(buf, 0, HEIGHT - 2, WIDTH, 2, color)
    rect(buf, 0, 0, 2, HEIGHT, color)
    rect(buf, WIDTH - 2, 0, 2, HEIGHT, color)


def color_bars():
    buf = canvas()
    colors = ((255, 255, 255), (255, 255, 0), (0, 255, 255),
              (0, 255, 0), (255, 0, 255), (255, 0, 0),
              (0, 0, 255), (0, 0, 0))
    for i, color in enumerate(colors):
        rect(buf, i * 20, 0, 20, HEIGHT, color)
    return buf


def geometry_grid():
    buf = canvas((12, 12, 16))
    for x in range(0, WIDTH, 8):
        line(buf, x, 0, x, HEIGHT - 1, (46, 52, 60))
    for y in range(0, HEIGHT, 8):
        line(buf, 0, y, WIDTH - 1, y, (46, 52, 60))
    for x in (40, 80, 120):
        rect(buf, x - 1, 0, 2, HEIGHT, (255, 255, 0))
    for y in (20, 40, 60):
        rect(buf, 0, y - 1, WIDTH, 2, (0, 255, 255))
    # Asymmetric corner markers reveal flips and rotations.
    rect(buf, 3, 3, 11, 11, (255, 0, 0))
    rect(buf, WIDTH - 14, 3, 11, 11, (0, 255, 0))
    rect(buf, 3, HEIGHT - 14, 11, 11, (0, 0, 255))
    rect(buf, WIDTH - 14, HEIGHT - 14, 11, 11, (255, 0, 255))
    return buf


def bands4():
    buf = canvas()
    colors = ((180, 24, 24), (180, 140, 0), (0, 130, 180), (100, 0, 180))
    for band, color in enumerate(colors):
        rect(buf, 0, band * 20, WIDTH, 20, color)
        for x in range(8 + band * 4, WIDTH - 4, 16):
            rect(buf, x, band * 20 + 8, 3, 4, (255, 255, 255))
        if band:
            rect(buf, 0, band * 20, WIDTH, 1, (255, 255, 255))
    return buf


def center3():
    buf = canvas()
    rect(buf, 0, 0, 40, HEIGHT, (0, 45, 180))
    rect(buf, 40, 0, 80, HEIGHT, (35, 35, 35))
    rect(buf, 120, 0, 40, HEIGHT, (180, 35, 0))
    for y in range(4, HEIGHT, 12):
        rect(buf, 9, y, 21, 4, (100, 175, 255))
        rect(buf, 130, y + 3, 21, 4, (255, 170, 90))
    for x in range(48, 120, 12):
        rect(buf, x, 18, 5, 44, (255, 255, 255))
    rect(buf, 39, 0, 2, HEIGHT, (255, 255, 0))
    rect(buf, 119, 0, 2, HEIGHT, (255, 255, 0))
    return buf


def halves2():
    buf = canvas()
    rect(buf, 0, 0, 80, HEIGHT, (0, 110, 170))
    rect(buf, 80, 0, 80, HEIGHT, (175, 80, 0))
    for y in range(8, 76, 12):
        rect(buf, 12, y, 52, 4, (255, 255, 255))
        rect(buf, 96, y + 3, 52, 4, (0, 0, 0))
    rect(buf, 79, 0, 2, HEIGHT, (255, 255, 0))
    return buf


def boundary_targets():
    buf = canvas((6, 10, 18))
    for i, x in enumerate((40, 80, 120)):
        rect(buf, x - 7, 7 + i * 19, 14, 14,
             ((255, 30, 30), (30, 255, 30), (30, 90, 255))[i])
        line(buf, x, 0, x, HEIGHT - 1, (170, 170, 170))
    for y in (20, 40, 60):
        line(buf, 0, y, WIDTH - 1, y, (130, 130, 130))
    border(buf)
    return buf


def checker1():
    buf = canvas()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            value = 255 if (x + y) % 2 else 0
            put(buf, x, y, (value, value, value))
    return buf


def gradient():
    buf = canvas()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            put(buf, x, y, (x * 255 // 159, y * 255 // 79, (x * 3 + y * 5) % 256))
    return buf


def noise():
    buf = canvas()
    state = 0x6D737532
    for i in range(len(buf)):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        state &= 0xFFFFFFFF
        buf[i] = state & 255
    return buf


IMAGES = {
    "01_color_bars": color_bars,
    "02_geometry_grid": geometry_grid,
    "03_bands4": bands4,
    "04_center3": center3,
    "05_halves2": halves2,
    "06_boundary_targets": boundary_targets,
    "07_checker1": checker1,
    "08_gradient": gradient,
    "09_noise": noise,
    "10_black": lambda: canvas((0, 0, 0)),
    "11_white": lambda: canvas((255, 255, 255)),
}


def png_chunk(kind, payload):
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def write_png(path, rgb, width=WIDTH, height=HEIGHT):
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     png_chunk(b"IDAT", zlib.compress(rows, 9)) +
                     png_chunk(b"IEND", b""))


def generate(directory=OUT):
    directory.mkdir(parents=True, exist_ok=True)
    lines = []
    previews = []
    for name, make in IMAGES.items():
        rgb = make()
        if len(rgb) != WIDTH * HEIGHT * 3:
            raise ValueError("wrong image size: " + name)
        ppm = directory / (name + ".ppm")
        png = directory / (name + ".png")
        ppm.write_bytes(f"P6\n{WIDTH} {HEIGHT}\n255\n".encode("ascii") + rgb)
        write_png(png, rgb)
        previews.append(rgb)
        for path in (ppm, png):
            lines.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n")
    gap = 4
    columns = 4
    rows = (len(previews) + columns - 1) // columns
    sheet_width = columns * WIDTH + (columns + 1) * gap
    sheet_height = rows * HEIGHT + (rows + 1) * gap
    sheet = bytearray(bytes((35, 35, 35)) * (sheet_width * sheet_height))
    for index, rgb in enumerate(previews):
        left = gap + (index % columns) * (WIDTH + gap)
        top = gap + (index // columns) * (HEIGHT + gap)
        for y in range(HEIGHT):
            start = ((top + y) * sheet_width + left) * 3
            sheet[start:start + WIDTH * 3] = rgb[y * WIDTH * 3:(y + 1) * WIDTH * 3]
    contact = directory / "contact_sheet.png"
    write_png(contact, sheet, sheet_width, sheet_height)
    lines.append(f"{hashlib.sha256(contact.read_bytes()).hexdigest()}  {contact.name}\n")
    (directory / "SHA256SUMS.txt").write_text("".join(lines), encoding="utf-8")
    return directory


if __name__ == "__main__":
    print(generate())
