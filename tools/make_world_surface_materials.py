#!/usr/bin/env python3
"""Regenerate OSF UI's world-surface material overrides from the vanilla ones.

The world surface needs the target material to sample an OSF UI-owned
placeholder texture rather than a vanilla one. Previously OSF UI shipped a
loose `ShipScreen_Avionics01_color.dds`, but that file is referenced by six
different cockpit materials (the base plus the Deimos/HopeTech/Stroud/Taiyo
variants), so the override replaced the avionics screen on every manufacturer's
ship and any of those materials could produce a matching SRV.

Overriding the *materials* instead keeps the vanilla texture intact for
everything else and points only the avionics screens at our own placeholder.

Starfield materials are JSON with an `Import` inheritance chain and `res:`
content-database object IDs. This script copies each vanilla file verbatim and
rewrites only the texture filename strings, so every `res:` ID, edge, and
parent link stays exactly as Bethesda authored it. Do not hand-author a
material at a *new* path by copying one of these: the `res:` IDs would be
duplicated, and we do not know the allocation scheme. A genuinely OSF UI-owned
material needs the Creation Kit to mint fresh IDs.

Usage: python tools/make_world_surface_materials.py [--game <Starfield Data dir>]
"""

from __future__ import annotations

import argparse
import pathlib
import sys

# Relative to the game's Data directory.
MATERIAL_DIR = pathlib.Path("Materials/Ships/Interior/CockpitScreens")
# Only these two reference the shared base texture. The Deimos/HopeTech/Stroud/
# Taiyo variants each have their own `<Maker>_ShipScreen_Avionics01_color.dds`
# and are deliberately left alone — a filename-substring search misleadingly
# suggests otherwise.
MATERIALS = [
    "ShipScreen_Avionics01.mat",
    "ShipScreen_Avionics01_A.mat",
]

# Paths inside the JSON are backslash-escaped, so these are the literal
# character sequences as they appear in the file text.
VANILLA_TEXTURE = "ships\\\\interior\\\\cockpitscreens\\\\shipscreen_avionics01_color.dds"
OSFUI_TEXTURE = "Data\\\\Textures\\\\OSFUI\\\\worldsurface_placeholder01.dds"

DEFAULT_GAME = pathlib.Path(
    r"C:\Program Files (x86)\Steam\steamapps\common\Starfield\Data"
)


def _retarget(text: str) -> tuple[str, int]:
    """Replace every reference to the vanilla screen texture, preserving the
    surrounding JSON byte-for-byte. Matching is case-insensitive because the
    vanilla files are inconsistent about `.DDS` vs `.dds`."""
    needle = VANILLA_TEXTURE.lower()
    out: list[str] = []
    lowered = text.lower()
    hits = 0
    i = 0
    while True:
        # The stored path may or may not carry a "Data\\" prefix; anchor on the
        # part that is always present and walk back over an optional prefix.
        found = lowered.find(needle, i)
        if found < 0:
            out.append(text[i:])
            break
        start = found
        prefix = "data\\\\textures\\\\"
        if lowered[max(0, found - len(prefix)):found] == prefix:
            start = found - len(prefix)
        out.append(text[i:start])
        out.append(OSFUI_TEXTURE)
        i = found + len(needle)
        hits += 1
    return "".join(out), hits


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", type=pathlib.Path, default=DEFAULT_GAME)
    args = parser.parse_args()

    if not args.game.is_dir():
        print(f"game Data directory not found: {args.game}", file=sys.stderr)
        return 1

    repo = pathlib.Path(__file__).resolve().parents[1]
    dest_root = repo / "data" / "assets" / "materials" / MATERIAL_DIR.relative_to("Materials")
    total = 0
    for rel in MATERIALS:
        src = args.game / MATERIAL_DIR / rel
        if not src.is_file():
            print(f"missing vanilla material: {src}", file=sys.stderr)
            return 1
        text = src.read_text(encoding="utf-8")
        retargeted, hits = _retarget(text)
        if hits == 0:
            print(f"no texture reference rewritten in {rel}", file=sys.stderr)
            return 1
        dest = dest_root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(retargeted, encoding="utf-8", newline="")
        print(f"{rel}: {hits} reference(s) retargeted")
        total += hits
    print(f"{len(MATERIALS)} material(s), {total} reference(s) -> {OSFUI_TEXTURE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
