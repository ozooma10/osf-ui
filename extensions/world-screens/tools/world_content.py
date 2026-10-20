"""Record and mesh authoring helpers for the world-screen consumer builds.

Only writes caller-selected outputs. Vanilla archives and records are read-only.
Layouts follow xEdit's wbDefinitionsSF1 and NifSkope's Starfield nif.xml.
"""
from collections import OrderedDict
import hashlib
import mmap
from pathlib import Path
import struct
import zlib


def sub(tag, value):
    if len(value) > 65535:
        return b"XXXX\x04\x00" + struct.pack("<I", len(value)) + tag + b"\0\0" + value
    return struct.pack("<4sH", tag, len(value)) + value


def subs(body):
    p = 0
    while p < len(body):
        tag, size = struct.unpack_from("<4sH", body, p)
        p += 6
        if tag == b"XXXX":
            if size != 4:
                raise ValueError("Invalid extended subrecord")
            size = struct.unpack_from("<I", body, p)[0]
            tag = body[p + 4:p + 8]
            p += 10
        if p + size > len(body):
            raise ValueError("Invalid subrecord size")
        yield tag, body[p:p + size]
        p += size


def records(data, start=0, end=None, path=(), groups=None):
    p, end = start, len(data) if end is None else end
    while p < end:
        header = bytes(data[p:p + 24])
        tag, size = struct.unpack_from("<4sI", header)
        if tag == b"GRUP":
            if size < 24 or p + size > end:
                raise ValueError("Invalid group size")
            if path or groups is None or header[8:12] in groups:
                yield from records(data, p + 24, p + size, (*path, header), groups)
            p += size
        else:
            if p + 24 + size > end:
                raise ValueError("Invalid record size")
            body = bytes(data[p + 24:p + 24 + size])
            if struct.unpack_from("<I", header, 8)[0] & 0x40000:
                expected = struct.unpack_from("<I", body)[0]
                body = zlib.decompress(body[4:])
                if len(body) != expected:
                    raise ValueError("Invalid compressed record")
            yield header, list(subs(body)), path
            p += 24 + size


def field(fields, tag):
    values = [v for t, v in fields if t == tag]
    if len(values) != 1:
        raise ValueError(f"Expected one {tag!r}")
    return values[0]


def fid(header):
    return struct.unpack_from("<I", header, 12)[0]


def reference_override_index(placements, own_slot):
    """TES4 ONAM: overridden temporary refs, as in xEdit's TwbFile.PrepareSave.

    ESM reference overrides need this index for on-demand loading. New records
    and persistent references must not be included.
    """
    ids = []
    for header, _, _ in placements:
        if header[:4] != b"REFR":
            raise ValueError("Expected a placed reference")
        form = fid(header)
        if form >> 24 >= own_slot:
            raise ValueError("Placement is not an override from a master")
        if not struct.unpack_from("<I", header, 8)[0] & 0x400:
            ids.append(form)
    if len(ids) != len(set(ids)):
        raise ValueError("Duplicate reference override")
    return struct.pack(f"<{len(ids)}I", *sorted(ids))


def encode_record(header, fields):
    body = b"".join(sub(t, v) for t, v in fields)
    header = bytearray(header)
    struct.pack_into("<I", header, 4, len(body))
    struct.pack_into("<I", header, 8, struct.unpack_from("<I", header, 8)[0] & ~0x40000)
    return bytes(header) + body


def collect(master, ids=(), bases=(), editor_ids=(), groups=None):
    found = {}
    with Path(master).open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
        for h, fs, path in records(data, groups=groups):
            edid = next((v.rstrip(b"\0").decode() for t, v in fs if t == b"EDID"), "")
            base = next((struct.unpack("<I", v)[0] for t, v in fs if t == b"NAME" and len(v) == 4), 0)
            if fid(h) in ids or edid in editor_ids or (h[:4] == b"REFR" and base in bases):
                found[fid(h)] = (h, fs, path)
    return found


def placement_parents(master, placements):
    """Include the parent records xEdit creates when copying placed references."""
    ids = {struct.unpack_from('<I', group, 8)[0]
           for _, _, path in placements for group in path
           if struct.unpack_from('<I', group, 12)[0] in (1, 6)}
    parents = collect(master, ids=ids, groups={b'WRLD', b'CELL'})
    if parents.keys() != ids:
        raise ValueError('Missing placement parent records')
    interior_count = 0
    for form, (header, fields, path) in parents.items():
        flags = struct.unpack_from('<I', header, 8)[0]
        if header[:4] == b'CELL':
            interior = bool(struct.unpack('<I', field(fields, b'DATA'))[0] & 1)
            interior_count += interior
            # xEdit cannot make nonpersistent exterior grid cells partial.
            partial = interior or bool(flags & 0x400)
        elif header[:4] == b'WRLD':
            partial = True
        else:
            raise ValueError('Unexpected placement parent type')
        if partial:
            # Match TwbMainRecord.MakePartialForm: EDID only, no source offsets
            # or localized strings. The master retains the actual parent data.
            header = bytearray(header)
            struct.pack_into('<I', header, 8, (flags | 0x4000) & ~0x40020)
            fields = [(tag, value) for tag, value in fields if tag == b'EDID']
        elif any(tag == b'FULL' for tag, _ in fields):
            raise ValueError('Exterior cell localized name needs conversion before copying')
        parents[form] = (bytes(header), fields, path)
    return parents, interior_count


def grouped(records_with_paths, parents):
    """Write each WRLD/CELL parent immediately before its child group."""
    tree = OrderedDict()
    count = 0
    for header, fs, path in records_with_paths:
        branch = tree
        for depth, group in enumerate(path):
            # Ignore source byte size when identifying a group.
            key = group[8:16]
            if key not in branch:
                kind = struct.unpack_from('<I', group, 12)[0]
                if kind in (1, 6):
                    form = struct.unpack_from('<I', group, 8)[0]
                    parent, fields, parent_path = parents[form]
                    if (parent[:4] != (b'WRLD' if kind == 1 else b'CELL') or
                            [g[8:16] for g in parent_path] != [g[8:16] for g in path[:depth]]):
                        raise ValueError('Placement parent does not match its child group')
                    branch[(parent[:4], form)] = encode_record(parent, fields)
                branch[key] = (group, OrderedDict())
            branch = branch[key][1]
        branch[(header[:4], fid(header))] = encode_record(header, fs)

    def write(branch):
        nonlocal count
        out = bytearray()
        for value in branch.values():
            count += 1
            if isinstance(value, bytes):
                out += value
            else:
                h, children = value
                h = bytearray(h)
                body = write(children)
                struct.pack_into("<I", h, 4, 24 + len(body))
                out += h + body
        return bytes(out)
    result = write(tree)
    return result, count


def archive_files(data_dir, archive_names, wanted):
    """Later archives override earlier ones, matching Starfield's patch assets."""
    wanted = {x.lower().replace("\\", "/") for x in wanted}
    result = {}
    for name in archive_names:
        with (Path(data_dir) / name).open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            magic, version, kind, count, p = struct.unpack_from("<4sI4sIQ", data)
            if magic != b"BTDX" or kind != b"GNRL":
                raise ValueError("Expected general BA2 archive")
            header = {1:24, 2:32, 3:36}[version]
            for i in range(count):
                length = struct.unpack_from("<H", data, p)[0]
                path = data[p + 2:p + 2 + length].decode().lower().replace("\\", "/")
                p += 2 + length
                if path not in wanted:
                    continue
                *_, offset, packed, size, _ = struct.unpack_from("<I4sIIQIII", data, header + i * 36)
                blob = bytes(data[offset:offset + (packed or size)])
                if packed:
                    blob = zlib.decompress(blob)
                if len(blob) != size:
                    raise ValueError(f"Invalid BA2 entry {path}")
                result[path] = blob
    missing = wanted - result.keys()
    if missing:
        raise ValueError(f"Missing archive assets: {sorted(missing)}")
    return result


class Nif:
    """Lossless block editor for 20.2.0.7 / BS stream 170+ external geometry."""
    def __init__(self, data):
        p = data.index(b"\n") + 1
        version, endian, user, count, bs = struct.unpack_from("<IBIII", data, p)
        if (version, endian, user) != (0x14020007, 1, 12) or bs < 170:
            raise ValueError("Unsupported NIF header")
        p += 17
        p += 1 + data[p]  # author
        p += 4  # unknown int
        p += 1 + data[p]  # export script
        p += 1 + data[p]  # export data
        num_types = struct.unpack_from("<H", data, p)[0]
        p += 2
        self.types = []
        for _ in range(num_types):
            n = struct.unpack_from("<I", data, p)[0]
            p += 4
            self.types.append(data[p:p+n].decode())
            p += n
        self.indices = struct.unpack_from(f"<{count}H", data, p)
        p += count * 2
        self.prefix = data[:p]
        sizes = struct.unpack_from(f"<{count}I", data, p)
        p += count * 4
        nstrings, _ = struct.unpack_from("<II", data, p)
        p += 8
        self.strings = []
        for _ in range(nstrings):
            n = struct.unpack_from("<I", data, p)[0]
            p += 4
            self.strings.append(data[p:p+n])
            p += n
        ngroups = struct.unpack_from("<I", data, p)[0]
        self.groups = data[p:p+4+ngroups*4]
        p += len(self.groups)
        self.blocks = []
        for size in sizes:
            self.blocks.append(bytearray(data[p:p+size]))
            p += size
        self.tail = data[p:]

    def geometries(self):
        for i, (kind, block) in enumerate(zip(self.indices, self.blocks)):
            if self.types[kind] != "BSGeometry":
                continue
            name, extra = struct.unpack_from("<II", block)
            flags = 12 + 4 * extra
            # Flags, transform and collision reference complete NiAVObject.
            av_end = flags + 60
            yield i, self.strings[name].decode(), flags, av_end

    def encode(self):
        return (self.prefix + struct.pack(f"<{len(self.blocks)}I", *(len(b) for b in self.blocks)) +
                struct.pack("<II", len(self.strings), max(map(len, self.strings))) +
                b"".join(struct.pack("<I", len(s)) + s for s in self.strings) +
                self.groups + b"".join(self.blocks) + self.tail)


def geometry_path(nif, name):
    for i, node, _, av_end in nif.geometries():
        if node == name:
            b = nif.blocks[i]
            p = av_end + 52
            if b[p] != 1:
                raise ValueError("Screen has no first mesh")
            count, verts, flags, size = struct.unpack_from("<IIII", b, p + 1)
            path = b[p+17:p+17+size].decode().replace("\\", "/")
            return "geometries/" + path + ".mesh"
    raise ValueError(f"Missing screen node {name}")


def screen_mesh(blob):
    """Assign the original screen quad its full image UVs."""
    out = bytearray(blob)
    _, nindices = struct.unpack_from("<II", out)
    p = 8 + nindices * 2
    _, weights, nverts = struct.unpack_from("<fII", out, p)
    if nverts != 4 or nindices != 6 or weights:
        raise ValueError("Expected a static screen quad")
    p += 12
    uv_at = p + 24
    if struct.unpack_from("<I", out, uv_at)[0] != 4:
        raise ValueError("Expected four screen UVs")
    uv_at += 4
    uv = [struct.unpack_from("<2e", out, uv_at+i*4) for i in range(4)]
    lo = [min(v[a] for v in uv) for a in range(2)]
    hi = [max(v[a] for v in uv) for a in range(2)]
    if any(hi[a] == lo[a] for a in range(2)):
        raise ValueError("Degenerate screen UVs")
    # Preserve winding, normal, tangent and the model's front/back orientation.
    # The page occupies the glass; the bezel's controls remain untouched.
    for i in range(4):
        struct.pack_into("<2e", out, uv_at+i*4, *((uv[i][a]-lo[a])/(hi[a]-lo[a]) for a in range(2)))
    return bytes(out)


def author_screen(nif_data, main_node, mesh_path, template=None, template_node=None):
    """Normalize only the display face; retain the original body and collision."""
    nif = Nif(nif_data)
    original_physics = [bytes(b) for t, b in zip(nif.indices, nif.blocks) if nif.types[t].startswith("bhk")]
    target = next(g for g in nif.geometries() if g[1] == main_node)
    index, _, flags_at, av_end = target
    block = nif.blocks[index]
    if template is not None:
        source = Nif(template)
        si, _, sf, se = next(g for g in source.geometries() if g[1] == template_node)
        # Transform and bounds only: never copy another NIF's block references.
        block[flags_at+4:av_end-4] = source.blocks[si][sf+4:se-4]
        block[av_end:av_end+40] = source.blocks[si][se:se+40]
    name = struct.unpack_from("<I", block)[0]
    nif.strings[name] = b"OSFUIScreen"
    struct.pack_into("<I", block, flags_at, struct.unpack_from("<I", block, flags_at)[0] & ~1)
    path = mesh_path.removeprefix("geometries/").removesuffix(".mesh").replace("/", "\\").encode()
    nif.blocks[index] = block[:av_end+52] + b"\x01" + struct.pack("<IIII", 6, 4, 0, len(path)) + path + b"\0\0\0"
    # Starfield still draws some BSGeometry overlays with the NiAV hidden bit set.
    # Detach those child references as well, keeping block IDs and physics intact.
    removed = set()
    for i, node, flags, _ in list(nif.geometries()):
        if i != index and (node.lower().startswith("screen") or node == "EditorMarker:0"):
            struct.pack_into("<I", nif.blocks[i], flags, struct.unpack_from("<I", nif.blocks[i], flags)[0] | 1)
            removed.add(i)
    for i, kind in enumerate(nif.indices):
        if nif.types[kind] != "NiNode":
            continue
        block = nif.blocks[i]
        extra = struct.unpack_from("<I", block, 4)[0]
        children_at = 12 + 4 * extra + 60
        count = struct.unpack_from("<I", block, children_at)[0]
        children_end = children_at + 4 + count * 4
        if children_end > len(block):
            raise ValueError("Invalid screen model child array")
        children = struct.unpack_from(f"<{count}i", block, children_at + 4)
        kept = [child for child in children if child not in removed]
        nif.blocks[i] = (block[:children_at] + struct.pack("<I", len(kept)) +
                         struct.pack(f"<{len(kept)}i", *kept) + block[children_end:])
    result = nif.encode()
    check = Nif(result)
    if [bytes(b) for t, b in zip(check.indices, check.blocks) if check.types[t].startswith("bhk")] != original_physics:
        raise ValueError("Screen authoring changed collision")
    if geometry_path(check, "OSFUIScreen") != mesh_path:
        raise ValueError("Screen mesh binding did not round-trip")
    return result


def build_screen_models(data_dir, output, owner, models):
    """models: (output name, vanilla NIF basename, screen node, optional quad source)."""
    archives = ["Starfield - Meshes01.ba2", "Starfield - Meshes02.ba2", "Starfield - MeshesPatch.ba2"]
    prefix = "meshes/setdressing/displayscreens/"
    names = {prefix + source for _, source, _, _ in models}
    names.update(prefix + template[0] for _, _, _, template in models if template)
    nifs = archive_files(data_dir, archives, names)
    mesh_paths = {}
    for name, source, node, template in models:
        quad_source, quad_node = template or (source, node)
        mesh_paths[name] = geometry_path(Nif(nifs[prefix + quad_source]), quad_node)
    meshes = archive_files(data_dir, archives, mesh_paths.values())
    evidence = []
    for name, source, node, template in models:
        # External mesh stems must not contain dots before the .mesh suffix.
        # Keep qualified mod IDs in NIF/catalog paths, but use a safe mesh folder.
        mesh_owner = owner.replace('.', '_')
        mesh_path = f"geometries/osfui/screens/{mesh_owner}/{name}.mesh"
        nif_path = f"meshes/osfui/screens/{owner}/{name}.nif"
        content = screen_mesh(meshes[mesh_paths[name]])
        authored = author_screen(nifs[prefix + source], node, mesh_path,
            nifs[prefix + template[0]] if template else None, template[1] if template else None)
        for path, data in ((mesh_path, content), (nif_path, authored)):
            dest = Path(output) / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        evidence.append({"path": nif_path, "source": prefix + source,
            "sourceSHA256": hashlib.sha256(nifs[prefix + source]).hexdigest(),
            "sha256": hashlib.sha256(authored).hexdigest(), "collisionPreserved": True,
            "geometry": "OSFUIScreen", "mesh": mesh_path})
    return evidence
