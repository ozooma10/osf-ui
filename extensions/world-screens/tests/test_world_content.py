"""Engine-free regression for removal of vanilla display overlays."""
import struct
import sys
import unittest
import tempfile
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from world_content import Nif, author_screen, geometry_path, build_screen_models


def model():
    n = Nif.__new__(Nif)
    n.types = ['NiNode', 'BSGeometry', 'bhkPhysicsSystem']
    n.indices = (0, 1, 1, 2)
    n.strings = [b'root', b'ScreenMain', b'ScreenOverlay']
    n.prefix = (b'Gamebryo File Format, Version 20.2.0.7\n' +
                struct.pack('<IBIII', 0x14020007, 1, 12, 4, 170) +
                b'\0' + bytes(4) + b'\0\0' + struct.pack('<H', 3) +
                b''.join(struct.pack('<I', len(t)) + t.encode() for t in n.types) +
                struct.pack('<4H', *n.indices))
    n.groups, n.tail = bytes(4), b'footer'
    def av(name):
        return (struct.pack('<IIiI', name, 0, -1, 14) +
                struct.pack('<13fi', 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, -1))
    def geometry(name):
        path = b'original'
        return av(name) + bytes(52) + b'\1' + struct.pack('<4I', 6, 4, 0, len(path)) + path + bytes(3)
    n.blocks = [bytearray(av(0) + struct.pack('<3I', 2, 1, 2)),
                bytearray(geometry(1)), bytearray(geometry(2)), bytearray(b'physics')]
    return n.encode()


def quad(position=0):
    return (struct.pack('<II6HfII', 2, 6, 0, 1, 2, 0, 2, 3, 1, 0, 4) +
            struct.pack('<h', position) + bytes(22) + struct.pack('<I8e', 4, 0, 0, 1, 0, 1, 1, 0, 1))


class ScreenAuthoring(unittest.TestCase):
    def test_qualified_mod_id_does_not_put_dots_in_external_mesh_stem(self):
        with tempfile.TemporaryDirectory() as folder, patch('world_content.archive_files', side_effect=[
                {'meshes/setdressing/displayscreens/original.nif': model()},
                {'geometries/original.mesh': quad()}]):
            result = build_screen_models('.', folder, 'x2357.ssse', [('monitor', 'original.nif', 'ScreenMain', None)])
            self.assertEqual(result[0]['mesh'], 'geometries/osfui/screens/x2357_2essse/monitor.mesh')
            self.assertEqual(result[0]['path'], 'meshes/osfui/screens/x2357.ssse/monitor.nif')
            self.assertTrue((Path(folder) / result[0]['mesh']).is_file())

    def test_dotted_underscored_and_escape_like_owners_keep_independent_meshes(self):
        owners = ('example.mod', 'example_mod', 'example_2emod')
        with tempfile.TemporaryDirectory() as folder:
            generated = []
            for position, owner in enumerate(owners):
                with patch('world_content.archive_files', side_effect=[
                        {'meshes/setdressing/displayscreens/original.nif': model()},
                        {'geometries/original.mesh': quad(position)}]):
                    generated.extend(build_screen_models('.', folder, owner,
                        [('monitor', 'original.nif', 'ScreenMain', None)]))
            self.assertEqual(len({entry['mesh'] for entry in generated}), len(owners))
            for position, entry in enumerate(generated):
                nif = Nif((Path(folder) / entry['path']).read_bytes())
                self.assertEqual(geometry_path(nif, 'OSFUIScreen'), entry['mesh'])
                self.assertNotIn('.', Path(entry['mesh']).parent.name)
                # The later package did not overwrite this owner's geometry.
                self.assertEqual(struct.unpack_from('<h', (Path(folder) / entry['mesh']).read_bytes(), 32)[0], position)

    def test_overlay_detached_and_physics_preserved(self):
        before = Nif(model())
        path = 'geometries/osfui/screens/test.mesh'
        after = Nif(author_screen(model(), 'ScreenMain', path))
        self.assertEqual(struct.unpack_from('<2I', after.blocks[0], 72), (1, 1))
        self.assertEqual(after.blocks[3], before.blocks[3])
        self.assertEqual(after.blocks[0][:72], before.blocks[0][:72])
        self.assertEqual(after.blocks[1][16:72], before.blocks[1][16:72])
        self.assertEqual(geometry_path(after, 'OSFUIScreen'), path)
        self.assertEqual(after.tail, before.tail)
        self.assertEqual(struct.unpack_from('<I', after.blocks[2], 12)[0] & 1, 1)


if __name__ == '__main__':
    unittest.main()
