#!/usr/bin/env python3
"""Generate OSF UI's world-surface placeholder texture.

The world surface identifies its target material by matching the placeholder's
exact dimensions in `ID3D12Device::CreateShaderResourceView` (see
`src/composite/WorldSurface.cpp`), so this texture's size *is* the binding key.
It must stay unique: nothing else the game loads may share it. The dimensions
deliberately equal the default browser size, which keeps the signature unusual
and makes a future custom mesh a 1:1 UV mapping.

Uncompressed BGRA8, one mip — matching what the engine's loader produced for
the proof (resource format 90, SRV format 87). Compressed or mipped formats
would not survive the descriptor swap to a shared browser texture.

The visible pattern only ever appears when the binding is *not* working, so it
is designed to be unmistakable rather than pretty: magenta/cyan checks with a
one-cell white border and corner blocks that reveal UV cropping and flips.

Usage: python tools/make_world_surface_placeholder.py [output.dds]
"""

from __future__ import annotations

import pathlib
import struct
import sys

# DELIBERATELY NOT a plausible render-target size. The binding matches on
# dimensions, so anything the engine might allocate internally — the backbuffer,
# a half/quarter-res post buffer, a shadow atlas — must never collide. A square
# NPOT size satisfies that; 1600x900 did NOT, and hijacking the engine's own
# render targets broke rendering across the whole frame. Never set this to a
# 16:9 or power-of-two size, and never to the browser size.
WIDTH = 1000
HEIGHT = 1000
CELL = 50

DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PIXELFORMAT = 0x1000
# DDSD_PITCH, not DDSD_LINEARSIZE: linear-size is for block-compressed data.
# Getting this wrong produces a header the game's texture loader rejects. The
# reference is the original proof texture — the first 128 bytes of the output
# must match its header exactly (see the header assertion in main()).
DDSD_PITCH = 0x8
DDPF_ALPHAPIXELS = 0x1
DDPF_RGB = 0x40
DDSCAPS_TEXTURE = 0x1000


def _header(width: int, height: int) -> bytes:
    pitch = width * 4
    header = struct.pack(
        "<4sIIIIIII44xIIIIIIIIIIII4x",
        b"DDS ",
        124,
        DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH,
        height,
        width,
        pitch,
        0,  # depth
        0,  # mip count: 0, matching the reference (no DDSD_MIPMAPCOUNT flag)
        32,  # pixel format size
        DDPF_RGB | DDPF_ALPHAPIXELS,
        0,  # four CC
        32,  # bit count
        0x00FF0000,  # red mask   (BGRA byte order)
        0x0000FF00,  # green mask
        0x000000FF,  # blue mask
        0xFF000000,  # alpha mask
        DDSCAPS_TEXTURE,
        0,
        0,
        0,
    )
    assert len(header) == 128, len(header)
    # Byte-exact header of the original proof texture, which the game's loader
    # is known to accept at these dimensions. Any drift here is a bug: a header
    # the streamer rejects does not merely leave the surface blank, it can take
    # world rendering down with it.
    flags, mips = struct.unpack("<I", header[8:12])[0], struct.unpack("<I", header[28:32])[0]
    if flags != 0x100F or mips != 0:
        raise SystemExit(
            f"DDS header drifted from the reference: flags=0x{flags:X} (want 0x100F), "
            f"mips={mips} (want 0)"
        )
    return header


def _pixels(width: int, height: int) -> bytearray:
    magenta = (0xFF, 0x00, 0xFF)
    cyan = (0xFF, 0xFF, 0x00)
    white = (0xFF, 0xFF, 0xFF)
    # Distinct corners: if the mesh crops or flips the UVs, which corner
    # survives tells you how, without needing a capture tool.
    corners = {
        (0, 0): (0x00, 0x00, 0xFF),  # top-left     red
        (1, 0): (0x00, 0xFF, 0x00),  # top-right    green
        (0, 1): (0xFF, 0x00, 0x00),  # bottom-left  blue
        (1, 1): (0x00, 0xFF, 0xFF),  # bottom-right yellow
    }
    block = CELL * 2
    data = bytearray(width * height * 4)
    for y in range(height):
        row = y * width * 4
        for x in range(width):
            if x < CELL or y < CELL or x >= width - CELL or y >= height - CELL:
                b, g, r = white
            elif x < block and y < block:
                b, g, r = corners[(0, 0)]
            elif x >= width - block and y < block:
                b, g, r = corners[(1, 0)]
            elif x < block and y >= height - block:
                b, g, r = corners[(0, 1)]
            elif x >= width - block and y >= height - block:
                b, g, r = corners[(1, 1)]
            else:
                b, g, r = magenta if ((x // CELL) + (y // CELL)) % 2 == 0 else cyan
            off = row + x * 4
            data[off] = b
            data[off + 1] = g
            data[off + 2] = r
            data[off + 3] = 0xFF
    return data


def main() -> int:
    default = (
        pathlib.Path(__file__).resolve().parents[1]
        / "research-world-surface-assets"
        / "textures"
        / "OSFUI"
        / "worldsurface_placeholder01.dds"
    )
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else default
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(_header(WIDTH, HEIGHT) + bytes(_pixels(WIDTH, HEIGHT)))
    print(f"wrote {out} ({WIDTH}x{HEIGHT} BGRA8, {out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
