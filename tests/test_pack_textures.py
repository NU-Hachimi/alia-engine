"""Textures in resource packs (docs/textures.md T3a): texture entries, materials naming them (and their uv_rect),
e.load_assets, load_pack still giving two dicts, e.material(id) read back, mipmaps, and what a broken or older pack
does."""

import os
import struct
import sys
import tempfile
import zlib

import alia_engine as ae
import alia_kit as kit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import alia_pack  # noqa: E402
from alia_pack import PackError, PackWriter  # noqa: E402
from png_pixels import near, pixel, pixels  # noqa: E402

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


def png(w, h, fn):
    """RGBA PNG bytes; fn(x, y) -> (r, g, b, a)."""
    raw = b"".join(b"\0" + bytes(c for x in range(w) for c in fn(x, y)) for y in range(h))
    chunk = lambda kind, data: (struct.pack(">I", len(data)) + kind + data
                                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


RED, GREEN, BLUE, WHITE = (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255)
QUAD4 = png(2, 2, lambda x, y: [RED, GREEN, BLUE, WHITE][y * 2 + x])     # red top-left ... white bottom-right
SHEET = png(1, 2, lambda x, y: [RED, BLUE][y])                            # two frames: red over blue
CHECKER = png(64, 64, lambda x, y: WHITE if (x + y) % 2 else (0, 0, 0, 255))

# a 2 x 2 m quad facing -Z, uv as kit.quad's
QUAD_P = [1, -1, 0, -1, -1, 0, -1, 1, 0, 1, 1, 0]
QUAD_I = [0, 1, 2, 0, 2, 3]
QUAD_UV = [0, 1, 1, 1, 1, 0, 0, 0]


def pack(lost=False, old_material=False):
    w = PackWriter()
    w.add_texture("quad", QUAD4)
    w.add_texture("sheet", SHEET, filter="nearest", wrap="clamp")
    w.add_texture("floor", CHECKER, wrap="repeat", mipmaps=True)
    w.add_material("pic", shading="unlit", color="texture", texture="quad")
    w.add_material("frame", shading="unlit", color="texture", texture="sheet", uv_rect=(0, 0.5, 1, 1))
    w.add_material("lost", shading="unlit", color="texture", texture="quad")
    w.add_mesh("pic", QUAD_P, QUAD_I, uv=QUAD_UV, submeshes=[(0, 2, "pic")])
    w.add_mesh("frame", QUAD_P, QUAD_I, uv=QUAD_UV, submeshes=[(0, 2, "frame")])
    w.add_mesh("lost", QUAD_P, QUAD_I, uv=QUAD_UV, submeshes=[(0, 2, "lost")])
    w.add_rig("stand", {"bones": [{"name": "root", "parent": None, "mesh": "pic"}]})
    if lost:  # a material naming a texture the pack doesn't have (the writer refuses; made by hand)
        k = next(i for i, (t, n, _) in enumerate(w.entries) if t == alia_pack.MATERIAL and n == "lost")
        t, n, payload = w.entries[k]
        body = payload[:alia_pack._MATERIAL.size]
        w.entries[k] = (t, n, body + struct.pack("<H", 4) + b"nope" + struct.pack("<4f", 0, 0, 1, 1))
        w.check = lambda: None
    if old_material:  # the first version's material: no texture name, no uv_rect
        k = next(i for i, (t, n, _) in enumerate(w.entries) if t == alia_pack.MATERIAL and n == "pic")
        t, n, payload = w.entries[k]
        w.entries[k] = (t, n, payload[:alia_pack._MATERIAL.size])
    return w


# -- the writer ---------------------------------------------------------------------------------------------
check(alia_pack.image_size(QUAD4) == (2, 2) and alia_pack.image_size(CHECKER) == (64, 64), "image_size: PNG")
w = PackWriter()
try:
    w.add_texture("x", b"not an image")
    check(False, "add_texture refuses what isn't a PNG or JPEG")
except PackError:
    check(True, "add_texture refuses what isn't a PNG or JPEG")
w.add_material("m", color="texture", texture="nope")
try:
    w.bytes()
    check(False, "the writer refuses a material naming a texture the pack doesn't have")
except PackError as err:
    check("nope" in str(err), f"the writer refuses a material naming a texture the pack doesn't have ({err})")
try:
    PackWriter().add_material("m", texture="quad")
    check(False, "texture= without color='texture' is refused")
except PackError:
    check(True, "texture= without color='texture' is refused")
data = pack().bytes()
check(data == pack().bytes(), "the same input gives the same bytes")
entries = alia_pack.read(data)
check((alia_pack.TEXTURE, "quad") in entries and entries[(alia_pack.TEXTURE, "quad")].endswith(QUAD4),
      "the texture entry holds the PNG as it was given")

with tempfile.TemporaryDirectory() as tmp:
    e = ae.Engine(headless=True, size=(640, 360))
    e.frame.sky_top = e.frame.sky_horizon = 0x000000
    e.frame.fog_steps = 0
    e.frame.eye, e.frame.yaw, e.frame.pitch = (0.0, 0.0, 0.0), 0.0, 0.0
    at = lambda x, y, z: [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z]

    def shot(*draws):
        for _ in range(2):
            e.poll()
            for mesh, model in draws:
                e.draw(mesh, model)
            e.end_frame()
        path = os.path.join(tmp, "t.png")
        e.screenshot(path, scene_only=True)
        return path

    def corners(path):
        p = pixels(path)
        return p(260, 120), p(380, 120), p(260, 240), p(380, 240)

    # -- load_assets: everything, wired up ------------------------------------------------------------------
    kb0 = e.stats["texture_kb"]
    a = e.load_assets(data)
    check(sorted(a.textures) == ["floor", "quad", "sheet"] and sorted(a.materials) == ["frame", "lost", "pic"]
          and sorted(a.meshes) == ["frame", "lost", "pic"] and list(a.rigs) == ["stand"],
          f"load_assets: textures, materials, meshes and rigs by name ({a})")
    q = a.textures["quad"]
    check(q.ready and (q.width, q.height) == (2, 2), "a pack texture is ready when load_assets returns")
    # 2x2 + 1x2 + 64x64 with mipmaps (4/3): about 21.4 KB
    kb = e.stats["texture_kb"] - kb0
    check(20 <= kb <= 23, f"texture_kb counts the mip chain ({kb})")
    tl, tr, bl, br = corners(shot((a.meshes["pic"], at(0, 0, 2))))
    check(near(tl, RED[:3]) and near(tr, GREEN[:3]) and near(bl, BLUE[:3]) and near(br, WHITE[:3]),
          f"a pack material shows its pack texture, the right way up ({tl} {tr} {bl} {br})")
    c = pixel(shot((a.meshes["frame"], at(0, 0, 2))), 320, 180)
    check(near(c, BLUE[:3]), f"the pack material's uv_rect picks the second frame ({c})")

    # -- e.material(id): read back, change one field, update ----------------------------------------------
    m = e.material(a.materials["frame"])
    check(m.color == ae.ColorSource.TEXTURE and m.shading == ae.Shading.UNLIT and tuple(m.uv_rect) == (0, 0.5, 1, 1)
          and m.texture == e.material(a.materials["frame"]).texture,
          f"e.material reads a material back (uv_rect {tuple(m.uv_rect)})")
    m.uv_rect = (0, 0, 1, 0.5)
    e.update_material(a.materials["frame"], m)
    c = pixel(shot((a.meshes["frame"], at(0, 0, 2))), 320, 180)
    check(near(c, RED[:3]), f"one field changed and updated: the first frame ({c})")
    m.texture, m.uv_rect = q, (0, 0, 1, 1)
    e.update_material(a.materials["frame"], m)
    tl, tr, bl, br = corners(shot((a.meshes["frame"], at(0, 0, 2))))
    check(near(tl, RED[:3]) and near(br, WHITE[:3]), f"the read-back material takes another pack texture ({tl} {br})")
    try:
        e.material(9999)
        check(False, "e.material of no material is a ValueError")
    except ValueError:
        check(True, "e.material of no material is a ValueError")
    m0 = e.material(0)
    check(m0.color == ae.ColorSource.PALETTE, "e.material(0) is the default material")

    # -- mipmaps: a fine checker far away is grey, not noise ------------------------------------------------
    plain = e.create_texture(64, 64, bytes(b for y in range(64) for x in range(64)
                                           for b in (WHITE if (x + y) % 2 else (0, 0, 0, 255))), wrap="repeat")
    def grey_share(texture):
        mat = e.create_material(ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE, texture=texture))
        mesh = e.create_mesh(kit.quad(2, 2, mat, tiles=24))   # 1536 texels across ~330 pixels
        p = pixels(shot((mesh, at(0, 0, 2))))
        vals = [p(x, y)[0] for x in range(240, 400, 7) for y in range(110, 250, 7)]
        e.destroy_mesh(mesh)
        e.destroy_material(mat)
        return sum(40 < v < 215 for v in vals) / len(vals)
    with_mips, without = grey_share(a.textures["floor"]), grey_share(plain)
    check(with_mips > 0.9 and without < 0.5, f"mipmaps: a minified checker is grey ({with_mips:.2f} grey; "
          f"without mipmaps {without:.2f})")
    t = e.create_texture(4, 4, bytes(64), mipmaps=True)
    t.update(bytes([255] * 64))
    t.update(bytes([0] * 16), rect=(0, 0, 2, 2))
    check(t.ready, "a mipmapped texture updates (whole and a rect)")

    # -- a material naming a texture the pack doesn't have: magenta, the rest loads --------------------------
    b = e.load_assets(pack(lost=True).bytes())
    c = pixel(shot((b.meshes["lost"], at(0, 0, 2))), 320, 180)
    check(near(c, (255, 0, 255)), f"a material naming no texture of its pack draws magenta ({c})")

    # -- a material of the first version (no texture name): loads, no texture yet (magenta) ---------------------
    b = e.load_assets(pack(old_material=True).bytes())
    c = pixel(shot((b.meshes["pic"], at(0, 0, 2))), 320, 180)
    check(near(c, (255, 0, 255)) and tuple(e.material(b.materials["pic"]).uv_rect) == (0, 0, 1, 1),
          f"an older pack's textured material loads without a texture ({c})")

    # -- a broken texture: an error, and nothing of the pack is left -------------------------------------------
    w = pack()
    k = next(i for i, (t, n, _) in enumerate(w.entries) if t == alia_pack.TEXTURE and n == "sheet")
    t, n, payload = w.entries[k]
    w.entries[k] = (t, n, payload[:alia_pack._TEXTURE.size] + b"\x89PNG\r\n\x1a\n" + bytes(len(SHEET) - 8))
    kb0 = e.stats["texture_kb"]
    try:
        e.load_assets(w.bytes())
        check(False, "a texture that doesn't decode: RuntimeError")
    except RuntimeError as err:
        check("sheet" in str(err) and e.stats["texture_kb"] == kb0,
              f"a texture that doesn't decode: RuntimeError, its other textures freed ({err})")

    # -- load_pack: the two dicts as before, the textures wired, the rigs not loaded ----------------------------
    path = os.path.join(tmp, "p.apak")
    pack().write(path)
    meshes, materials = e.load_pack(path)
    tl, tr, bl, br = corners(shot((meshes["pic"], at(0, 0, 2))))
    check(sorted(meshes) == ["frame", "lost", "pic"] and near(tl, RED[:3]) and near(br, WHITE[:3]),
          "load_pack: (meshes, materials) as before, its materials with their textures")

sys.exit(1 if failures else 0)
