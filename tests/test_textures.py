"""Textures (docs/textures.md, step T1): PNG / JPEG from bytes or the cart's files, scaled down, made from RGBA,
updated, loaded in the background, drawn with hud_image (crop, fit, a part, tint, transparency), destroyed."""

import base64
import os
import struct
import sys
import tempfile
import time
import zlib

import alia_engine as ae

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_pixels import near, pixel  # noqa: E402

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


def png(w, h, rgba):
    """A PNG of RGBA8 bytes (no filter)."""
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


# 32x16 JPEG: the left half red (220, 40, 40), the right half blue (40, 60, 220)
JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsK"
    "DAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAQACAD"
    "ASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKB"
    "kaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZ"
    "mqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQF"
    "BgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5"
    "OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX"
    "2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD53ooor8HP9aDyeiiiv9xD/Ds//9k=")
RED, BLUE = (220, 40, 40), (40, 60, 220)

with tempfile.TemporaryDirectory() as tmp:
    e = ae.Engine(headless=True, size=(640, 360))
    e.frame.sky_top = e.frame.sky_horizon = 0x000000
    e.frame.fog_steps = 0

    def shot(draw):
        e.poll()
        draw()
        e.end_frame()
        e.poll()
        draw()
        e.end_frame()
        path = os.path.join(tmp, "t.png")
        e.screenshot(path, scene_only=True)
        return path

    j = e.load_texture(JPEG)
    check((j.width, j.height) == (32, 16) and j.ready and j.done and j.error is None, "a JPEG from bytes")
    p = shot(lambda: e.hud_image(j, 0, 0, 64, 32))
    check(near(pixel(p, 10, 16), RED, 12) and near(pixel(p, 50, 16), BLUE, 12), "hud_image: the JPEG, the right way round")

    # crop to fill (default) vs fit; a part (src); a tint
    p = shot(lambda: e.hud_image(j, 0, 0, 64, 64))  # 2:1 into 1:1: the middle half, red | blue split in the middle
    check(near(pixel(p, 16, 32), RED, 12) and near(pixel(p, 48, 32), BLUE, 12), "cropped to fill the box")
    p = shot(lambda: e.hud_image(j, 0, 0, 64, 64, fit=True))  # all of it, 64x32 in the middle
    check(pixel(p, 32, 8) == (0, 0, 0) and near(pixel(p, 5, 32), RED, 12), "fit: all of it, centred")
    p = shot(lambda: e.hud_image(j, 0, 0, 64, 32, src=(0.5, 0, 1, 1)))
    check(near(pixel(p, 5, 16), BLUE, 12) and near(pixel(p, 60, 16), BLUE, 12), "src: only the right half")
    p = shot(lambda: e.hud_image(j, 0, 0, 64, 32, color=0x00FF00FF))
    check(pixel(p, 50, 16)[0] < 5 and pixel(p, 50, 16)[2] < 5 and pixel(p, 50, 16)[1] > 40, "color multiplies it")

    # PNG with transparency, from a file in the cart's content (ALIA_CART_DIR/content)
    os.makedirs(os.path.join(tmp, "cart", "content", "art"))
    rgba = bytes([255, 255, 0, 255] * 8 + [0, 0, 0, 0] * 8) * 16  # 16x16: the left half yellow, the right clear
    open(os.path.join(tmp, "cart", "content", "art", "half.png"), "wb").write(png(16, 16, rgba))
    os.environ["ALIA_CART_DIR"] = os.path.join(tmp, "cart")
    t = e.load_texture("art/half.png")
    p = shot(lambda: (e.hud_rect(0, 0, 64, 64, 0x2040A0FF), e.hud_image(t, 0, 0, 64, 64)))
    check(near(pixel(p, 10, 30), (255, 255, 0)) and near(pixel(p, 50, 30), (0x20, 0x40, 0xA0)),
          "a PNG from the cart's files; its transparent half shows what's behind")
    check(t.width == 16 and e.stats["texture_kb"] >= 2, f"stats: texture_kb ({e.stats['texture_kb']})")

    # made from RGBA, nearest by default (sharp), updated
    c = e.create_texture(2, 1, bytes([255, 0, 0, 255, 0, 255, 0, 255]))
    p = shot(lambda: e.hud_image(c, 0, 0, 64, 32))
    check(near(pixel(p, 30, 16), (255, 0, 0)) and near(pixel(p, 33, 16), (0, 255, 0)), "create_texture; nearest: a sharp edge")
    c.update(bytes([0, 0, 255, 255, 255, 255, 255, 255]))
    p = shot(lambda: e.hud_image(c, 0, 0, 64, 32))
    check(near(pixel(p, 10, 16), (0, 0, 255)) and near(pixel(p, 50, 16), (255, 255, 255)), "update: new pixels")
    try:
        c.update(b"short")
        check(False, "update: the wrong size is an error")
    except ValueError:
        check(True, "update: the wrong size is an error")

    # big images are scaled down; the background loader
    big = e.load_texture(png(400, 100, bytes([10, 200, 10, 255]) * 40000), max_size=64)
    check((big.width, big.height) == (64, 16), f"max_size: 400x100 -> {big.width}x{big.height}")
    a = e.load_texture_async(JPEG, filter="linear")
    deadline = time.time() + 5
    while not a.done and time.time() < deadline:
        e.poll()
        e.end_frame()
    check(a.ready and a.width == 32, "load_texture_async: done after a few frames, ready")
    bad = e.load_texture_async(b"not an image")
    while not bad.done and time.time() < deadline:
        e.poll()
        e.end_frame()
    check(bad.done and not bad.ready and "PNG or JPEG" in (bad.error or ""), f"a bad image in the background: .error ({bad.error})")
    try:
        e.load_texture(b"nope")
        check(False, "a bad image: an error")
    except RuntimeError:
        check(True, "a bad image: an error")

    # destroyed: draws nothing; the camera's texture without a camera
    j.destroy()
    p = shot(lambda: e.hud_image(j, 0, 0, 64, 32))
    check(pixel(p, 10, 16) == (0, 0, 0) and j.error == "destroyed", "destroyed: nothing drawn")
    ct = e.lens.texture
    check(not ct.ready and ct.width == 0, "the camera's texture: not ready without a camera")
    e.close()
    del e, j, t, c, big, a, bad, ct

sys.exit(1 if failures else 0)
