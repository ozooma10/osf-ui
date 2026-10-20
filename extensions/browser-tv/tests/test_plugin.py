"""Validate the built TV against its vanilla inputs, without loading the game."""
import argparse
from pathlib import Path
import json
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "extensions/world-screens/tools"))
from world_content import collect, field, fid, records


class BrowserTVContent(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plugin = (DATA / "OSFBrowserTV.esm").read_bytes()
        cls.rows = list(records(cls.plugin))
        cls.forms = {fid(h): (h, fs, path) for h, fs, path in cls.rows}
        cls.vanilla = {
            field(fs, b"EDID").rstrip(b"\0").decode(): fs
            for _, fs, _ in collect(GAME_DATA / "Starfield.esm", editor_ids={
                "DisplayScreen5", "PictureFrame_SetB_01", "co_Outpost_Misc_ClearBountyTerminal",
                "co_Outpost_Decor_WallMonitor01"}, groups={b"STAT", b"COBJ"}).values()
        }

    def test_full_esm_only_new_records_and_one_master(self):
        self.assertEqual(len(self.rows), 4)
        self.assertEqual(set(self.forms), {0, 0x01000800, 0x01000801, 0x01000802})
        h, fs, path = self.forms[0]
        self.assertEqual(h[:4], b"TES4")
        self.assertEqual(struct.unpack_from("<I", h, 8)[0], 1)  # No localized/small/medium flags.
        self.assertEqual([v for t, v in fs if t == b"MAST"], [b"Starfield.esm\0"])
        self.assertFalse(any(t == b"ONAM" for t, _ in fs))
        self.assertEqual(struct.unpack("<fII", field(fs, b"HEDR"))[1:], (6, 0x803))
        for form, tag in ((0x01000800, b"ACTI"), (0x01000801, b"COBJ"), (0x01000802, b"MESG")):
            h, _, path = self.forms[form]
            self.assertEqual(h[:4], tag)
            self.assertEqual(len(path), 1)
            self.assertEqual(path[0][8:12], tag)

    def test_model_bounds_and_wall_mounting_preserved(self):
        fs = self.forms[0x01000800][1]
        self.assertEqual(field(fs, b"FULL"), b"Browser TV\0")
        for tag in (b"OBND", b"MODL"):
            self.assertEqual(field(fs, tag), field(self.vanilla["DisplayScreen5"], tag))
        for tag in (b"PTT2", b"SNTP"):
            self.assertEqual(field(fs, tag), field(self.vanilla["PictureFrame_SetB_01"], tag))

    def test_script_property_resolves_to_watch_message(self):
        vmad = field(self.forms[0x01000800][1], b"VMAD")
        self.assertEqual(struct.unpack_from("<HHH", vmad), (6, 2, 1))
        offset = 6
        def string():
            nonlocal offset
            size = struct.unpack_from("<H", vmad, offset)[0]
            offset += 2
            value = vmad[offset:offset + size].decode()
            offset += size
            return value
        self.assertEqual(string(), "OSFBrowserTV")
        self.assertEqual(struct.unpack_from("<BH", vmad, offset), (0, 1))
        offset += 3
        self.assertEqual(string(), "WatchText")
        self.assertEqual(vmad[offset:offset + 2], b"\1\1")
        offset += 2
        unused, alias, form = struct.unpack_from("<HhI", vmad, offset)
        self.assertEqual((unused, alias), (0, -1))
        self.assertEqual(field(self.forms[form][1], b"FULL"), b"Watch\0")
        self.assertEqual(offset + 8, len(vmad))

    def test_recipe_creates_tv_and_preserves_cost_without_unlock_condition(self):
        fs = self.forms[0x01000801][1]
        self.assertEqual(struct.unpack("<I", field(fs, b"CNAM"))[0], 0x01000800)
        recipe = self.vanilla["co_Outpost_Misc_ClearBountyTerminal"]
        for tag in (b"FVPA", b"NNAM", b"SNAM", b"TNAM", b"PUSH", b"PDSH", b"LRNM", b"DATA", b"RECF"):
            self.assertEqual(field(fs, tag), field(recipe, tag))
        costs = list(struct.iter_unpack("<III", field(fs, b"FVPA")))
        self.assertEqual(costs, [(0x57D6, 4, 0), (0x24576E, 2, 0), (0x57D8, 2, 0)])
        self.assertFalse(any(t in (b"CTDA", b"CIS1", b"CIS2") for t, _ in fs))
        for tag in (b"BNAM", b"FNAM"):
            self.assertEqual(field(fs, tag), field(self.vanilla["co_Outpost_Decor_WallMonitor01"], tag))

    def test_catalog_and_payload_ownership(self):
        catalog = json.loads((DATA / "SFSE/Plugins/OSF/WorldScreens/screens/osf.browser-tv.json").read_text())
        self.assertEqual(catalog, {"version": 1, "bases": [
            {"plugin": "OSFBrowserTV.esm", "formId": "800", "profile": "display-screen5"}]})
        self.assertEqual({str(p.relative_to(DATA)).replace("\\", "/") for p in DATA.rglob("*") if p.is_file()}, {
            "OSFBrowserTV.esm", "Scripts/OSFBrowserTV.pex", "Scripts/Source/OSFBrowserTV.psc",
            "SFSE/Plugins/OSF/WorldScreens/screens/osf.browser-tv.json",
            "OSF Browser TV/README.md", "OSF Browser TV/LICENSE", "OSF Browser TV/EXCEPTIONS"})
        self.assertGreater((DATA / "Scripts/OSFBrowserTV.pex").stat().st_size, 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--game-data", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    DATA, GAME_DATA = args.data, args.game_data
    unittest.main(argv=[sys.argv[0], *remaining])
