"""Write (and read back) alia-engine resource packs, .apak (docs/formats.md). Pure Python, no dependencies:
a game's build scripts import it on the PC to bake their assets.

    import sys; sys.path.insert(0, "<alia-engine>/tools")
    from alia_pack import PackWriter
    w = PackWriter()
    w.add_mesh("rock", positions, indices, tri_palette=colors)   # default material: flat, palette colours
    w.add_mesh("ground", positions, indices, vertex_rgb=rgb)     # default material: smooth, RGB per vertex
    w.add_material("glow", shading="unlit", palette_index=9, emissive=1.0, fog=False)
    w.add_mesh("rune", positions, indices, tri_palette=colors, submeshes=[(0, 10, ""), (10, 4, "glow")])
    w.add_texture("crate", "art/crate.png")                      # a PNG or JPEG (path or bytes), stored as it is
    w.add_material("crate", color="texture", texture="crate")    # materials name textures of the same pack
    w.write("gen/meshes.apak")                                   # or w.bytes()

The game loads it with alia_engine: `assets = engine.load_assets("gen/meshes.apak")` (.meshes, .materials,
.textures, .rigs by name), or `meshes, materials = engine.load_pack(...)`. Material fields: docs/api.md §2;
textures: docs/textures.md.
"""

import struct

MAGIC = b"APAK"
VERSION = 1
MESH = 1
MATERIAL = 2
RIG = 3
TEXTURE = 4
_HEADER = struct.Struct("<4sHHII")   # magic, version, reserved, entry count, directory offset
_DIR = struct.Struct("<BBHII")       # type, flags, name length, offset, size
_MESH = struct.Struct("<IIBBH")      # vertices, triangles, colours (bit0 palette, bit1 rgb), flags (bit0 double-sided, bit1 parts)
_MATERIAL = struct.Struct("<BBBBfffBBHf")
_TEXTURE = struct.Struct("<HHBBHI")  # width, height, format (0: PNG / JPEG file), flags (linear, repeat, mipmaps), 0, size
SHADING = {"flat": 0, "smooth": 1, "unlit": 2}
COLOR = {"palette": 0, "vertex": 1, "texture": 2}
LIGHT = {"sun": 0, "character": 1}
ALPHA = {"opaque": 0, "blend": 1, "cutout": 2}


class PackError(ValueError):
    pass


def image_size(data):
    """(width, height) of PNG or JPEG bytes, from the header; None when it is neither."""
    if data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR":
        return struct.unpack(">II", data[16:24])
    if data[:2] == b"\xff\xd8":
        at = 2
        while at + 9 < len(data):
            if data[at] != 0xFF:
                return None
            marker, length = data[at + 1], struct.unpack(">H", data[at + 2:at + 4])[0]
            if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):  # SOFn
                h, w = struct.unpack(">HH", data[at + 5:at + 9])
                return w, h
            at += 2 + length
    return None


def _pad4(data):
    return data + bytes(-len(data) % 4)


class PackWriter:
    def __init__(self):
        self.entries = []  # (type, name, payload)

    def add_texture(self, name, image, filter="nearest", wrap="clamp", mipmaps=False):
        """A texture (docs/textures.md §4): `image` is a PNG or JPEG, a path or its bytes, stored as it is and
        decoded when the pack is loaded (at most 2048 on the longer side, bigger is scaled down). filter
        'nearest' or 'linear', wrap 'clamp' or 'repeat', mipmaps as e.load_texture. Materials of the same pack
        name it (add_material(texture=name))."""
        if isinstance(image, str):
            with open(image, "rb") as f:
                image = f.read()
        image = bytes(image)
        size = image_size(image)
        if not size or not all(0 < v < 65536 for v in size):
            raise PackError(f"{name}: not a PNG or JPEG")
        if filter not in ("nearest", "linear") or wrap not in ("clamp", "repeat"):
            raise PackError(f"{name}: filter 'nearest' or 'linear', wrap 'clamp' or 'repeat'")
        if any(n == name and t == TEXTURE for t, n, _ in self.entries):
            raise PackError(f"{name}: duplicate texture")
        flags = (1 if filter == "linear" else 0) | (2 if wrap == "repeat" else 0) | (4 if mipmaps else 0)
        self.entries.append((TEXTURE, name, _TEXTURE.pack(*size, 0, flags, 0, len(image)) + image))

    def add_material(self, name, shading="flat", color="palette", palette_index=0, light="sun", ambient=-1.0,
                     diffuse=-1.0, emissive=0.0, alpha_mode="opaque", opacity=1.0, double_sided=False, fog=True,
                     ground_fog=True, texture="", uv_rect=(0.0, 0.0, 1.0, 1.0)):
        """A material (docs/api.md §2); meshes in the same pack name it in their submeshes. color='texture':
        `texture` names a texture of the same pack (add_texture), uv_rect the part of it the mesh's uv cover."""
        try:
            fields = (SHADING[shading], COLOR[color], int(palette_index), LIGHT[light])
        except KeyError as error:
            raise PackError(f"{name}: unknown value {error}") from None
        if not 0 <= palette_index < 32 or not 0 <= emissive <= 1 or not 0 <= opacity <= 1 or alpha_mode not in ALPHA:
            raise PackError(f"{name}: palette_index 0..31, emissive and opacity 0..1, alpha_mode opaque, blend or cutout")
        if texture and color != "texture":
            raise PackError(f"{name}: texture= needs color='texture'")
        if len(uv_rect) != 4:
            raise PackError(f"{name}: uv_rect is (u0, v0, u1, v1)")
        if any(n == name and t == MATERIAL for t, n, _ in self.entries):
            raise PackError(f"{name}: duplicate material")
        flags = (1 if double_sided else 0) | (2 if fog else 0) | (4 if ground_fog else 0)
        raw = texture.encode()
        # the texture name and uv_rect come after the first version's fields: older engines stop before them
        self.entries.append((MATERIAL, name, _MATERIAL.pack(*fields, ambient, diffuse, emissive, ALPHA[alpha_mode],
                                                            flags, 0, opacity)
                             + struct.pack("<H", len(raw)) + raw + struct.pack("<4f", *uv_rect)))

    def add_mesh(self, name, positions, indices, tri_palette=None, vertex_rgb=None, double_sided=False,
                 submeshes=None, uv=None):
        """positions: flat xyz floats; indices: flat triangle indices (counter-clockwise front faces);
        the colours its materials use: tri_palette (a palette index per triangle) and/or vertex_rgb
        (3 bytes per vertex) and/or uv (flat u, v floats per vertex, for textured materials; 0, 0 is the
        image's top-left corner); submeshes: [(first_tri, tri_count, material name)], "" for the default
        material. Without submeshes the default material needs tri_palette or vertex_rgb."""
        positions, indices = list(positions), list(indices)
        nv, nt = len(positions) // 3, len(indices) // 3
        if len(positions) % 3 or len(indices) % 3 or not nv or not nt:
            raise PackError(f"{name}: positions must be xyz triples and indices whole triangles")
        if nv > 65535 or max(indices) >= nv or min(indices) < 0:
            raise PackError(f"{name}: index out of range (or more than 65535 vertices)")
        mode, colors = 0, b""
        if tri_palette is not None:
            mode, colors = mode | 1, colors + bytes(tri_palette)
            if len(tri_palette) != nt:
                raise PackError(f"{name}: tri_palette needs one entry per triangle")
        if vertex_rgb is not None:
            mode, colors = mode | 2, colors + bytes(vertex_rgb)
            if len(vertex_rgb) != 3 * nv:
                raise PackError(f"{name}: vertex_rgb needs one RGB per vertex")
        if not mode and not submeshes:
            raise PackError(f"{name}: give tri_palette or vertex_rgb (or submeshes with materials)")
        if any(n == name and t == MESH for t, n, _ in self.entries):
            raise PackError(f"{name}: duplicate mesh")
        parts = b""
        if submeshes:
            parts = struct.pack("<I", len(submeshes))
            for first, count, material in submeshes:
                if count <= 0 or first < 0 or first + count > nt:
                    raise PackError(f"{name}: submesh outside the triangles")
                raw = material.encode()
                parts += struct.pack("<IIH", first, count, len(raw)) + raw
        uvs = b""
        if uv is not None:
            uv = list(uv)
            if len(uv) != 2 * nv:
                raise PackError(f"{name}: uv needs one u, v per vertex")
            uvs = struct.pack(f"<{2 * nv}f", *uv)
        flags = (1 if double_sided else 0) | (2 if submeshes else 0) | (4 if uv is not None else 0)
        payload = (_MESH.pack(nv, nt, mode, flags, 0) + struct.pack(f"<{3 * nv}f", *positions)
                   + struct.pack(f"<{3 * nt}H", *indices) + colors + parts + uvs)
        self.entries.append((MESH, name, payload))

    def add_rig(self, name, rig):
        """A rigid rig (docs/formats.md type 3), in the shape of Megalith's assets/rigs/*.json:
        {"bones": [{"name", "parent" (a bone name or None, parents first), "offset": [x, y, z],
                    "mesh" (a mesh name or None), "colliders": [collider, ...]}],
         "slots": [{"bone", "offset", "rot" (degrees), "variants": [None, mesh name, ...], "collider": collider}],
         "attach": {name: {"bone", "offset"}},
         "anims": [{"name", "loop", "keys", "tracks": {bone: [[rx, ry, rz] per key]} (degrees),
                    "root_y": [per key] or None, "events": [{"phase", "type", other keys: str or number}]}]}
        collider: {"type": "sphere", "c": [x, y, z], "r"} or {"type": "capsule", "a", "b", "r"}, in the bone's
        (or slot's) frame, with optional "layers" (bit mask, default 1), "user" (an int for the game) and
        "priority" (wins ties between hits at the same distance). Other keys (hp, tags, ids, hookable ...) are the
        game's and are left out; turn the ones that matter into layers before calling this."""
        def s16(text):
            raw = (text or "").encode()
            return struct.pack("<H", len(raw)) + raw

        def vec3(v, what):
            if len(v) != 3:
                raise PackError(f"{name}: {what} needs 3 numbers")
            return struct.pack("<3f", *v)

        def collider(c, where):
            if not c:
                return struct.pack("<B", 0) + bytes(37)  # the same 38 bytes, shape 0
            if c.get("type") == "sphere":
                shape, a, b = 1, c["c"], c["c"]
            elif c.get("type") == "capsule":
                shape, a, b = 2, c["a"], c["b"]
            else:
                raise PackError(f"{name}: {where}: collider type must be sphere or capsule")
            return (struct.pack("<B", shape) + vec3(a, where) + vec3(b, where)
                    + struct.pack("<fIIb", c["r"], c.get("layers", 1), c.get("user", 0), c.get("priority", 0)))

        if any(n == name and t == RIG for t, n, _ in self.entries):
            raise PackError(f"{name}: duplicate rig")
        bones = rig["bones"]
        index = {}
        out = bytearray(struct.pack("<H", len(bones)))
        for k, bone in enumerate(bones):
            parent = bone.get("parent")
            if parent is not None and parent not in index:
                raise PackError(f"{name}: bone {bone['name']} comes before its parent {parent}")
            out += s16(bone["name"]) + struct.pack("<h", -1 if parent is None else index[parent])
            out += vec3(bone.get("offset", (0, 0, 0)), "offset") + s16(bone.get("mesh"))
            colliders = bone.get("colliders", [])
            out += struct.pack("<B", len(colliders)) + b"".join(collider(c, bone["name"]) for c in colliders)
            index[bone["name"]] = k
        slots = rig.get("slots", [])
        out += struct.pack("<H", len(slots))
        for slot in slots:
            if slot["bone"] not in index:
                raise PackError(f"{name}: slot on {slot['bone']}, which is not a bone")
            variants = slot.get("variants", [None])
            out += struct.pack("<H", index[slot["bone"]]) + vec3(slot.get("offset", (0, 0, 0)), "offset")
            out += vec3(slot.get("rot", (0, 0, 0)), "rot") + struct.pack("<B", len(variants))
            out += b"".join(s16(v) for v in variants) + collider(slot.get("collider"), "slot")
        attach = rig.get("attach", {})
        out += struct.pack("<H", len(attach))
        for point, a in sorted(attach.items()):
            if a["bone"] not in index:
                raise PackError(f"{name}: attach point {point} on {a['bone']}, which is not a bone")
            out += s16(point) + struct.pack("<H", index[a["bone"]]) + vec3(a.get("offset", (0, 0, 0)), "offset")
        anims = rig.get("anims", [])
        out += struct.pack("<H", len(anims))
        for anim in anims:
            keys = int(anim["keys"])
            if keys < 1:
                raise PackError(f"{name}: animation {anim['name']} has no keys")
            root_y = anim.get("root_y")
            out += s16(anim["name"]) + struct.pack("<BH", 1 if anim.get("loop", True) else 0, keys)
            if root_y:
                if len(root_y) != keys:
                    raise PackError(f"{name}: {anim['name']}: root_y needs one value per key")
                out += struct.pack(f"<B{keys}f", 1, *root_y)
            else:
                out += b"\0"
            tracks = anim.get("tracks", {})
            out += struct.pack("<H", len(tracks))
            for bone, track in sorted(tracks.items(), key=lambda t: index.get(t[0], -1)):
                if bone not in index or len(track) != keys or any(len(k) != 3 for k in track):
                    raise PackError(f"{name}: {anim['name']}: track {bone} must be a bone with {keys} keys of 3 angles")
                out += struct.pack("<H", index[bone]) + struct.pack(f"<{3 * keys}f", *(a for k in track for a in k))
            events = anim.get("events", [])
            out += struct.pack("<H", len(events))
            for ev in events:
                strings = [(k, v) for k, v in sorted(ev.items()) if k not in ("phase", "type") and isinstance(v, str)]
                numbers = [(k, v) for k, v in sorted(ev.items()) if k not in ("phase", "type") and not isinstance(v, str)]
                if any(not isinstance(v, (int, float)) for _, v in numbers):
                    raise PackError(f"{name}: {anim['name']}: event values must be strings or numbers")
                out += struct.pack("<B", int(ev["phase"]) & 0xFF) + s16(ev["type"])
                out += struct.pack("<B", len(strings)) + b"".join(s16(k) + s16(v) for k, v in strings)
                out += struct.pack("<B", len(numbers)) + b"".join(s16(k) + struct.pack("<f", v) for k, v in numbers)
        self.entries.append((RIG, name, bytes(out)))

    def check(self):
        """Every submesh names a material in this pack (the engine checks the same when loading), and every
        material's texture is in it (the engine would draw it magenta)."""
        materials = {n for t, n, _ in self.entries if t == MATERIAL}
        textures = {n for t, n, _ in self.entries if t == TEXTURE}
        for kind, name, payload in self.entries:
            if kind == MATERIAL and len(payload) > _MATERIAL.size:
                (length,) = struct.unpack_from("<H", payload, _MATERIAL.size)
                texture = payload[_MATERIAL.size + 2:_MATERIAL.size + 2 + length].decode()
                if texture and texture not in textures:
                    raise PackError(f"{name}: no texture {texture!r} in the pack")
        for kind, name, payload in self.entries:
            if kind != MESH or not payload[9] & 2:
                continue
            nv, nt, mode, _, _ = _MESH.unpack_from(payload)
            at = _MESH.size + 12 * nv + 6 * nt + (nt if mode & 1 else 0) + (3 * nv if mode & 2 else 0)
            (count,), at = struct.unpack_from("<I", payload, at), at + 4
            for _ in range(count):
                _, _, length = struct.unpack_from("<IIH", payload, at)
                material = payload[at + 10:at + 10 + length].decode()
                at += 10 + length
                if material and material not in materials:
                    raise PackError(f"{name}: no material {material!r} in the pack")

    def bytes(self):
        """The pack: entries sorted by (type, name), so the same input always gives the same bytes."""
        self.check()
        out, directory = bytearray(_HEADER.size), bytearray()
        for kind, name, payload in sorted(self.entries, key=lambda e: (e[0], e[1])):
            offset = len(out)
            out += _pad4(payload)
            raw = name.encode()
            directory += _DIR.pack(kind, 0, len(raw), offset, len(payload)) + raw
        _HEADER.pack_into(out, 0, MAGIC, VERSION, 0, len(self.entries), len(out))
        return bytes(out + directory)

    def write(self, path):
        with open(path, "wb") as f:
            f.write(self.bytes())


def read(data):
    """{(type, name): payload} from pack bytes; for tests and tools."""
    if len(data) < _HEADER.size:
        raise PackError("not a resource pack")
    magic, version, _, count, directory = _HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION:
        raise PackError("not a version 1 resource pack")
    entries, at = {}, directory
    for _ in range(count):
        kind, _, length, offset, size = _DIR.unpack_from(data, at)
        at += _DIR.size
        entries[(kind, data[at:at + length].decode())] = data[offset:offset + size]
        at += length
    return entries
