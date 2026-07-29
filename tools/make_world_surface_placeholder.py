#!/usr/bin/env python3
"""Generate OSF UI's world-surface placeholder textures.

A world surface identifies its target material by matching the placeholder's
exact dimensions in `ID3D12Device::CreateShaderResourceView` (see
`src/composite/WorldSurface.cpp`), so each texture's size *is* the binding key.
It must stay unique twice over: nothing else the game loads may share it, and
no two configured surfaces may share one — which is why the canonical sizes
below are the only sizes this tool emits by default, one per surface slot.

Uncompressed BGRA8, one mip — matching what the engine's loader produced for
the proof (resource format 90, SRV format 87). Compressed or mipped formats
would not survive the descriptor swap to a shared browser texture. Never run
the output through texconv or Creation Kit recompression.

The visible pattern only ever appears when the binding is *not* working, so it
is designed to be unmistakable rather than pretty: magenta/cyan checks with a
one-cell white border, corner blocks that reveal UV cropping and flips, and a
row of N white pips identifying which surface slot's placeholder you are
looking at.

Usage:
  python tools/make_world_surface_placeholder.py --all
  python tools/make_world_surface_placeholder.py --index 2
  python tools/make_world_surface_placeholder.py --index 1 --out custom.dds
  python tools/make_world_surface_placeholder.py --size 994x994 --out custom.dds
  python tools/make_world_surface_placeholder.py --verify staged.dds
"""

from __future__ import annotations

import argparse
import pathlib
import struct

# DELIBERATELY NOT plausible render-target sizes. The binding matches on
# dimensions, so anything the engine might allocate internally — the backbuffer,
# a half/quarter-res post buffer, a shadow atlas — must never collide. Square
# NPOT sizes satisfy that; 1600x900 did NOT, and hijacking the engine's own
# render targets broke rendering across the whole frame. These four are the
# sizes the config examples and docs use, index-aligned with the surface list.
SIZES = [(1000, 1000), (998, 998), (1002, 1002), (996, 996)]
CELL = 50

DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PIXELFORMAT = 0x1000
# DDSD_PITCH, not DDSD_LINEARSIZE: linear-size is for block-compressed data.
# Getting this wrong produces a header the game's texture loader rejects. The
# reference is the original proof texture — the first 128 bytes of the output
# must match its header exactly (see the header assertion below).
DDSD_PITCH = 0x8
DDPF_ALPHAPIXELS = 0x1
DDPF_RGB = 0x40
DDSCAPS_TEXTURE = 0x1000


def check_size(width: int, height: int) -> str:
    """Mirror of Config::CheckPlaceholderSize — one rule set, two languages.

    Returns the rejection reason, or "" when the size is safe.
    """
    if width != height:
        return "not square (16:9-ish shapes are what engine render targets look like)"
    if width < 256 or width > 8192:
        return "outside the 256-8192 range"
    if width & (width - 1) == 0:
        return "a power of two (the shape of atlases and mip-chained textures)"
    return ""


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


def _pixels(width: int, height: int, pips: int) -> bytearray:
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
    # The pip row identifies the surface slot: N white squares mean this is
    # placeholder N. A screen showing the wrong count is bound to the wrong
    # material/config pairing.
    pip_y0, pip_y1 = CELL * 3, CELL * 4
    pip_spans = [
        (CELL * (4 + 2 * i), CELL * (5 + 2 * i))
        for i in range(pips)
    ]
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
            elif pip_y0 <= y < pip_y1 and any(x0 <= x < x1 for x0, x1 in pip_spans):
                b, g, r = white
            else:
                b, g, r = magenta if ((x // CELL) + (y // CELL)) % 2 == 0 else cyan
            off = row + x * 4
            data[off] = b
            data[off + 1] = g
            data[off + 2] = r
            data[off + 3] = 0xFF
    return data


def _verify(path: pathlib.Path) -> None:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise SystemExit(f"could not read {path}: {exc}") from exc
    if len(data) < 128:
        raise SystemExit(f"{path} is only {len(data)} bytes; not a complete DDS")
    magic, header_size, flags, height, width, pitch, depth, mips = struct.unpack(
        "<4s7I", data[:32]
    )
    if magic != b"DDS " or header_size != 124:
        raise SystemExit(f"{path} does not have a supported legacy DDS header")
    try:
        index = SIZES.index((width, height)) + 1
    except ValueError as exc:
        raise SystemExit(
            f"{path} is {width}x{height}; expected one canonical size: "
            + ", ".join(f"{w}x{h}" for w, h in SIZES)
        ) from exc
    expected = _header(width, height) + bytes(_pixels(width, height, index))
    if data != expected:
        mismatch = next(
            (offset for offset, (actual, wanted) in enumerate(zip(data, expected))
             if actual != wanted),
            min(len(data), len(expected)),
        )
        raise SystemExit(
            f"{path} is not the canonical slot {index} placeholder "
            f"(first mismatch at byte {mismatch}, length {len(data)}; "
            f"expected {len(expected)}). Regenerate it and copy byte-for-byte."
        )
    print(
        f"verified {path} as canonical slot {index} "
        f"({width}x{height} BGRA8, {len(data)} bytes, flags=0x{flags:X}, "
        f"pitch={pitch}, depth={depth}, mips={mips})"
    )

def _default_out(index: int) -> pathlib.Path:
    return (
        pathlib.Path(__file__).resolve().parents[1]
        / "data"
        / "Textures"
        / "OSFUI"
        / f"worldsurface_placeholder{index:02d}.dds"
    )


def _write(index: int, width: int, height: int, out: pathlib.Path) -> None:
    reason = check_size(width, height)
    if reason:
        raise SystemExit(f"refusing unsafe placeholder size {width}x{height}: {reason}")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(_header(width, height) + bytes(_pixels(width, height, index)))
    print(f"wrote {out} ({width}x{height} BGRA8, {index} pip(s), {out.stat().st_size} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    what = parser.add_mutually_exclusive_group(required=True)
    what.add_argument("--all", action="store_true",
        help=f"generate all {len(SIZES)} canonical placeholders")
    what.add_argument("--index", type=int, choices=range(1, len(SIZES) + 1),
        help="generate the canonical placeholder for surface slot N (1-based)")
    what.add_argument("--size", metavar="WxH",
        help="custom size; runs the same safety checks the config parser enforces")
    what.add_argument("--verify", type=pathlib.Path, metavar="DDS",
        help="verify that a staged DDS is byte-exactly one canonical placeholder")
    parser.add_argument("--out", type=pathlib.Path,
        help="output path (default data/Textures/OSFUI/worldsurface_placeholder0N.dds; "
             "required with --size)")
    args = parser.parse_args()

    if args.verify is not None:
        if args.out:
            parser.error("--out cannot be combined with --verify")
        _verify(args.verify)
        return 0
    if args.all:
        if args.out:
            parser.error("--out cannot be combined with --all")
        for i, (width, height) in enumerate(SIZES, start=1):
            _write(i, width, height, _default_out(i))
        return 0
    if args.index is not None:
        width, height = SIZES[args.index - 1]
        _write(args.index, width, height, args.out or _default_out(args.index))
        return 0
    # --size: an escape hatch for authors with a colliding mod; the pip count
    # is 0 because the size is outside the canonical slot table.
    try:
        width, height = (int(part) for part in args.size.lower().split("x"))
    except ValueError:
        parser.error(f"--size wants WxH, got '{args.size}'")
    if not args.out:
        parser.error("--size requires --out (custom sizes never land in data/)")
    _write(0, width, height, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
