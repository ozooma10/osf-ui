"""Inspect the actual staged artifacts, not version strings or source exports.

Loads a candidate DLL in a disposable subprocess and calls only API acquisition
and the frozen bridge's version slot. Never calls SFSEPlugin_Load or game APIs.
The small PEX reader deliberately accepts only a native-only Starfield script.
Format reference: https://github.com/Orvid/Champollion/blob/main/Pex/FileReader.cpp
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys


def require(condition, message):
    if not condition:
        raise ValueError(message)


def probe_dll(path):
    dll = ctypes.WinDLL(str(path.resolve()))
    modern = dll.OSFUI_RequestAPI
    modern.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    modern.restype = ctypes.c_void_p
    actual = ctypes.c_uint32()
    pointer = modern(0x20000, ctypes.byref(actual))
    require(pointer and actual.value == 0x20000, f"modern API 2.0 missing: {actual.value:#x}")
    legacy = dll.OSFUI_RequestBridge
    legacy.argtypes = [ctypes.c_uint32]
    legacy.restype = ctypes.c_void_p
    bridge = legacy(0x10008)
    require(bridge, "legacy bridge 1.8 missing")
    for minor in range(9):
        require(legacy(0x10000 + minor) == bridge, f"legacy bridge 1.{minor} missing")
    require(not legacy(0x10009) and not legacy(0x20000), "incompatible legacy ABI accepted")
    vtable = ctypes.cast(bridge, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    version = ctypes.WINFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p)(vtable[0])(bridge)
    require(version == 0x10008, f"legacy vtable reports {version:#x}")
    print(json.dumps({"modern_api": "2.0", "legacy_bridge": "1.0-1.8", "legacy_vtable": "1.8"}))


class NativePex:
    def __init__(self, path):
        self.data = path.read_bytes()
        self.pos = 0

    def number(self, fmt):
        value = struct.unpack_from('<' + fmt, self.data, self.pos)[0]
        self.pos += struct.calcsize(fmt)
        return value

    def skip(self, count):
        self.pos += count
        require(self.pos <= len(self.data), 'truncated PEX')

    def text(self):
        size = self.number('H')
        start = self.pos
        self.skip(size)
        return self.data[start:self.pos].decode('utf-8')

    def name(self):
        return self.strings[self.number('H')].lower()

    def read(self):
        require(self.number('I') == 0xFA57C0DE, 'invalid PEX magic')
        require(self.number('B') == 3, 'unsupported PEX major')
        self.number('B')
        require(self.number('H') == 4, 'PEX is not Starfield')
        self.skip(8)
        self.text(); self.text(); self.text()
        self.strings = [self.text() for _ in range(self.number('H'))]
        if self.number('B'):
            self.skip(8)
            for _ in range(self.number('H')):
                self.skip(7)
                self.skip(2 * self.number('H'))
            for _ in range(self.number('H')):
                self.skip(10)
                self.skip(2 * self.number('H'))
            for _ in range(self.number('H')):
                self.skip(4)
                self.skip(2 * self.number('H'))
        self.skip(3 * self.number('H'))  # user flag definitions
        require(self.number('H') == 1, 'expected one OSFUI object')
        require(self.name() == 'osfui', 'wrong PEX script')
        self.skip(4)  # object size
        self.name(); self.name()  # parent, documentation
        self.skip(5)  # const flag + user flags
        self.name()  # auto state
        for table in ('structs', 'variables', 'guards', 'properties'):
            require(self.number('H') == 0, f'native-only OSFUI unexpectedly contains {table}')
        functions = {}
        for _ in range(self.number('H')):
            self.name()
            for _ in range(self.number('H')):
                name, result = self.name(), self.name()
                self.name(); self.skip(4)
                require(self.number('B') == 3, f'{name} is not Global Native')
                parameters = [(self.name(), self.name()) for _ in range(self.number('H'))]
                require(self.number('H') == 0 and self.number('H') == 0, f'{name} has a body')
                require(name not in functions, f'duplicate function {name}')
                functions[name] = (result, [kind for _, kind in parameters])
        require(self.pos == len(self.data), 'unparsed PEX bytes')
        return functions


def check_pex(pex, source):
    functions = NativePex(pex).read()
    declarations = {}
    pattern = r'^\s*(?:(\w+(?:\[\])?)\s+)?Function\s+(\w+)\(([^)]*)\)\s+Global\s+Native\s*$'
    for match in re.finditer(pattern, source.read_text(), re.I | re.M):
        result, name, args = match.groups()
        declarations[name.lower()] = ((result or 'none').lower(), [p.strip().split()[0].lower() for p in args.split(',') if p.strip()])
    require(functions == declarations, 'compiled PEX signatures differ from shipped source')
    # Freeze the legacy consumer contract independently of the source file under test.
    required = {
        'listenforviewactions': ('int', ['scriptobject', 'string']),
        'listenforviewrequests': ('int', ['scriptobject', 'string']),
        'registerforhotkey': ('int', ['scriptobject', 'string', 'string', 'string']),
        'unregister': ('bool', ['int']),
        'rejectviewrequest': ('bool', ['string', 'string', 'string']),
        'openmenu': ('bool', ['string']),
        'registersend': ('int', ['scriptobject', 'string', 'string']),
    }
    for suffix, kind in [('Bool', 'bool'), ('Int', 'int'), ('Float', 'float'), ('String', 'string'),
                         ('Bools', 'bool[]'), ('Ints', 'int[]'), ('Floats', 'float[]'), ('Strings', 'string[]'), ('Forms', 'form[]')]:
        required['setview' + suffix.lower()] = ('none', ['string', 'string', kind])
        required['replyview' + suffix.lower()] = ('bool', ['string', kind])
    for name, signature in required.items():
        require(functions.get(name) == signature, f'wrong/missing compiled signature: {name}')
    return len(functions)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('data', type=Path, nargs='?')
    parser.add_argument('--probe-dll', type=Path)
    args = parser.parse_args()
    if args.probe_dll:
        probe_dll(args.probe_dll)
        return
    require(args.data, 'staged Data root required')
    dll = args.data / 'SFSE/Plugins/OSFUI.dll'
    child = subprocess.run([sys.executable, __file__, '--probe-dll', str(dll)], capture_output=True, text=True, timeout=20)
    require(child.returncode == 0, f'DLL acquisition probe failed ({child.returncode}): {child.stdout} {child.stderr}')
    print(child.stdout.strip())
    count = check_pex(args.data / 'Scripts/OSFUI.pex', args.data / 'Scripts/Source/OSFUI.psc')
    print(f'{count} compiled Global Native signatures verified, including modern and legacy contracts')
    for path in [dll, args.data / 'Scripts/OSFUI.pex', args.data / 'SFSE/Plugins/OSF/UI/bin/osfui_webview2_host.exe']:
        print(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(args.data)}')


if __name__ == '__main__':
    main()
