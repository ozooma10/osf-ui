"""Four independent material swaps on vanilla QASmoke screens; private test assets."""
import hashlib
import json
from pathlib import Path
import struct
import uuid


BOARDS = [
    ("NOVA", "Nova Galactic", "#48e0bf", 124),
    ("DEIM", "Deimos", "#ffbd55", 278),
    ("STRO", "Stroud-Eklund", "#69b9ff", 196),
    ("RYUJ", "Ryujin Industries", "#f887b9", 342),
]
NAMESPACE = uuid.UUID("5f02d13f-7855-483c-b98a-98a11a1eb9fa")


def sub(tag, value):
    return struct.pack("<4sH", tag, len(value)) + value


def subs(body):
    pos = 0
    while pos < len(body):
        tag, size = struct.unpack_from("<4sH", body, pos)
        assert tag != b"XXXX" and pos + 6 + size <= len(body)
        yield tag, body[pos+6:pos+6+size]
        pos += 6 + size
    assert pos == len(body)


def material_path(index):
    return f"OSFUIWorldTest\\board{index}.mat"


def replace_refl_path(body, path):
    # REFL is a sequence of tagged, length-prefixed blocks. Change only the
    # proven fixture's swap LIST, preserving its type descriptors and key.
    old = b"Architecture\\City\\NewAtlantis\\Lodge\\BaseMaterials\\NA_Lodge_Space01.mat\0"
    new = path.encode() + b"\0"
    output = bytearray(body[:16])
    pos = 16
    replaced = 0
    while pos < len(body):
        tag, size = struct.unpack_from("<4sI", body, pos)
        value = body[pos+8:pos+8+size]
        if old in value:
            assert tag == b"LIST" and value.count(old) == 1
            value = value.replace(struct.pack("<H", len(old)) + old,
                                  struct.pack("<H", len(new)) + new)
            replaced += 1
        output.extend(struct.pack("<4sI", tag, len(value)) + value)
        pos += 8 + size
    assert pos == len(body) and replaced == 1
    return bytes(output)


def fixture(template, count):
    def record(header, body, form=None):
        header = bytearray(header)
        struct.pack_into("<I", header, 4, len(body))
        if form is not None:
            struct.pack_into("<I", header, 12, form)
        return header + body

    def rewrite(data):
        output = bytearray()
        pos = 0
        while pos < len(data):
            tag, size = struct.unpack_from("<4sI", data, pos)
            header = data[pos:pos+24]
            if tag == b"GRUP":
                body = rewrite(data[pos+24:pos+size])
                new_header = bytearray(header)
                struct.pack_into("<I", new_header, 4, 24 + len(body))
                output.extend(new_header + body)
                pos += size
                continue
            body = data[pos+24:pos+24+size]
            form = struct.unpack_from("<I", header, 12)[0]
            if tag == b"REFR":
                for index in range(count):
                    # Two columns, two rows; the vanilla mesh faces south.
                    x, y, z = -3.72 + (index % 2) * 2.2, 5.45, .592 + (index // 2) * 1.3
                    value = (sub(b"NAME", struct.pack("<I", 0xC499F)) +
                             sub(b"XLMS", struct.pack("<I", 0x01000880 + index)) +
                             sub(b"DATA", struct.pack("<6f", x, y, z, 0, 0, 0)))
                    output.extend(record(header, value, 0x01000900 + index))
            elif tag == b"LMSW":
                if form == 0x01000816:
                    for index in range(count):
                        value = b"".join(sub(t, f"OSFUIWorldBoard{index}\0".encode() if t == b"EDID"
                                              else replace_refl_path(v, material_path(index)) if t == b"REFL" else v)
                                         for t, v in subs(body))
                        output.extend(record(header, value, 0x01000880 + index))
            elif tag == b"TES4":
                body = b"".join(sub(t, struct.pack("<fII", .96, 7 + 2 * count, 0x900 + count) if t == b"HEDR"
                                    else b"OSF UI independent world boards\0" if t == b"CNAM" else v)
                                for t, v in subs(body))
                output.extend(record(header, body))
            else:
                output.extend(header + body)
            pos += 24 + size
        assert pos == len(data)
        return bytes(output)
    return rewrite(template)


def dds(path, size, color):
    header = struct.pack("<4sIIIIIII44xIIIIIIIIIIII4x", b"DDS ", 124, 0x100F,
                         size, size, size * 4, 0, 0, 32, 0x41, 0, 32,
                         0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000, 0x1000, 0, 0, 0)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + bytes((*reversed(color), 255)) * size * size)


def material(index):
    def resource(kind):
        h = uuid.uuid5(NAMESPACE, f"{index}/{kind}").hex
        return f"res:{int(h[:8], 16):08X}:{(int(h[8:16], 16) & 0x3FFFF) | 0x40000:08X}:{(int(h[16:24], 16) & 0x7FFFFFF) | 0xA0000000:08X}"
    def component(kind, data, slot=0):
        return {"Type": "BSMaterial::" + kind, "Index": slot, "Data": data}
    def obj(kind, components):
        return {"ID": resource(kind), "Parent": f"materials\\layered\\root\\{kind}.mat", "Components": components}
    texture = feed_texture(index).replace("/", "\\")
    slots = {0: texture, 1: "textures\\OSFUIWorldTest\\normal.dds", 3: "textures\\OSFUIWorldTest\\rough.dds",
             4: "textures\\OSFUIWorldTest\\black.dds", 5: "textures\\OSFUIWorldTest\\white.dds", 7: texture}
    return {"Version": 1, "Objects": [
        {"Parent": "materials\\layered\\root\\layeredmaterials.mat", "Components": [
            component("LayerID", {"ID": resource("layers")}),
            component("ShaderModelComponent", {"FileName": "ColorEmissive"}),
            component("EmissiveSettingsComponent", {"Enabled": "true", "Settings": {
                "Type": "BSMaterial::EmittanceSettings", "Data": {
                    "LuminousEmittance": "300.0", "AdaptiveEmittance": "true", "ExposureOffset": "5.5",
                    "EmissiveClipThreshold": "0.0", "EmissiveTint": {"Type": "BSMaterial::Color", "Data": {
                        "Value": {"Type": "XMFLOAT4", "Data": dict.fromkeys("wxyz", "1.0")}}}}}})]},
        obj("layers", [component("MaterialID", {"ID": resource("materials")}), component("UVStreamID", {"ID": resource("uvstreams")})]),
        obj("uvstreams", []), obj("materials", [component("TextureSetID", {"ID": resource("texturesets")})]),
        obj("texturesets", [component("MRTextureFile", {"FileName": path}, slot) for slot, path in slots.items()]),
    ]}


def feed_id(index):
    mod = "osfui-world-test" if index < 2 else "z-world-test"
    view = "screen" if index % 2 == 0 else "screen-2"
    return f"{mod}/{view}"


def feed_texture(index):
    digest = hashlib.sha256(feed_id(index).encode()).hexdigest()
    return f"textures/osfui/feeds/{digest[:32]}/{digest[32:]}.dds"


def stage(out, template, count, plugin_name):
    out.mkdir(parents=True, exist_ok=True)
    (out / plugin_name).write_bytes(fixture(template, count))
    requests = out / "SFSE/Plugins/OSF/UI/views/osfui-world-test"
    requests.mkdir(parents=True, exist_ok=True)
    (requests / "evict.request").write_text("")

    for name, color in [("normal", (128, 128, 255)), ("rough", (190, 190, 190)),
                        ("black", (0, 0, 0)), ("white", (255, 255, 255))]:
        dds(out / f"textures/OSFUIWorldTest/{name}.dds", 4, color)
    page = (Path(__file__).resolve().parents[2] / "examples/world-stock-board/index.html").read_text(encoding="utf-8")
    for index, (symbol, title, color, price) in enumerate(BOARDS[:count]):
        dds(out / feed_texture(index), 64, (0, 0, 0))
        path = out / "materials" / material_path(index).replace("\\", "/")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(material(index), indent=2) + "\n")
        view = out / "SFSE/Plugins/OSF/UI/views" / feed_id(index)
        view.mkdir(parents=True, exist_ok=True)
        (view / "manifest.json").write_text(json.dumps({"manifestVersion": 1, "title": title,
            "kind": "world", "entry": "index.html", "width": 1000, "height": 1000, "texture": feed_texture(index)}, indent=2) + "\n")
        (view / "board.json").write_text(json.dumps({"symbol": symbol, "title": title, "color": color,
            "price": price, "interval": 700 + index * 350, "demo": False, "fixture": True}) + "\n")
        (view / "index.html").write_text(page, encoding="utf-8")
