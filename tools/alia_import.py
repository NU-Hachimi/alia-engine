"""Import art made in other tools into alia-engine resource packs (docs/import.md). Pure Python, no dependencies; it
runs on the PC (in the runtime image, `alia assets`), never in a cart.

    from alia_pack import PackWriter
    from alia_import import import_glb, add_png, read_png, read_glb
    w = PackWriter()
    import_glb(w, "pipeline/models/crate.glb")            # meshes, materials, textures of a glTF 2.0 file (.glb / .gltf)
    add_png(w, "pipeline/textures/wood.png", wrap="repeat")
    w.write("assets/props.apak")

    width, height, rgba = read_png("pipeline/parts/eyes.png")   # the pixels, to composite in a builder of your own
    scene = read_glb("pipeline/hair/bob.glb")                    # meshes as arrays, materials, images

glTF and alia-engine agree on the axes (right-handed, +Y up, a model's front towards +Z) and the units (metres), so
nothing is turned; `scale` is for files that aren't in metres. Normals aren't read: the engine derives them from the
material's shading, as it does for every mesh. Colours: glTF's vertex colours and base colour factors are linear and
the engine's are display values, so they're converted (sRGB); textures are sRGB already and go in as they are.
"""

import base64
import json
import math
import os
import struct
import zlib

import alia_pack

VERSION = 1  # bumped when the same source gives different packs (alia assets rebuilds what it made before)

# The engine's palette before a game sets its own (src/render/handheld.hpp kDefaultPalette), for colors="palette".
DEFAULT_PALETTE = [
    (0, 0, 0), (255, 255, 255), (200, 200, 200), (140, 140, 140), (80, 80, 80), (35, 35, 40),
    (220, 60, 50), (240, 140, 40), (245, 210, 70), (120, 200, 70), (50, 140, 70), (60, 190, 190),
    (70, 150, 230), (50, 70, 170), (150, 80, 200), (230, 90, 170), (250, 170, 180), (130, 80, 50),
    (200, 160, 110), (240, 225, 190), (90, 120, 60), (60, 85, 45), (150, 140, 130), (100, 95, 105),
    (170, 210, 240), (110, 150, 200), (255, 240, 150), (255, 120, 90), (120, 240, 255), (200, 255, 140),
    (255, 160, 240), (250, 250, 250)]


class AssetError(ValueError):
    """A source that can't be imported (the message names the file and what's wrong)."""


def _warn(warnings, text):
    if warnings is not None:
        warnings.append(text)


def _source(src):
    """(bytes, the directory relative files are in, a name for messages)."""
    if isinstance(src, (bytes, bytearray, memoryview)):
        return bytes(src), os.getcwd(), "<bytes>"
    path = os.fspath(src)
    with open(path, "rb") as f:
        return f.read(), os.path.dirname(os.path.abspath(path)), path


# ---- PNG ----------------------------------------------------------------------------------------------------------

def read_png(src):
    """A PNG (a path or its bytes) -> (width, height, RGBA bytes, top row first). Every colour type and bit depth,
    with tRNS transparency; not interlaced (re-save it without interlacing)."""
    data, _, name = _source(src)
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssetError(f"{name}: not a PNG")
    at, idat, plte, trns, ihdr = 8, bytearray(), None, None, None
    while at + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        chunk = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"PLTE":
            plte = chunk
        elif kind == b"tRNS":
            trns = chunk
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
        at += 12 + length
    if not ihdr:
        raise AssetError(f"{name}: no IHDR")
    w, h, depth, ctype, _, _, interlace = ihdr
    if interlace:
        raise AssetError(f"{name}: interlaced PNG (save it without interlacing)")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if channels is None or depth not in (1, 2, 4, 8, 16) or (ctype in (2, 4, 6) and depth < 8) or (ctype == 3 and depth > 8):
        raise AssetError(f"{name}: unsupported PNG (colour type {ctype}, depth {depth})")
    try:
        raw = zlib.decompress(bytes(idat))
    except zlib.error as err:
        raise AssetError(f"{name}: broken image data ({err})") from None
    bits = channels * depth
    stride, bpp = (w * bits + 7) // 8, max(1, bits // 8)
    if len(raw) < h * (stride + 1):
        raise AssetError(f"{name}: truncated image data")
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if f == 1:
            for k in range(bpp, stride):
                line[k] = (line[k] + line[k - bpp]) & 0xFF
        elif f == 2:
            for k in range(stride):
                line[k] = (line[k] + prev[k]) & 0xFF
        elif f == 3:
            for k in range(stride):
                line[k] = (line[k] + ((line[k - bpp] if k >= bpp else 0) + prev[k]) // 2) & 0xFF
        elif f == 4:
            for k in range(stride):
                a, b, c = (line[k - bpp] if k >= bpp else 0), prev[k], (prev[k - bpp] if k >= bpp else 0)
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[k] = (line[k] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        elif f != 0:
            raise AssetError(f"{name}: bad PNG filter {f}")
        rows.append(line)
        prev = line
    out = bytearray(w * h * 4)
    key = None
    if trns is not None and ctype == 0 and len(trns) >= 2:
        key = struct.unpack(">H", trns[:2])[0]
    elif trns is not None and ctype == 2 and len(trns) >= 6:
        key = struct.unpack(">3H", trns[:6])
    alpha = list(trns) + [255] * 256 if ctype == 3 and trns is not None else [255] * 256
    scale = 255 // ((1 << depth) - 1) if depth < 8 else 1
    o = 0
    for line in rows:
        if depth == 16:
            vals = struct.unpack(f">{len(line) // 2}H", bytes(line))
        elif depth == 8:
            vals = line
        else:
            vals = [(line[k // (8 // depth)] >> (8 - depth - (k % (8 // depth)) * depth)) & ((1 << depth) - 1)
                    for k in range(w * channels)]
        for x in range(w):
            px = vals[x * channels:(x + 1) * channels]
            to8 = (lambda v: v >> 8) if depth == 16 else (lambda v: v * scale if ctype != 3 else v)
            if ctype == 3:
                idx = px[0]
                if plte is None or idx * 3 + 2 >= len(plte):
                    raise AssetError(f"{name}: palette index out of range")
                r, g, b, a = plte[idx * 3], plte[idx * 3 + 1], plte[idx * 3 + 2], alpha[idx]
            elif ctype == 0:
                r = g = b = to8(px[0])
                a = 0 if key is not None and px[0] == key else 255
            elif ctype == 4:
                r = g = b = to8(px[0])
                a = to8(px[1])
            elif ctype == 2:
                r, g, b = (to8(v) for v in px)
                a = 0 if key is not None and tuple(px) == key else 255
            else:
                r, g, b, a = (to8(v) for v in px)
            out[o:o + 4] = bytes((r, g, b, a))
            o += 4
    return w, h, bytes(out)


def add_png(writer, src, name=None, filter="nearest", wrap="clamp", mipmaps=False):
    """A PNG (or JPEG) file as a texture of the pack (PackWriter.add_texture), named after the file by default.
    Returns the name."""
    if name is None:
        if not isinstance(src, (str, os.PathLike)):
            raise AssetError("add_png: name= is needed for bytes")
        name = os.path.splitext(os.path.basename(os.fspath(src)))[0]
    data, _, where = _source(src)
    try:
        writer.add_texture(name, data, filter=filter, wrap=wrap, mipmaps=mipmaps)
    except alia_pack.PackError as err:
        raise AssetError(f"{where}: {err}") from None
    return name


# ---- glTF -------------------------------------------------------------------------------------------------------

_COMPONENT = {5120: ("b", 1, 127.0), 5121: ("B", 1, 255.0), 5122: ("h", 2, 32767.0), 5123: ("H", 2, 65535.0),
              5125: ("I", 4, None), 5126: ("f", 4, None)}
_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
_SUPPORTED_EXTENSIONS = {"KHR_materials_unlit", "KHR_texture_transform", "KHR_materials_emissive_strength"}


class GlbMaterial:
    """A glTF material, as the importer reads it."""

    def __init__(self, name, base_color=(1.0, 1.0, 1.0, 1.0), texture=None, alpha_mode="OPAQUE", alpha_cutoff=0.5,
                 double_sided=False, unlit=False, emissive=(0.0, 0.0, 0.0)):
        self.name, self.base_color, self.texture = name, tuple(base_color), texture  # texture: an index into images
        self.alpha_mode, self.alpha_cutoff, self.double_sided = alpha_mode, alpha_cutoff, double_sided
        self.unlit, self.emissive = unlit, tuple(emissive)
        self.wrap, self.filter = None, None  # from the texture's sampler: "repeat"/"clamp", "nearest"/"linear"

    def __repr__(self):
        return f"GlbMaterial({self.name!r}, texture={self.texture})"


class GlbImage:
    def __init__(self, name, mime, data):
        self.name, self.mime, self.data = name, mime, data

    def __repr__(self):
        return f"GlbImage({self.name!r}, {self.mime}, {len(self.data)} bytes)"


class GlbMesh:
    """One mesh: flat arrays (positions xyz, indices of triangles, colors rgba 0..1 linear or None, uv or None) and
    its parts [(first triangle, triangle count, material index or None)]."""

    def __init__(self, name):
        self.name = name
        self.positions, self.indices, self.colors, self.uv, self.parts = [], [], [], [], []
        self._has_colors = self._has_uv = False

    def _finish(self):
        if not self._has_colors:
            self.colors = None
        if not self._has_uv:
            self.uv = None

    @property
    def vertices(self):
        return len(self.positions) // 3

    @property
    def triangles(self):
        return len(self.indices) // 3

    def __repr__(self):
        return f"GlbMesh({self.name!r}, {self.vertices} vertices, {self.triangles} triangles, {len(self.parts)} parts)"


class GlbScene:
    def __init__(self, meshes, materials, images):
        self.meshes, self.materials, self.images = meshes, materials, images

    def __repr__(self):
        return f"GlbScene({len(self.meshes)} meshes, {len(self.materials)} materials, {len(self.images)} images)"


def _mat_mul(a, b):
    return [sum(a[r + 4 * k] * b[k + 4 * c] for k in range(4)) for c in range(4) for r in range(4)]  # column-major


def _node_matrix(node):
    if "matrix" in node:
        return [float(v) for v in node["matrix"]]
    tx, ty, tz = node.get("translation", (0, 0, 0))
    qx, qy, qz, qw = node.get("rotation", (0, 0, 0, 1))
    sx, sy, sz = node.get("scale", (1, 1, 1))
    r = [1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy + qz * qw), 2 * (qx * qz - qy * qw),
         2 * (qx * qy - qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz + qx * qw),
         2 * (qx * qz + qy * qw), 2 * (qy * qz - qx * qw), 1 - 2 * (qx * qx + qy * qy)]
    return [r[0] * sx, r[1] * sx, r[2] * sx, 0, r[3] * sy, r[4] * sy, r[5] * sy, 0,
            r[6] * sz, r[7] * sz, r[8] * sz, 0, tx, ty, tz, 1]


def _parse(data, base, name):
    """(the glTF JSON, [buffer bytes]) of a .glb or .gltf."""
    if data[:4] == b"glTF":
        _, length = struct.unpack_from("<II", data, 4)
        at, doc, binary = 12, None, None
        while at + 8 <= min(length, len(data)):
            size, kind = struct.unpack_from("<II", data, at)
            chunk = data[at + 8:at + 8 + size]
            if kind == 0x4E4F534A:
                doc = json.loads(chunk.decode("utf-8"))
            elif kind == 0x004E4942 and binary is None:
                binary = chunk
            at += 8 + size
        if doc is None:
            raise AssetError(f"{name}: a .glb without its JSON chunk")
    else:
        try:
            doc, binary = json.loads(data.decode("utf-8")), None
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise AssetError(f"{name}: neither a .glb nor a .gltf") from None
    if not str(doc.get("asset", {}).get("version", "")).startswith("2"):
        raise AssetError(f"{name}: not glTF 2.0")
    unsupported = sorted(set(doc.get("extensionsRequired", [])) - _SUPPORTED_EXTENSIONS)
    if unsupported:
        raise AssetError(f"{name}: needs {', '.join(unsupported)} (export without it: no Draco / meshopt compression)")
    buffers = []
    for k, b in enumerate(doc.get("buffers", [])):
        uri = b.get("uri")
        if uri is None:
            if binary is None:
                raise AssetError(f"{name}: buffer {k} has no data")
            buffers.append(binary)
        elif uri.startswith("data:"):
            buffers.append(base64.b64decode(uri.split(",", 1)[1]))
        else:
            path = os.path.join(base, uri.replace("%20", " "))
            try:
                with open(path, "rb") as f:
                    buffers.append(f.read())
            except OSError:
                raise AssetError(f"{name}: buffer {uri} not found next to it") from None
    return doc, buffers


def _view(doc, buffers, index, name):
    v = doc["bufferViews"][index]
    buf = buffers[v["buffer"]]
    start = v.get("byteOffset", 0)
    return buf[start:start + v["byteLength"]], v.get("byteStride")


def _accessor(doc, buffers, index, name):
    """An accessor's values as a flat list of floats (normalized integers to 0..1 / -1..1) or ints."""
    a = doc["accessors"][index]
    if "sparse" in a:
        raise AssetError(f"{name}: sparse accessors aren't supported (export without them)")
    fmt, size, norm = _COMPONENT[a["componentType"]]
    n, count = _COUNT[a["type"]], a["count"]
    if "bufferView" not in a:
        return [0.0] * (n * count)
    data, stride = _view(doc, buffers, a["bufferView"], name)
    offset, elem = a.get("byteOffset", 0), size * n
    stride = stride or elem
    if count and offset + stride * (count - 1) + elem > len(data):
        raise AssetError(f"{name}: accessor {index} runs past its buffer")
    if stride == elem:
        out = list(struct.unpack_from(f"<{n * count}{fmt}", data, offset))
    else:
        out = []
        for k in range(count):
            out += struct.unpack_from(f"<{n}{fmt}", data, offset + k * stride)
    if a.get("normalized") and norm:
        out = [max(v / norm, -1.0) for v in out]
    elif norm and a["type"] != "SCALAR" and fmt in ("B", "H"):  # COLOR_0 / TEXCOORD_0 as unsigned ints are normalized
        out = [v / norm for v in out]
    return out


def read_glb(src, merge=True, scale=1.0, warnings=None):
    """A glTF 2.0 file (.glb, or .gltf with its .bin and images next to it; a path or the bytes) -> GlbScene:
    .meshes [GlbMesh] (merge True: the whole default scene as one mesh with every node's transform baked in; False:
    one mesh per node that has one, in world space, named after the node), .materials [GlbMaterial],
    .images [GlbImage] (PNG / JPEG bytes). Triangles only; positions times `scale`."""
    data, base, name = _source(src)
    doc, buffers = _parse(data, base, name)
    images = []
    for k, im in enumerate(doc.get("images", [])):
        label = im.get("name") or (os.path.splitext(os.path.basename(im["uri"]))[0] if "uri" in im else f"image{k}")
        if "bufferView" in im:
            blob = bytes(_view(doc, buffers, im["bufferView"], name)[0])
        elif im.get("uri", "").startswith("data:"):
            blob = base64.b64decode(im["uri"].split(",", 1)[1])
        elif "uri" in im:
            try:
                with open(os.path.join(base, im["uri"].replace("%20", " ")), "rb") as f:
                    blob = f.read()
            except OSError:
                raise AssetError(f"{name}: image {im['uri']} not found next to it") from None
        else:
            raise AssetError(f"{name}: image {k} has no data")
        mime = "image/png" if blob[:8] == b"\x89PNG\r\n\x1a\n" else "image/jpeg" if blob[:2] == b"\xff\xd8" else None
        if mime is None:
            raise AssetError(f"{name}: image {label} is neither PNG nor JPEG (export PNG textures)")
        images.append(GlbImage(label, mime, blob))
    textures, samplers = doc.get("textures", []), doc.get("samplers", [])
    materials = []
    for k, m in enumerate(doc.get("materials", [])):
        pbr = m.get("pbrMetallicRoughness", {})
        mat = GlbMaterial(m.get("name") or f"material{k}", pbr.get("baseColorFactor", (1, 1, 1, 1)),
                          alpha_mode=m.get("alphaMode", "OPAQUE"), alpha_cutoff=m.get("alphaCutoff", 0.5),
                          double_sided=m.get("doubleSided", False),
                          unlit="KHR_materials_unlit" in m.get("extensions", {}),
                          emissive=m.get("emissiveFactor", (0, 0, 0)))
        info = pbr.get("baseColorTexture")
        if info is not None:
            t = textures[info["index"]]
            if "source" not in t:
                raise AssetError(f"{name}: material {mat.name}: its texture is not PNG / JPEG (export PNG textures)")
            mat.texture = t["source"]
            if info.get("texCoord", 0) != 0:
                _warn(warnings, f"{name}: material {mat.name}: texture uses TEXCOORD_{info['texCoord']}; TEXCOORD_0 is read")
            if "KHR_texture_transform" in info.get("extensions", {}):
                _warn(warnings, f"{name}: material {mat.name}: KHR_texture_transform is ignored")
            s = samplers[t["sampler"]] if "sampler" in t else {}
            wrap = {s.get("wrapS", 10497), s.get("wrapT", 10497)}
            mat.wrap = "clamp" if wrap == {33071} else "repeat"
            if 33648 in wrap:
                _warn(warnings, f"{name}: material {mat.name}: mirrored repeat becomes repeat")
            if "magFilter" in s:
                mat.filter = "linear" if s["magFilter"] == 9729 else "nearest"
        materials.append(mat)

    def primitives(mesh_index, matrix, into):
        flip = _det3(matrix) < 0  # a mirroring transform turns the winding round
        for p in doc["meshes"][mesh_index].get("primitives", []):
            if p.get("mode", 4) != 4:
                raise AssetError(f"{name}: mesh {doc['meshes'][mesh_index].get('name', mesh_index)}: only triangles "
                                  f"(mode {p['mode']})")
            attrs = p.get("attributes", {})
            if "POSITION" not in attrs:
                continue
            if "targets" in p:
                _warn(warnings, f"{name}: morph targets are ignored")
            pos = _accessor(doc, buffers, attrs["POSITION"], name)
            nv = len(pos) // 3
            base_v = into.vertices
            for k in range(nv):
                x, y, z = pos[3 * k:3 * k + 3]
                into.positions += [(matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12]) * scale,
                                   (matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13]) * scale,
                                   (matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14]) * scale]
            if "COLOR_0" in attrs:  # rgba 0..1 for every vertex; white where a primitive has none
                c = _accessor(doc, buffers, attrs["COLOR_0"], name)
                n = 4 if doc["accessors"][attrs["COLOR_0"]]["type"] == "VEC4" else 3
                into.colors += [v for k in range(nv) for v in (c[n * k:n * k + 3] + ([c[n * k + 3]] if n == 4 else [1.0]))]
                into._has_colors = True
            else:
                into.colors += [1.0, 1.0, 1.0, 1.0] * nv
            if "TEXCOORD_0" in attrs:
                into.uv += _accessor(doc, buffers, attrs["TEXCOORD_0"], name)
                into._has_uv = True
            else:
                into.uv += [0.0, 0.0] * nv
            idx = _accessor(doc, buffers, p["indices"], name) if "indices" in p else list(range(nv))
            if len(idx) % 3:
                raise AssetError(f"{name}: a primitive's indices aren't whole triangles")
            first = into.triangles
            for k in range(0, len(idx), 3):
                a, b, c = idx[k:k + 3]
                if max(a, b, c) >= nv:
                    raise AssetError(f"{name}: index out of range")
                into.indices += [base_v + a, base_v + c, base_v + b] if flip else [base_v + a, base_v + b, base_v + c]
            into.parts.append((first, into.triangles - first, p.get("material")))

    scenes = doc.get("scenes", [])
    roots = scenes[doc.get("scene", 0)]["nodes"] if scenes else [k for k in range(len(doc.get("nodes", [])))
                                                                  if not any(k in n.get("children", []) for n in doc["nodes"])]
    meshes, merged, used = [], GlbMesh(""), set()

    def visit(k, parent):
        node = doc["nodes"][k]
        matrix = _mat_mul(parent, _node_matrix(node))
        if "mesh" in node:
            if "skin" in node:
                _warn(warnings, f"{name}: skinning is ignored (node {node.get('name', k)}): the rest pose is imported")
            if merge:
                primitives(node["mesh"], matrix, merged)
            else:
                label = node.get("name") or doc["meshes"][node["mesh"]].get("name") or f"node{k}"
                unique, n = label, 2
                while unique in used:
                    unique, n = f"{label}.{n}", n + 1
                used.add(unique)
                m = GlbMesh(unique)
                primitives(node["mesh"], matrix, m)
                m._finish()
                if m.triangles:
                    meshes.append(m)
        for c in node.get("children", []):
            visit(c, matrix)

    identity = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]
    for r in roots:
        visit(r, identity)
    if merge and merged.triangles:
        merged._finish()
        meshes = [merged]
    if not meshes:
        raise AssetError(f"{name}: no triangles in its scene")
    return GlbScene(meshes, materials, images)


def _det3(m):
    return (m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2])
            + m[8] * (m[1] * m[6] - m[5] * m[2]))


def _srgb(v):
    """A linear 0..1 value -> a display byte."""
    v = min(max(v, 0.0), 1.0)
    return round(255 * (12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055))


def _nearest(rgb, palette):
    return min(range(len(palette)), key=lambda i: sum((a - b) ** 2 for a, b in zip(rgb, palette[i])))


def import_glb(writer, src, prefix=None, merge=True, colors="vertex", shading="smooth", scale=1.0, filter=None,
               mipmaps=False, palette=None, warnings=None):
    """A glTF 2.0 file's meshes, materials and textures into `writer` (an alia_pack.PackWriter). Returns
    {"meshes": [...], "materials": [...], "textures": [...]}, the names added:

    - meshes: merge True (props): one mesh named `prefix`; False: "prefix/<node>" for each node with a mesh;
    - materials "prefix/<material>"; textures "prefix/<image>" (the PNG / JPEG embedded in the file, as it is).

    prefix: default the file's name without the extension. colors: "vertex" (base colour factor x COLOR_0, smooth
    colours) or "palette" (each triangle snapped to the nearest of `palette`, default the engine's default palette:
    the handheld's flat look). shading: "smooth", "flat" or "unlit" (materials with KHR_materials_unlit are unlit
    anyway). filter: a texture's filter; default its sampler's magFilter, else "nearest". mipmaps: for every texture.
    What isn't carried over (metal / roughness, emissive, a factor on a textured material, texture transforms) is
    reported in `warnings` (a list, appended to) when one is given."""
    data, _, where = _source(src)
    if prefix is None:
        if where == "<bytes>":
            raise AssetError("import_glb: prefix= is needed for bytes")
        prefix = os.path.splitext(os.path.basename(where))[0]
    if colors not in ("vertex", "palette") or shading not in ("smooth", "flat", "unlit"):
        raise AssetError("import_glb: colors 'vertex' or 'palette'; shading 'smooth', 'flat' or 'unlit'")
    palette = [tuple(c) for c in (palette or DEFAULT_PALETTE)]
    scene = read_glb(data if where == "<bytes>" else where, merge=merge, scale=scale, warnings=warnings)
    names = {"meshes": [], "materials": [], "textures": []}
    image_names = {}

    def texture_name(index, mat):
        if index not in image_names:
            label = f"{prefix}/{scene.images[index].name}"
            taken, n = {t for t in image_names.values()}, 2
            unique = label
            while unique in taken:
                unique, n = f"{label}.{n}", n + 1
            writer.add_texture(unique, scene.images[index].data, filter=filter or mat.filter or "nearest",
                               wrap=mat.wrap or "repeat", mipmaps=mipmaps)
            image_names[index] = unique
            names["textures"].append(unique)
        return image_names[index]

    material_names = {}

    def material_name(index):
        """The pack material for glTF material `index` (None: glTF's default material, white)."""
        if index in material_names:
            return material_names[index]
        mat = scene.materials[index] if index is not None else GlbMaterial("default")
        label = f"{prefix}/{mat.name}"
        taken, n, unique = set(material_names.values()), 2, label
        while unique in taken:
            unique, n = f"{label}.{n}", n + 1
        textured = mat.texture is not None
        alpha = {"OPAQUE": "opaque", "MASK": "cutout", "BLEND": "blend"}.get(mat.alpha_mode, "opaque")
        if alpha == "cutout" and not textured:
            _warn(warnings, f"{where}: material {mat.name}: MASK without a texture is drawn opaque")
            alpha = "opaque"
        if alpha == "cutout" and abs(mat.alpha_cutoff - 0.5) > 1e-3:
            _warn(warnings, f"{where}: material {mat.name}: alphaCutoff {mat.alpha_cutoff} becomes 0.5 (the engine's)")
        if textured and any(abs(v - 1) > 1e-3 for v in mat.base_color[:3]):
            _warn(warnings, f"{where}: material {mat.name}: its base colour factor isn't applied to the texture")
        if any(v > 0 for v in mat.emissive):
            _warn(warnings, f"{where}: material {mat.name}: emissive is ignored")
        kw = dict(shading="unlit" if mat.unlit else shading, double_sided=mat.double_sided, alpha_mode=alpha,
                  opacity=min(max(mat.base_color[3], 0.0), 1.0) if alpha == "blend" else 1.0)
        if textured:
            kw.update(color="texture", texture=texture_name(mat.texture, mat))
        else:
            kw.update(color="palette" if colors == "palette" else "vertex")
        writer.add_material(unique, **kw)
        material_names[index] = unique
        names["materials"].append(unique)
        return unique

    for mesh in scene.meshes:
        label = prefix if merge else f"{prefix}/{mesh.name}"
        if mesh.vertices > 65535:
            raise AssetError(f"{where}: mesh {label}: {mesh.vertices} vertices, at most 65535 per mesh "
                              "(split it, or import with merge=False)")
        factor = [None] * mesh.vertices  # per vertex: its material's base colour (untextured parts)
        tri_mat = []
        for first, count, mi in mesh.parts:
            mat = scene.materials[mi] if mi is not None else GlbMaterial("default")
            tri_mat += [mat] * count
            for t in range(first, first + count):
                for v in mesh.indices[3 * t:3 * t + 3]:
                    factor[v] = mat.base_color
        def rgb_lin(v):
            f = factor[v] or (1.0, 1.0, 1.0, 1.0)
            c = mesh.colors[4 * v:4 * v + 3] if mesh.colors is not None else (1.0, 1.0, 1.0)
            return [c[k] * f[k] for k in range(3)]
        any_textured = any(m.texture is not None for m in tri_mat)
        textured_vs = {v for t, m in enumerate(tri_mat) if m.texture is not None for v in mesh.indices[3 * t:3 * t + 3]}
        if mesh.colors is not None and any(abs(c - 1) > 1e-3 for v in textured_vs for c in mesh.colors[4 * v:4 * v + 3]):
            _warn(warnings, f"{where}: mesh {label}: vertex colours on textured parts aren't shown")
        vertex_rgb = tri_palette = None
        if colors == "vertex":
            vertex_rgb = bytes(_srgb(c) for v in range(mesh.vertices) for c in rgb_lin(v))
        else:
            tri_palette = []
            for t in range(mesh.triangles):
                vs = mesh.indices[3 * t:3 * t + 3]
                lin = [sum(rgb_lin(v)[k] for v in vs) / 3 for k in range(3)]
                tri_palette.append(_nearest([_srgb(c) for c in lin], palette))
        uv = mesh.uv if any_textured else None
        if any_textured and uv is None:
            raise AssetError(f"{where}: mesh {label}: textured but no TEXCOORD_0")
        subs = [(first, count, material_name(mi)) for first, count, mi in mesh.parts if count]
        try:
            writer.add_mesh(label, mesh.positions, mesh.indices, tri_palette=tri_palette, vertex_rgb=vertex_rgb,
                            submeshes=subs, uv=uv)
        except alia_pack.PackError as err:
            raise AssetError(f"{where}: {err}") from None
        names["meshes"].append(label)
    return names
