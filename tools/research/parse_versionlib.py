"""Resolve AddressLib IDs -> offsets from a versionlib .bin (format V2).

Mirrors commonlib-shared REL::IDDB::unpack_file. Used 2026-06-12 to identify
which RE::VTABLE::UI index is the live BSInputEventReceiver vtable.
"""
import struct, sys

def parse(path, wanted):
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    def rd(fmt):
        nonlocal pos
        vals = struct.unpack_from(fmt, data, pos)
        pos += struct.calcsize(fmt)
        return vals if len(vals) > 1 else vals[0]

    fmt_ver = rd("<I")
    if fmt_ver == 5:
        # HEADER_V5: gameVersion u32[4], name char[64], ptrSize i32,
        # dataFormat i32, offsetCount i32; then offsets[id] as raw u32.
        game_ver = rd("<4I")
        name = data[pos:pos + 64].split(b"\0")[0].decode(); pos += 64
        ptr_size, data_format, count = rd("<3i")
        offsets = struct.unpack_from(f"<{count}I", data, pos)
        print(f"{path}\n  format 5, game {'.'.join(map(str, game_ver))} ({name}), {count} offsets")
        return {ident: offsets[ident] for ident in wanted if ident < count and offsets[ident]}
    assert fmt_ver in (1, 2), f"unsupported format {fmt_ver}"
    game_ver = rd("<4I")
    name_len = rd("<I")
    name = data[pos:pos + name_len].decode(); pos += name_len
    ptr_size = rd("<i")
    count = rd("<i")

    out = {}
    prev_id = prev_off = 0
    for _ in range(count):
        t = rd("<B")
        lo, hi = t & 0xF, t >> 4
        if   lo == 0: ident = rd("<Q")
        elif lo == 1: ident = prev_id + 1
        elif lo == 2: ident = prev_id + rd("<B")
        elif lo == 3: ident = prev_id - rd("<B")
        elif lo == 4: ident = prev_id + rd("<H")
        elif lo == 5: ident = prev_id - rd("<H")
        elif lo == 6: ident = rd("<H")
        elif lo == 7: ident = rd("<I")
        else: raise ValueError(lo)

        tmp = (prev_off // ptr_size) if (hi & 8) else prev_off
        h = hi & 7
        if   h == 0: off = rd("<Q")
        elif h == 1: off = tmp + 1
        elif h == 2: off = tmp + rd("<B")
        elif h == 3: off = tmp - rd("<B")
        elif h == 4: off = tmp + rd("<H")
        elif h == 5: off = tmp - rd("<H")
        elif h == 6: off = rd("<H")
        elif h == 7: off = rd("<I")
        else: raise ValueError(h)
        if hi & 8:
            off *= ptr_size

        if ident in wanted:
            out[ident] = off
        prev_id, prev_off = ident, off

    print(f"{path}\n  game {'.'.join(map(str, game_ver))} ({name}), {count} mappings")
    return out

# RE::VTABLE::UI from CommonLibSF IDs_VTABLE.h, in declared array order:
UI_VTABLE = [475429, 475441, 475431, 475445, 475443, 475433, 475447, 475437, 475435, 475449, 475439]
LIVE_RECEIVER_VPTR = 0x4D7E408  # observed in-game 2026-06-12, rebased

for path in sys.argv[1:]:
    res = parse(path, set(UI_VTABLE))
    for i, ident in enumerate(UI_VTABLE):
        off = res.get(ident)
        mark = "  <-- LIVE BSInputEventReceiver vptr" if off == LIVE_RECEIVER_VPTR else ""
        print(f"  VTABLE[{i:2}] ID {ident} -> {off:#x}{mark}" if off is not None
              else f"  VTABLE[{i:2}] ID {ident} -> MISSING")
