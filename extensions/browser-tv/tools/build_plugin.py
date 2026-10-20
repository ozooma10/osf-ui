"""Build the Papyrus-only Browser TV using the existing Starcade workshop recipe.

Starfield.esm is a read-only input. No placed references, vanilla overrides,
framework binaries, web pages or copied vanilla meshes belong in this payload.
"""
import argparse
from collections import OrderedDict
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "extensions/world-screens/tools"))
from world_content import collect, encode_record, field, fid, records

PLUGIN = "OSFBrowserTV.esm"
SCRIPT = "OSFBrowserTV"
OWN = 0x01000000  # One master: Starfield.esm. A full ESM, not a small/medium plugin.
TV, RECIPE, WATCH = (OWN | x for x in (0x800, 0x801, 0x802))
MODEL = "setdressing/displayscreens/displayscreen5.nif"
TEMPLATES = {
    "Neon_Kiosk_TerraBrew_01": b"ACTI",
    "DisplayScreen5": b"STAT",
    "PictureFrame_SetB_01": b"STAT",
    "co_Outpost_Misc_ClearBountyTerminal": b"COBJ",
    "co_Outpost_Decor_WallMonitor01": b"COBJ",
}


def text(value):
    return value.encode("utf-8") + b"\0"


def wstr(value):
    value = value.encode("utf-8")
    return struct.pack("<H", len(value)) + value


def script_attachment():
    # VMAD v6/object format 2: one script, one edited Object property.
    return (struct.pack("<HHH", 6, 2, 1) + wstr(SCRIPT) + struct.pack("<BH", 0, 1)
            + wstr("WatchText") + bytes((1, 1)) + struct.pack("<HhI", 0, -1, WATCH))


def make_plugin(templates):
    monitor = templates["DisplayScreen5"]
    mounting = templates["PictureFrame_SetB_01"]
    model = field(monitor, b"MODL")
    if model.rstrip(b"\0").decode().lower().replace("\\", "/") != MODEL:
        raise ValueError("DisplayScreen5 no longer uses the supported model")

    # Use the same supported ACTI and workshop mounting data as Starcade's TV.
    # Explicit fields avoid inheriting localized strings, scripts or kiosk behavior.
    kiosk = templates["Neon_Kiosk_TerraBrew_01"]
    activator = [
        (b"EDID", text("OSFBrowserTV")), (b"VMAD", script_attachment()),
        (b"OBND", field(monitor, b"OBND")), (b"ODTY", field(kiosk, b"ODTY")),
        (b"PTT2", field(mounting, b"PTT2")), (b"SNTP", field(mounting, b"SNTP")),
        (b"FULL", text("Browser TV")), (b"MODL", model),
        (b"FLLD", field(kiosk, b"FLLD")), (b"PNAM", field(kiosk, b"PNAM")),
        (b"FNAM", field(kiosk, b"FNAM")), (b"JNAM", field(kiosk, b"JNAM")),
    ]
    recipe = []
    replacements = {
        b"EDID": text("co_Outpost_Decor_OSFBrowserTV"),
        b"DESC": text("A wall TV for web videos and music. Activate to watch. "
                      "Web browsing requires consent in Mods > OSF UI and a restart."),
        b"CNAM": struct.pack("<I", TV),
        b"BNAM": field(templates["co_Outpost_Decor_WallMonitor01"], b"BNAM"),
        b"FNAM": field(templates["co_Outpost_Decor_WallMonitor01"], b"FNAM"),
    }
    for tag, value in templates["co_Outpost_Misc_ClearBountyTerminal"]:
        recipe.append((tag, replacements.get(tag, value)))
    for tag in replacements:
        field(recipe, tag)  # Missing/duplicate source fields must fail the build.
    if any(tag in (b"CTDA", b"CIS1", b"CIS2", b"VMAD", b"FULL") for tag, _ in recipe):
        raise ValueError("Unexpected gated/scripted/localized recipe template")
    message = [
        (b"EDID", text("OSFBrowserTVWatch")), (b"DESC", text("")),
        (b"FULL", text("Watch")), (b"INAM", struct.pack("<I", 0)),
        (b"DNAM", struct.pack("<I", 0)), (b"BNAM", struct.pack("<i", 0)),
    ]

    def record(tag, form, fields, flags=0):
        return encode_record(struct.pack("<4sIIIIHH", tag, 0, flags, form, 0, 582, 0), fields)

    groups = OrderedDict(((b"ACTI", (TV, activator)), (b"COBJ", (RECIPE, recipe)),
                          (b"MESG", (WATCH, message))))
    body = b""
    for tag, (form, fields) in groups.items():
        payload = record(tag, form, fields)
        body += struct.pack("<4sI4sIII", b"GRUP", 24 + len(payload), tag, 0, 0, 0) + payload
    header = [
        (b"HEDR", struct.pack("<fII", .96, 6, 0x803)),
        (b"CNAM", text("OSF")), (b"SNAM", text("Craftable World Screens browser TV")),
        (b"MAST", text("Starfield.esm")), (b"INCC", struct.pack("<I", 0)),
    ]
    return record(b"TES4", 0, header, flags=1) + body


def build(game_data, out):
    source = collect(game_data / "Starfield.esm", editor_ids=TEMPLATES,
                     groups=set(TEMPLATES.values()))
    templates = {}
    provenance = {}
    for header, fields, _ in source.values():
        name = field(fields, b"EDID").rstrip(b"\0").decode()
        if name in templates or header[:4] != TEMPLATES[name]:
            raise ValueError(f"Unexpected or duplicate template: {name}")
        templates[name] = fields
        provenance[name] = f"{fid(header):08X}"
    if templates.keys() != TEMPLATES.keys():
        raise ValueError(f"Missing templates: {TEMPLATES.keys() - templates.keys()}")
    plugin = make_plugin(templates)
    decoded = list(records(plugin))
    if [fid(h) for h, _, _ in decoded] != [0, TV, RECIPE, WATCH]:
        raise ValueError("Generated plugin has unexpected records")
    out.mkdir(parents=True, exist_ok=True)
    (out / PLUGIN).write_bytes(plugin)
    catalog = out / "SFSE/Plugins/OSF/WorldScreens/screens/osf.browser-tv.json"
    catalog.parent.mkdir(parents=True, exist_ok=True)
    catalog.write_text(json.dumps({"version": 1, "bases": [
        {"plugin": PLUGIN, "formId": "800", "profile": "display-screen5"}
    ]}, indent=2) + "\n", encoding="utf-8")
    evidence = {"plugin": PLUGIN, "sha256": hashlib.sha256(plugin).hexdigest(),
                "templates": provenance, "runtimeValidated": False}
    (out.parent / "content-build.json").write_text(
        json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(f"Built {PLUGIN}: Browser TV, Decorations recipe, Watch activation text.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-data", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    build(args.game_data, args.out)
