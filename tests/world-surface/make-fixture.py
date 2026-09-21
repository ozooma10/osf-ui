#!/usr/bin/env python3
"""Stage the preserved QASmoke display fixture in an isolated test mod directory."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import struct


PLUGIN_SHA256 = "63a179b62f89a0df8aabf3fc56246397605942f9b8fbaee30976eb4f537c9c2f"
DDS_SHA256 = "817905fb906c5ee63ba30511dc985b092ca133d91455cae5d49a4fb1cf9edd21"
PLUGIN_NAME = "OSFUIWorldSurfaceTest.esp"


def placeholder() -> bytes:
    """Byte-identical to the 1000x1000 BGRA8 texture proven on 2026-07-29."""
    header = struct.pack(
        "<4sIIIIIII44xIIIIIIIIIIII4x",
        b"DDS ", 124, 0x100F, 1000, 1000, 4000, 0, 0,
        32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF,
        0xFF000000, 0x1000, 0, 0, 0,
    )
    white = bytes((255, 255, 255, 255))
    magenta = bytes((255, 0, 255, 255))
    cyan = bytes((255, 255, 0, 255))
    corners = {
        (1, 1): bytes((0, 0, 255, 255)),
        (18, 1): bytes((0, 255, 0, 255)),
        (1, 18): bytes((255, 0, 0, 255)),
        (18, 18): bytes((0, 255, 255, 255)),
    }
    rows = []
    for y in range(20):
        cells = []
        for x in range(20):
            if x in (0, 19) or y in (0, 19) or (x == 4 and y == 3):
                color = white
            else:
                color = corners.get((x, y), magenta if (x + y) % 2 == 0 else cyan)
            cells.append(color * 50)
        rows.append(b"".join(cells) * 50)
    result = header + b"".join(rows)
    if hashlib.sha256(result).hexdigest() != DDS_SHA256:
        raise RuntimeError("Generated placeholder no longer matches the proven texture")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path,
                        help="isolated test mod Data directory; never the production OSF UI mod")
    args = parser.parse_args()
    source = Path(__file__).parent / "fixtures" / PLUGIN_NAME
    if hashlib.sha256(source.read_bytes()).hexdigest() != PLUGIN_SHA256:
        raise SystemExit("Preserved plugin fixture checksum changed")
    data = placeholder()
    args.out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, args.out / PLUGIN_NAME)
    textures = args.out / "Textures/Architecture/City/NewAtlantis/Lodge"
    textures.mkdir(parents=True, exist_ok=True)
    for suffix in ("color", "emissive"):
        (textures / f"NA_Lodge_Space01_{suffix}.DDS").write_bytes(data)
    print(f"Staged {PLUGIN_NAME} and two verified placeholder textures in {args.out}")


if __name__ == "__main__":
    main()
