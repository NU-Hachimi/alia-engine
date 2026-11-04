"""Importing art (docs/import.md, I1): read_png for every PNG kind, read_glb (node transforms, mirrored nodes,
normalized colours, .gltf with files next to it, what's refused), import_glb into packs that the engine draws right,
and `alia assets` (incremental, --check, --force, the game's builder)."""

import json
import math
import os
import shutil
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import alia_assets  # noqa: E402
import alia_import  # noqa: E402
from alia_import import AssetError, import_glb, read_glb, read_png  # noqa: E402
from alia_pack import PackWriter  # noqa: E402

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


def raises(fn, needle=""):
    try:
        fn()
    except (AssetError, ValueError) as err:
        return needle in str(err)
    return False


# ---- PNG writer (every colour type / depth / filter) -----------------------------------------------------------

def png_bytes(w, h, ctype, depth, rows, plte=None, trns=None, filters=(0,)):
    """rows: per row, the raw (unfiltered) bytes; each row filtered with filters[y % len]."""
    bpp = max(1, ({0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype] * depth) // 8)
    out, prev = b"", bytes(len(rows[0]))
    for y, row in enumerate(rows):
        f = filters[y % len(filters)]
        enc = bytearray()
        for k, v in enumerate(row):
            a = row[k - bpp] if k >= bpp else 0
            b, c = prev[k], prev[k - bpp] if k >= bpp else 0
            p = a + b - c
            pr = a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else b if abs(p - b) <= abs(p - c) else c
            enc.append((v - (0, a, b, (a + b) // 2, pr)[f]) & 0xFF)
        out += bytes([f]) + bytes(enc)
        prev = row
    chunk = lambda kind, data: (struct.pack(">I", len(data)) + kind + data
                                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    body = chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 0))
    if plte:
        body += chunk(b"PLTE", plte)
    if trns:
        body += chunk(b"tRNS", trns)
    return b"\x89PNG\r\n\x1a\n" + body + chunk(b"IDAT", zlib.compress(out)) + chunk(b"IEND", b"")


def rgba_png(w, h, fn):
    return png_bytes(w, h, 6, 8, [bytes(c for x in range(w) for c in fn(x, y)) for y in range(h)])


px = lambda rgba, w, x, y: tuple(rgba[(y * w + x) * 4:(y * w + x) * 4 + 4])
RGBA = [(255, 0, 0, 255), (0, 255, 0, 128), (0, 0, 255, 0), (10, 20, 30, 255)]
img = rgba_png(2, 2, lambda x, y: RGBA[y * 2 + x])
check(read_png(img) == (2, 2, bytes(c for p in RGBA for c in p)), "read_png: RGBA 8-bit")
rows = [bytes(c for x in range(3) for c in (x * 40, y * 50, 7, 200)) for y in range(4)]
w, h, d = read_png(png_bytes(3, 4, 6, 8, rows, filters=(0, 1, 2, 3, 4)))
check(all(px(d, 3, x, y) == (x * 40, y * 50, 7, 200) for x in range(3) for y in range(4)), "read_png: all five filters")
w, h, d = read_png(png_bytes(2, 1, 2, 8, [bytes([1, 2, 3, 9, 9, 9])], trns=struct.pack(">3H", 9, 9, 9)))
check(px(d, 2, 0, 0) == (1, 2, 3, 255) and px(d, 2, 1, 0) == (9, 9, 9, 0), "read_png: RGB with a tRNS colour key")
w, h, d = read_png(png_bytes(2, 1, 2, 16, [struct.pack(">6H", 65535, 0, 32768, 256, 512, 1024)]))
check(px(d, 2, 0, 0) == (255, 0, 128, 255) and px(d, 2, 1, 0) == (1, 2, 4, 255), "read_png: 16-bit RGB")
w, h, d = read_png(png_bytes(8, 1, 0, 1, [bytes([0b10100000])]))
check([px(d, 8, x, 0)[0] for x in range(4)] == [255, 0, 255, 0], "read_png: 1-bit grey")
w, h, d = read_png(png_bytes(2, 1, 4, 8, [bytes([100, 50, 200, 255])]))
check(px(d, 2, 0, 0) == (100, 100, 100, 50), "read_png: grey + alpha")
w, h, d = read_png(png_bytes(4, 1, 3, 4, [bytes([0x01, 0x20])], plte=bytes([0, 0, 0, 255, 0, 0, 0, 255, 0]),
                             trns=bytes([255, 128])))
check([px(d, 4, x, 0) for x in range(4)] == [(0, 0, 0, 255), (255, 0, 0, 128), (0, 255, 0, 255), (0, 0, 0, 255)],
      "read_png: 4-bit palette with tRNS alpha")
check(raises(lambda: read_png(b"nope"), "not a PNG"), "read_png refuses what isn't a PNG")


# ---- glTF writer --------------------------------------------------------------------------------------------------

class Gltf:
    def __init__(self):
        self.doc = {"asset": {"version": "2.0"}, "scenes": [{"nodes": []}], "scene": 0, "nodes": [], "meshes": [],
                    "accessors": [], "bufferViews": [], "buffers": [], "materials": []}
        self.bin = bytearray()

    def view(self, data):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["bufferViews"].append({"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)})
        self.bin += data
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values, ctype, kind, normalized=False):
        fmt = {5126: "f", 5121: "B", 5123: "H", 5125: "I"}[ctype]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[kind]
        a = {"bufferView": self.view(struct.pack(f"<{len(values)}{fmt}", *values)), "componentType": ctype,
             "count": len(values) // n, "type": kind}
        if normalized:
            a["normalized"] = True
        if kind == "VEC3" and ctype == 5126:
            a["min"] = [min(values[k::3]) for k in range(3)]
            a["max"] = [max(values[k::3]) for k in range(3)]
        self.doc["accessors"].append(a)
        return len(self.doc["accessors"]) - 1

    def mesh(self, prims):
        self.doc["meshes"].append({"primitives": prims})
        return len(self.doc["meshes"]) - 1

    def prim(self, pos, idx, material=None, colors=None, uv=None):
        p = {"attributes": {"POSITION": self.accessor(pos, 5126, "VEC3")}, "indices": self.accessor(idx, 5123, "SCALAR")}
        if colors:
            p["attributes"]["COLOR_0"] = self.accessor(colors, 5121, "VEC4", normalized=True)
        if uv:
            p["attributes"]["TEXCOORD_0"] = self.accessor(uv, 5126, "VEC2")
        if material is not None:
            p["material"] = material
        return p

    def node(self, root=True, **kw):
        self.doc["nodes"].append(kw)
        k = len(self.doc["nodes"]) - 1
        if root:
            self.doc["scenes"][0]["nodes"].append(k)
        return k

    def material(self, **kw):
        self.doc["materials"].append(kw)
        return len(self.doc["materials"]) - 1

    def image(self, data, name):
        self.doc.setdefault("images", []).append({"bufferView": self.view(data), "mimeType": "image/png", "name": name})
        self.doc.setdefault("samplers", []).append({"magFilter": 9728, "wrapS": 33071, "wrapT": 33071})
        self.doc.setdefault("textures", []).append({"source": len(self.doc["images"]) - 1,
                                                    "sampler": len(self.doc["samplers"]) - 1})
        return len(self.doc["textures"]) - 1

    def glb(self):
        if not self.doc["materials"]:
            del self.doc["materials"]
        self.doc["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(self.doc).encode()
        js += b" " * (-len(js) % 4)
        binary = bytes(self.bin) + bytes(-len(self.bin) % 4)
        body = struct.pack("<II", len(js), 0x4E4F534A) + js + struct.pack("<II", len(binary), 0x004E4942) + binary
        return struct.pack("<4sII", b"glTF", 2, 12 + len(body)) + body


QUAD_P = [1, -1, 0, -1, -1, 0, -1, 1, 0, 1, 1, 0]  # 2 x 2 m, facing -Z (a camera at yaw 0 sees it), as kit.quad
QUAD_I = [0, 1, 2, 0, 2, 3]
QUAD_UV = [0, 1, 1, 1, 1, 0, 0, 0]
QUAD4 = rgba_png(2, 2, lambda x, y: [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255)][y * 2 + x])
LEAF = rgba_png(2, 2, lambda x, y: (40, 200, 40, 255) if x == 0 else (0, 0, 0, 0))

g = Gltf()
tex = g.image(QUAD4, "quad4")
leaf_tex = g.image(LEAF, "leaf")
pic = g.material(name="picture", pbrMetallicRoughness={"baseColorTexture": {"index": tex}})
leaf = g.material(name="leaf", alphaMode="MASK", doubleSided=True, pbrMetallicRoughness={"baseColorTexture": {"index": leaf_tex}})
glass = g.material(name="glass", alphaMode="BLEND", pbrMetallicRoughness={"baseColorFactor": [0.2140, 0.2140, 0.2140, 0.5]})
glow = g.material(name="glow", extensions={"KHR_materials_unlit": {}}, emissiveFactor=[1, 0, 0],
                  pbrMetallicRoughness={"baseColorFactor": [1, 1, 1, 1]})
picture = g.mesh([g.prim(QUAD_P, QUAD_I, pic, uv=QUAD_UV)])
# vertex colours (normalized bytes, linear): 55 ~ linear 0.2157 -> display ~128
painted = g.mesh([g.prim(QUAD_P, QUAD_I, glass, colors=[55, 55, 55, 255] * 4),
                  g.prim([v + (2 if k % 3 == 0 else 0) for k, v in enumerate(QUAD_P)], QUAD_I, glow)])
card = g.mesh([g.prim(QUAD_P, QUAD_I, leaf, uv=QUAD_UV)])
child = g.node(root=False, name="lid", mesh=painted, translation=[0, 0, 1],
               rotation=[0, math.sin(math.pi / 4), 0, math.cos(math.pi / 4)], scale=[2, 1, 1])
g.node(name="crate", mesh=picture, translation=[0, 1, 0], children=[child])
g.node(name="mirror", mesh=card, scale=[-1, 1, 1], translation=[5, 0, 0])
GLB = g.glb()

# ---- read_glb -----------------------------------------------------------------------------------------------------
warn = []
s = read_glb(GLB, merge=False, warnings=warn)
names = [m.name for m in s.meshes]
check(names == ["crate", "lid", "mirror"], f"read_glb merge=False: a mesh per node, named after it ({names})")
crate, lid, mirror = s.meshes
check(crate.positions[:3] == [1, 0, 0], f"the node's translation is applied ({crate.positions[:3]})")
# lid: scale x2 on X, then 90 deg about Y, then +1 Z, then the parent's +1 Y. Its vertex (1, -1, 0) -> scaled (2, -1, 0)
# -> rotated about Y by 90 deg: (0, -1, -2) -> + (0, 0, 1) + (0, 1, 0) = (0, 0, -1)
got = [round(v, 5) for v in lid.positions[:3]]
check(got == [0, 0, -1], f"a child's TRS under its parent's, in order (scale, rotate, translate) ({got})")
check(lid.parts == [(0, 2, glass), (2, 2, glow)] and lid.vertices == 8, f"one part per primitive ({lid.parts})")
check(abs(lid.colors[0] - 55 / 255) < 1e-6 and lid.colors[4 * 4:4 * 4 + 4] == [1.0] * 4,
      "COLOR_0 normalized bytes -> 0..1; a primitive without colours is white")
check(mirror.indices[:3] == [0, 2, 1], f"a mirroring node turns the winding round ({mirror.indices[:3]})")
merged = read_glb(GLB).meshes
check(len(merged) == 1 and merged[0].vertices == 16 and len(merged[0].parts) == 4,
      "read_glb merge=True: the whole scene in one mesh, every primitive a part")
check([m.name for m in s.materials] == ["picture", "leaf", "glass", "glow"] and s.materials[3].unlit
      and s.materials[0].texture == 0 and s.materials[0].wrap == "clamp" and s.materials[0].filter == "nearest",
      "materials: names, unlit, texture with the sampler's wrap and filter")
check([i.name for i in s.images] == ["quad4", "leaf"] and s.images[0].data == QUAD4, "embedded images as they are")
scaled = read_glb(GLB, scale=0.01).meshes[0]
check(abs(scaled.positions[1] - 0.0) < 1e-9 and abs(scaled.positions[0] - 0.01) < 1e-9, "scale= scales positions")

bad = Gltf()
bad.node(mesh=bad.mesh([bad.prim(QUAD_P, QUAD_I)]))
bad.doc["extensionsRequired"] = ["KHR_draco_mesh_compression"]
check(raises(lambda: read_glb(bad.glb()), "KHR_draco_mesh_compression"), "a file needing Draco is refused, by name")
lines = Gltf()
p = lines.prim(QUAD_P, QUAD_I)
p["mode"] = 1
lines.node(mesh=lines.mesh([p]))
check(raises(lambda: read_glb(lines.glb()), "only triangles"), "lines are refused")
check(raises(lambda: read_glb(b"{}"), "glTF 2.0"), "not glTF 2.0: refused")

with tempfile.TemporaryDirectory() as tmp:
    # .gltf with a .bin and a .png next to it
    t = Gltf()
    t.node(name="sign", mesh=t.mesh([t.prim(QUAD_P, QUAD_I, t.material(name="m", pbrMetallicRoughness={
        "baseColorTexture": {"index": 0}}), uv=QUAD_UV)]))
    t.doc["images"] = [{"uri": "sign face.png"}]
    t.doc["textures"] = [{"source": 0}]
    t.doc["buffers"] = [{"uri": "sign.bin", "byteLength": len(t.bin)}]
    with open(os.path.join(tmp, "sign.bin"), "wb") as f:
        f.write(t.bin)
    with open(os.path.join(tmp, "sign face.png"), "wb") as f:
        f.write(QUAD4)
    with open(os.path.join(tmp, "sign.gltf"), "w") as f:
        json.dump(t.doc, f)
    s = read_glb(os.path.join(tmp, "sign.gltf"))
    check(s.meshes[0].triangles == 2 and s.images[0].data == QUAD4 and s.images[0].name == "sign face",
          ".gltf: its .bin and image read from next to it")

# ---- import_glb into a pack ---------------------------------------------------------------------------------------
w = PackWriter()
warn = []
out = import_glb(w, GLB, prefix="crate", merge=False, warnings=warn)
check(out == {"meshes": ["crate/crate", "crate/lid", "crate/mirror"],
              "materials": ["crate/picture", "crate/glass", "crate/glow", "crate/leaf"],
              "textures": ["crate/quad4", "crate/leaf"]}, f"import_glb: what it added, by prefix ({out})")
check(any("emissive" in x for x in warn), f"what isn't carried over is warned about ({len(warn)} warnings)")
data = w.bytes()
w2 = PackWriter()
import_glb(w2, GLB, prefix="crate", merge=False)
check(w2.bytes() == data, "the same file gives the same pack")
wp = PackWriter()
import_glb(wp, GLB, prefix="p", colors="palette", shading="flat")
check(len(wp.bytes()) > 0, "colors='palette' builds")
big = Gltf()
n = 70000
big.node(mesh=big.mesh([{"attributes": {"POSITION": big.accessor([0.0] * (3 * n), 5126, "VEC3")},
                         "indices": big.accessor(list(range(n - n % 3)), 5125, "SCALAR")}]))
check(raises(lambda: import_glb(PackWriter(), big.glb(), prefix="big"), "65535"), "over 65535 vertices: an error naming it")

# ---- in the engine ------------------------------------------------------------------------------------------------
try:
    import alia_engine as ae
except ImportError:
    ae = None
if ae is not None:
    from png_pixels import near, pixels

    e = ae.Engine(headless=True, size=(640, 360))
    e.frame.sky_top = e.frame.sky_horizon = 0x000000
    e.frame.fog_steps = 0
    e.frame.eye, e.frame.yaw, e.frame.pitch = (0.0, 0.0, 0.0), 0.0, 0.0
    at = lambda x, y, z: [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z]
    tmp = tempfile.mkdtemp()

    def shot(mesh, model):
        for _ in range(2):
            e.poll()
            e.draw(mesh, model)
            e.end_frame()
        path = os.path.join(tmp, "t.png")
        e.screenshot(path, scene_only=True)
        return pixels(path)

    a = e.load_assets(data)
    check(sorted(a.textures) == ["crate/leaf", "crate/quad4"], "the imported pack loads")
    m = e.material(a.materials["crate/picture"])
    m.shading = ae.Shading.UNLIT
    e.update_material(a.materials["crate/picture"], m)  # unlit: the texture as it is
    p = shot(a.meshes["crate/crate"], at(0, -1, 2))           # the node's +1 Y taken back
    tl, tr, bl, br = p(260, 120), p(380, 120), p(260, 240), p(380, 240)
    check(near(tl, (255, 0, 0)) and near(tr, (0, 255, 0)) and near(bl, (0, 0, 255)) and near(br, (255, 255, 255)),
          f"a glTF textured quad: the right way up and round, facing a camera at yaw 0 ({tl} {tr} {bl} {br})")
    check(e.material(a.materials["crate/leaf"]).alpha_mode == ae.AlphaMode.CUTOUT
          and e.material(a.materials["crate/leaf"]).double_sided, "MASK -> CUTOUT, doubleSided kept")
    g2 = e.material(a.materials["crate/glass"])
    check(g2.alpha_mode == ae.AlphaMode.BLEND and abs(g2.opacity - 0.5) < 1e-6 and g2.color == ae.ColorSource.VERTEX,
          "BLEND -> BLEND with the factor's alpha as opacity, vertex colours")
    check(e.material(a.materials["crate/glow"]).shading == ae.Shading.UNLIT, "KHR_materials_unlit -> UNLIT")
    # the colour: base factor 0.214 x COLOR_0 55/255 (both linear) -> display ~ 0.046 linear ~ 61
    one = Gltf()
    one.node(mesh=one.mesh([one.prim(QUAD_P, QUAD_I, one.material(name="c", extensions={"KHR_materials_unlit": {}},
                                     pbrMetallicRoughness={"baseColorFactor": [0.2140, 0.5, 1.0, 1]}))]))
    wq = PackWriter()
    import_glb(wq, one.glb(), prefix="q")
    c = shot(e.load_assets(wq.bytes()).meshes["q"], at(0, 0, 2))(320, 180)
    check(near(c, (128, 188, 255), 3), f"linear base colour -> display colour, unlit as it is ({c}, want ~128 188 255)")
    wq = PackWriter()
    import_glb(wq, one.glb(), prefix="q", colors="palette")
    c = shot(e.load_assets(wq.bytes()).meshes["q"], at(0, 0, 2))(320, 180)
    check(near(c, (170, 210, 240), 3), f"colors='palette': snapped to the nearest palette colour (sky) ({c})")

# ---- alia assets --------------------------------------------------------------------------------------------------
with tempfile.TemporaryDirectory() as root:
    os.makedirs(os.path.join(root, "pipeline", "models"))
    os.makedirs(os.path.join(root, "pipeline", "textures"))
    with open(os.path.join(root, "pipeline", "models", "crate.glb"), "wb") as f:
        f.write(GLB)
    with open(os.path.join(root, "pipeline", "textures", "floor.png"), "wb") as f:
        f.write(QUAD4)
    with open(os.path.join(root, "pipeline", "build.py"), "w") as f:
        f.write('''
def build(ctx):
    src = "pipeline/textures/floor.png"
    if ctx.stale([src], ["parts.json"]):
        w, h, rgba = ctx.read_png(f"{ctx.root}/{src}")
        ctx.write("parts.json", '{"w": %d, "h": %d}' % (w, h))
        if not ctx.check:
            open(f"{ctx.root}/built.txt", "a").write("x")
''')
    with open(os.path.join(root, "alia.toml"), "w") as f:
        f.write('''[game]
name = "t"
[assets]
build = "pipeline/build.py:build"
[[assets.pack]]
out = "assets/props.apak"
glb = ["pipeline/models/*.glb"]
png = ["pipeline/textures/*.png"]
options = { mipmaps = true }
[assets.pack.files."pipeline/textures/floor.png"]
wrap = "repeat"
''')
    run = lambda *args: alia_assets.main(["--root", root, *args])
    pack_path = os.path.join(root, "assets", "props.apak")
    check(run("--check") == 1, "--check before any build: stale")
    check(run() == 0 and os.path.isfile(pack_path) and os.path.isfile(os.path.join(root, "assets", "parts.json")),
          "alia assets builds the pack and the builder's file")
    first = open(pack_path, "rb").read()
    import alia_pack
    entries = alia_pack.read(first)
    check((alia_pack.TEXTURE, "floor") in entries and entries[(alia_pack.TEXTURE, "floor")][5] == 6
          and (alia_pack.MESH, "crate") in entries, "the pack: the glb (prefix = file name) and the png, with its options")
    mtime = os.path.getmtime(pack_path)
    check(run() == 0 and os.path.getmtime(pack_path) == mtime and open(os.path.join(root, "built.txt")).read() == "x",
          "nothing changed: nothing built again (pack untouched, the builder's stale() is False)")
    check(run("--check") == 0, "--check after a build: up to date")
    with open(os.path.join(root, "pipeline", "textures", "floor.png"), "wb") as f:
        f.write(LEAF)
    check(run("--check") == 1, "--check after a source changed: stale")
    check(run() == 0 and open(pack_path, "rb").read() != first and open(os.path.join(root, "built.txt")).read() == "xx",
          "a source changed: the pack and the builder's output made again")
    check(run("--force") == 0 and open(os.path.join(root, "built.txt")).read() == "xxx", "--force builds everything")
    stamp = json.load(open(os.path.join(root, "assets", ".alia-assets.json")))
    check(sorted(stamp["outputs"]) == ["assets/parts.json", "assets/props.apak"], "the stamp file lists every output")
    with open(os.path.join(root, "alia.toml"), "a") as f:
        f.write('bogus = 1\n')
    check(run() == 1, "an unknown key in [[assets.pack]] is an error")

sys.exit(1 if failures else 0)
