#!/usr/bin/env python3
"""Convert a 24/32-bit BMP into an RGB565 palette + RLE Arduino header.

The script intentionally uses only Python's standard library so the firmware
asset can be rebuilt on a stock macOS machine after resizing with `sips`.
"""

from __future__ import annotations

import argparse
import struct
from collections import Counter
from pathlib import Path


Color = tuple[int, int, int]
WeightedColor = tuple[Color, int]


def read_bmp(path: Path) -> tuple[int, int, list[Color]]:
    data = path.read_bytes()
    if data[:2] != b"BM":
        raise ValueError("input is not a BMP file")

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    if dib_size < 40:
        raise ValueError("unsupported BMP header")

    width, signed_height = struct.unpack_from("<ii", data, 18)
    planes, bits_per_pixel = struct.unpack_from("<HH", data, 26)
    compression = struct.unpack_from("<I", data, 30)[0]
    if planes != 1 or bits_per_pixel not in (24, 32) or compression != 0:
        raise ValueError("expected an uncompressed 24-bit or 32-bit BMP")

    height = abs(signed_height)
    bottom_up = signed_height > 0
    bytes_per_pixel = bits_per_pixel // 8
    row_stride = (width * bytes_per_pixel + 3) & ~3
    pixels: list[Color] = []

    for output_y in range(height):
        source_y = height - 1 - output_y if bottom_up else output_y
        row_start = pixel_offset + source_y * row_stride
        for x in range(width):
            offset = row_start + x * bytes_per_pixel
            blue, green, red = data[offset : offset + 3]
            pixels.append((red, green, blue))

    return width, height, pixels


def channel_range(box: list[WeightedColor], channel: int) -> int:
    values = [entry[0][channel] for entry in box]
    return max(values) - min(values)


def split_box(box: list[WeightedColor]) -> tuple[list[WeightedColor], list[WeightedColor]]:
    channel = max(range(3), key=lambda item: channel_range(box, item))
    ordered = sorted(box, key=lambda entry: entry[0][channel])
    halfway = sum(weight for _, weight in ordered) / 2
    running = 0
    split_at = 1
    for index, (_, weight) in enumerate(ordered[:-1], start=1):
        running += weight
        if running >= halfway:
            split_at = index
            break
    return ordered[:split_at], ordered[split_at:]


def median_cut(pixels: list[Color], color_count: int) -> list[Color]:
    boxes: list[list[WeightedColor]] = [list(Counter(pixels).items())]
    while len(boxes) < color_count:
        candidates = [
            (max(channel_range(box, channel) for channel in range(3))
             * sum(weight for _, weight in box), index)
            for index, box in enumerate(boxes)
            if len(box) > 1
        ]
        if not candidates:
            break
        _, index = max(candidates)
        left, right = split_box(boxes.pop(index))
        boxes.extend((left, right))

    palette: list[Color] = []
    for box in boxes:
        total = sum(weight for _, weight in box)
        palette.append(tuple(
            round(sum(color[channel] * weight for color, weight in box) / total)
            for channel in range(3)
        ))
    return palette


def nearest_palette_indices(pixels: list[Color], palette: list[Color]) -> list[int]:
    cache: dict[Color, int] = {}
    indices: list[int] = []
    for color in pixels:
        if color not in cache:
            cache[color] = min(
                range(len(palette)),
                key=lambda index: sum(
                    (color[channel] - palette[index][channel]) ** 2
                    for channel in range(3)
                ),
            )
        indices.append(cache[color])
    return indices


def encode_rle(indices: list[int]) -> list[int]:
    encoded: list[int] = []
    start = 0
    while start < len(indices):
        value = indices[start]
        run = 1
        while start + run < len(indices) and indices[start + run] == value and run < 255:
            run += 1
        encoded.extend((run, value))
        start += run
    return encoded


def rgb565(color: Color) -> int:
    red, green, blue = color
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def rgb565_to_rgb(value: int) -> Color:
    red = ((value >> 11) & 0x1F) * 255 // 31
    green = ((value >> 5) & 0x3F) * 255 // 63
    blue = (value & 0x1F) * 255 // 31
    return red, green, blue


def canonicalize_rgb565_palette(
    palette: list[Color], indices: list[int]
) -> tuple[list[Color], list[int]]:
    """Merge palette entries that become identical on the RGB565 display."""
    packed_to_index: dict[int, int] = {}
    remap: list[int] = []
    canonical: list[Color] = []

    for color in palette:
        packed = rgb565(color)
        if packed not in packed_to_index:
            packed_to_index[packed] = len(canonical)
            canonical.append(rgb565_to_rgb(packed))
        remap.append(packed_to_index[packed])

    return canonical, [remap[index] for index in indices]


def format_values(values: list[int], width: int, formatter) -> str:
    lines = []
    for offset in range(0, len(values), width):
        lines.append("  " + ", ".join(formatter(value) for value in values[offset : offset + width]) + ",")
    return "\n".join(lines)


def write_header(path: Path, width: int, height: int, palette: list[Color], encoded: list[int]) -> None:
    content = f"""// Generated by tools/image_to_palette.py. Do not edit by hand.
#pragma once

#include <Arduino.h>

constexpr uint16_t BUDDY_BOOT_WIDTH = {width};
constexpr uint16_t BUDDY_BOOT_HEIGHT = {height};
constexpr uint8_t BUDDY_BOOT_PALETTE_SIZE = {len(palette)};
constexpr uint32_t BUDDY_BOOT_RLE_SIZE = {len(encoded)};

const uint16_t PROGMEM BUDDY_BOOT_PALETTE[BUDDY_BOOT_PALETTE_SIZE] = {{
{format_values([rgb565(color) for color in palette], 8, lambda value: f'0x{value:04X}')}
}};

const uint8_t PROGMEM BUDDY_BOOT_RLE[BUDDY_BOOT_RLE_SIZE] = {{
{format_values(encoded, 20, lambda value: str(value))}
}};
"""
    path.write_text(content)


def write_ppm(path: Path, width: int, height: int, palette: list[Color], indices: list[int]) -> None:
    with path.open("wb") as output:
        output.write(f"P6\n{width} {height}\n255\n".encode())
        for index in indices:
            output.write(bytes(palette[index]))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_bmp", type=Path)
    parser.add_argument("output_header", type=Path)
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--colors", type=int, default=64)
    args = parser.parse_args()

    if not 2 <= args.colors <= 256:
        parser.error("--colors must be between 2 and 256")

    width, height, pixels = read_bmp(args.input_bmp)
    palette = median_cut(pixels, args.colors)
    indices = nearest_palette_indices(pixels, palette)
    palette, indices = canonicalize_rgb565_palette(palette, indices)
    encoded = encode_rle(indices)
    if sum(encoded[0::2]) != width * height:
        raise RuntimeError("RLE validation failed")
    write_header(args.output_header, width, height, palette, encoded)
    if args.preview:
        write_ppm(args.preview, width, height, palette, indices)

    raw_size = width * height * 2
    asset_size = len(palette) * 2 + len(encoded)
    print(f"{width}x{height}: {len(palette)} colors, {asset_size} bytes ({asset_size / raw_size:.1%} of RGB565)")


if __name__ == "__main__":
    main()
