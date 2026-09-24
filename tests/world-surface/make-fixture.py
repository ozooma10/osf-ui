#!/usr/bin/env python3
"""Stage the preserved QASmoke display fixture in an isolated test mod directory."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


PLUGIN_SHA256 = "63a179b62f89a0df8aabf3fc56246397605942f9b8fbaee30976eb4f537c9c2f"
PLUGIN_NAME = "OSFUIWorldSurfaceTest.esp"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path,
                        help="isolated test mod Data directory; never the production OSF UI mod")
    parser.add_argument("--screens", type=int, choices=range(1, 5), default=1)
    args = parser.parse_args()
    source = Path(__file__).parent / "fixtures" / PLUGIN_NAME
    if hashlib.sha256(source.read_bytes()).hexdigest() != PLUGIN_SHA256:
        raise SystemExit("Preserved plugin fixture checksum changed")
    from world_boards import stage
    stage(args.out, source.read_bytes(), args.screens, PLUGIN_NAME)
    print(f"Staged {args.screens} independent world boards with private materials and textures in {args.out}")



if __name__ == "__main__":
    main()
