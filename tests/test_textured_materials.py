"""Textured materials (docs/textures.md T2): uv on meshes and in packs, ColorSource.TEXTURE (the texture lit like
any colour, the right way up), tints multiplying it, CUTOUT, BLEND with the texture's alpha, repeat tiling,
swapping a material's texture, and a texture that isn't there (white)."""

import os
import sys
from array import array
import tempfile

import alia_engine as ae
import alia_kit as kit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from alia_pack import PackWriter  # noqa: E402
from png_pixels import near, pixel  # noqa: E402

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


def rgba(w, h, fn):
    return bytes(b for y in range(h) for x in range(w) for b in fn(x, y))


RED, GREEN, BLUE, WHITE = (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 255)
# 2x2: red top-left, green top-right, blue bottom-left, white bottom-right
QUAD4 = rgba(2, 2, lambda x, y: [RED, GREEN, BLUE, WHITE][y * 2 + x])

with tempfile.TemporaryDirectory() as tmp:
    e = ae.Engine(headless=True, size=(640, 360))
    e.frame.sky_top = e.frame.sky_horizon = 0x000000
    e.frame.fog_steps = 0
    e.frame.eye, e.frame.yaw, e.frame.pitch = (0.0, 0.0, 0.0), 0.0, 0.0

    def shot(*draws):
        for _ in range(2):
            e.poll()
            for mesh, model, *rest in draws:
                e.draw(mesh, model, *rest)
            e.end_frame()
        path = os.path.join(tmp, "t.png")
        e.screenshot(path, scene_only=True)
        return path

    at = lambda x, y, z: [1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z]
    tex = e.create_texture(2, 2, QUAD4)
    unlit = e.create_material(ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE, texture=tex))
    q = e.create_mesh(kit.quad(2, 2, unlit))
    # the quad fills the middle of the view at 2 m: screen x 320 +- ~166 px (fov 75 deg)
    p = shot((q, at(0, 0, 2)))
    tl, tr, bl, br = pixel(p, 260, 120), pixel(p, 380, 120), pixel(p, 260, 240), pixel(p, 380, 240)
    check(near(tl, RED[:3]) and near(tr, GREEN[:3]) and near(bl, BLUE[:3]) and near(br, WHITE[:3]),
          f"TEXTURE, unlit: the texture as it is, the right way up and round ({tl} {tr} {bl} {br})")
    p = shot((q, at(0, 0, 2), 6))  # tint 6: the default palette's red (220, 60, 50)
    check(near(pixel(p, 380, 240), (220, 60, 50), 8) and pixel(p, 380, 120)[0] < 8,
          f"a tint multiplies the texture ({pixel(p, 380, 240)} {pixel(p, 380, 120)})")

    lit = e.create_material(ae.Material(color=ae.ColorSource.TEXTURE, texture=tex))
    ql = e.create_mesh(kit.quad(2, 2, lit))
    p = shot((ql, at(0, 0, 2)))
    w = pixel(p, 380, 240)
    check(0 < w[0] < 255 and w[0] == w[1] == w[2], f"TEXTURE, lit: shaded like any colour ({w})")

    # cutout: the transparent half isn't drawn (what's behind shows), blend: half the colour
    half = e.create_texture(2, 1, bytes([255, 255, 0, 255, 255, 255, 0, 0]))  # left yellow, right clear
    back = e.create_mesh(kit.quad(4, 4, e.create_material(ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE,
                                                                       texture=e.create_texture(1, 1, bytes(BLUE))))))
    cut = e.create_mesh(kit.quad(2, 2, e.create_material(ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE,
                                                                     texture=half, alpha_mode=ae.AlphaMode.CUTOUT))))
    p = shot((back, at(0, 0, 3)), (cut, at(0, 0, 2)))
    check(near(pixel(p, 260, 180), (255, 255, 0)) and near(pixel(p, 380, 180), (0, 0, 255)),
          f"CUTOUT: the clear half shows what's behind ({pixel(p, 260, 180)} {pixel(p, 380, 180)})")
    glass = e.create_mesh(kit.quad(2, 2, e.create_material(ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE,
                                                                       texture=e.create_texture(1, 1, bytes([255, 0, 0, 128])),
                                                                       alpha_mode=ae.AlphaMode.BLEND))))
    p = shot((back, at(0, 0, 3)), (glass, at(0, 0, 2)))
    check(near(pixel(p, 320, 180), (128, 0, 127), 8), f"BLEND: the texture's alpha ({pixel(p, 320, 180)})")

    # repeat: 3 tiles across; clamp: the edge after 1
    stripes = rgba(2, 1, lambda x, y: [RED, BLUE][x])
    rep = e.create_texture(2, 1, stripes, wrap="repeat")
    tiled = e.create_mesh(kit.quad(2, 2, e.create_material(ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE,
                                                                       texture=rep)), tiles=(3, 1)))
    p = shot((tiled, at(0, 0, 2)))
    row = [pixel(p, x, 180) for x in range(160, 480, 4)]
    changes = sum(1 for a, b in zip(row, row[1:]) if (a[0] > 128) != (b[0] > 128))
    check(changes >= 5, f"wrap='repeat': 3 tiles, 6 stripes ({changes + 1} runs)")

    # a material's texture swapped with update_material; a destroyed / unset texture: white
    e.update_material(unlit, ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE, texture=rep))
    p = shot((q, at(0, 0, 2)))
    check(near(pixel(p, 260, 120), RED[:3]) and near(pixel(p, 380, 120), BLUE[:3]), "update_material: the new texture")
    gone = e.create_texture(1, 1, bytes(RED))
    m = e.create_mesh(kit.quad(2, 2, e.create_material(ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE, texture=gone))))
    gone.destroy()
    p = shot((m, at(0, 0, 2)))
    check(near(pixel(p, 320, 180), (255, 255, 255)), "a destroyed texture: white")
    try:
        e.create_mesh(ae.Mesh(array("f", [0, 0, 0, 1, 0, 0, 0, 1, 0]).tobytes(), array("H", [0, 1, 2]).tobytes(),
                              submeshes=[(0, 1, unlit)]))
        check(False, "a textured material without uv: an error")
    except (ValueError, RuntimeError):
        check(True, "a textured material without uv: an error")

    # packs: uv and a textured material, the texture set by the game
    w_ = PackWriter()
    w_.add_material("pic", shading="unlit", color="texture", alpha_mode="cutout")
    w_.add_mesh("card", [1, -1, 0, -1, -1, 0, -1, 1, 0, 1, 1, 0], [0, 1, 2, 0, 2, 3], uv=[0, 1, 1, 1, 1, 0, 0, 0],
                submeshes=[(0, 2, "pic")])
    path = os.path.join(tmp, "p.apak")
    open(path, "wb").write(w_.bytes())
    meshes, mats = e.load_pack(path)
    e.update_material(mats["pic"], ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE, texture=tex,
                                               alpha_mode=ae.AlphaMode.CUTOUT))
    p = shot((meshes["card"], at(0, 0, 2)))
    check(near(pixel(p, 260, 120), RED[:3]) and near(pixel(p, 380, 240), WHITE[:3]), "a pack mesh with uv, its textured material")
    e.close()
    del e, tex, half, rep, gone

sys.exit(1 if failures else 0)
