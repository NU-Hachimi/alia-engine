"""The camera (docs/camera.md) against a fake camera service speaking the SDK's protocol v2
(third_party/alia-sdk/alia/camera_proto.h): headless, frames copied (a memfd is no dmabuf), so this
checks the protocol, the states, developing (colours, which way up), the background, the HUD box and
photos; the zero-copy import is checked on the handheld."""

import os
import select
import socket
import struct
import sys
import tempfile
import threading
import time

import alia_engine as ae

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from png_pixels import near, pixel  # noqa: E402

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


# The protocol's messages (camera_proto.h; sizes asserted there).
HELLO, OPEN, RELEASE, CLOSE, PHOTO = 1, 2, 3, 4, 5
SERVER_HELLO, MODE, BUFFER, FRAME, PARAMS, PENDING, PAUSED, RESUMED, PHOTO_DONE, ERROR = range(16, 26)
DENIED, REVOKED = 1, 2
MODE_S = struct.Struct("<6I Q 3I 4s 2I 9f I")
FRAME_S = struct.Struct("<4I Q I 4f I")
assert MODE_S.size == 96 and FRAME_S.size == 48

W, H, STRIDE, NBUF, BLACK = 320, 240, 320, 3, 16
# the photo: a real 32x16 JPEG, the left half red (220, 40, 40), the right half blue (40, 60, 220)
from test_textures_data import JPEG, RED, BLUE  # noqa: E402


def shared_memory(name, data):
    """A memfd, or where this Python has none (some builds) an unlinked temporary file: both mmap."""
    if hasattr(os, "memfd_create"):
        fd = os.memfd_create(name)
    else:
        fd, tmp = tempfile.mkstemp()
        os.unlink(tmp)
    os.write(fd, data)
    return fd


def pattern():
    """RGGB: the top-left quarter coloured (R 200, G 100, B 50), the rest dark (30)."""
    rows = []
    for y in range(H):
        row = bytearray(STRIDE)
        for x in range(W):
            if x < W // 2 and y < H // 2:
                row[x] = 200 if (x % 2 == 0 and y % 2 == 0) else 50 if (x % 2 and y % 2) else 100
            else:
                row[x] = 30
        rows.append(bytes(row))
    return b"".join(rows)


class FakeCamera(threading.Thread):
    def __init__(self, path, pending=0.0, deny=False, revoke_after=None, turn=0):
        super().__init__(daemon=True)
        self.path, self.pending, self.deny, self.revoke_after, self.turn = path, pending, deny, revoke_after, turn
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.server.bind(path)
        self.server.listen(1)
        self.held, self.released, self.sent, self.opened, self.closed = set(), 0, 0, None, False
        self.stop = False
        self.start()

    def send(self, data, fd=None):
        try:
            if fd is None:
                self.conn.send(data)
            else:
                socket.send_fds(self.conn, [data], [fd])
        except (BrokenPipeError, ConnectionResetError):  # the client hung up
            self.closed = True

    def mode(self):
        self.send(MODE_S.pack(MODE, W, H, STRIDE, 0, 0, 0, 60, NBUF, self.turn, b"RGGB", BLACK, 30000,
                              300, 300, 160, 120, 0, 0, 0, 0, 0, 0))
        image = pattern()
        for i in range(NBUF):
            fd = shared_memory(f"frame{i}", image)
            self.send(struct.pack("<3I", BUFFER, i, len(image)), fd)
            os.close(fd)
        self.held.clear()
        # identity colour (ccm 1, shading 1, tone linear): what comes out is (v - black) / (255 - black)
        ccm = [1, 0, 0, 0, 1, 0, 0, 0, 1]
        shading = [1.0] * (12 * 16 * 3)
        tone = [i / 1023 for i in range(1024)]
        self.send(struct.pack("<2I 9f 576f 1024f", PARAMS, 1, *ccm, *shading, *tone))

    def run(self):
        self.conn, _ = self.server.accept()
        streaming, seq, start, photo = False, 0, None, None
        while not self.stop:
            r, _, _ = select.select([self.conn], [], [], 0.005)
            if r:
                try:
                    msg = self.conn.recv(256)
                except ConnectionResetError:  # hung up with frames unread: the protocol's other way to close
                    msg = b""
                if not msg:
                    self.closed = True
                    break
                kind = struct.unpack_from("<I", msg)[0]
                if kind == HELLO:
                    self.send(struct.pack("<2I", SERVER_HELLO, 2))
                elif kind == OPEN:
                    self.opened = struct.unpack("<4I", msg)[1:]
                    if self.pending:
                        self.send(struct.pack("<I", PENDING))
                        time.sleep(self.pending)
                    if self.deny:
                        self.send(struct.pack("<3I116s", ERROR, DENIED, 0, b"the user said no"))
                        break
                    self.mode()
                    streaming, start = True, time.time()
                elif kind == RELEASE:
                    self.held.discard(struct.unpack("<2I", msg)[1])
                    self.released += 1
                elif kind == PHOTO:
                    photo = struct.unpack("<2I", msg)[1]
                elif kind == CLOSE:
                    self.closed = True
                    break
            if self.closed:
                break
            if photo is not None:  # the sensor changes mode for the photo: PAUSED, a new MODE, RESUMED
                self.send(struct.pack("<2I", PAUSED, 1))
                time.sleep(0.05)
                self.mode()
                self.send(struct.pack("<I", RESUMED))
                fd = shared_memory("photo", JPEG)
                self.send(struct.pack("<3I", PHOTO_DONE, photo, len(JPEG)), fd)
                os.close(fd)
                photo = None
            if streaming and self.revoke_after and time.time() - start > self.revoke_after:
                self.send(struct.pack("<3I116s", ERROR, REVOKED, 0, b"turned off in settings"))
                break
            if streaming and len(self.held) < 2 and not self.closed:  # max 2 held; a client that keeps them gets fewer frames
                free = [i for i in range(NBUF) if i not in self.held]
                index = free[seq % len(free)]
                seq += 1
                self.held.add(index)
                self.send(FRAME_S.pack(FRAME, index, seq, 1, time.monotonic_ns(), 8000, 1.0, 1.0, 1.0, 5000.0, 0))
                self.sent += 1
                time.sleep(1 / 120)
        time.sleep(0.05)
        self.conn.close()
        self.server.close()


def frames(e, n, then=None):
    for _ in range(n):
        e.poll()
        if then:
            then(e)
        e.end_frame()
        time.sleep(0.005)


expect = tuple(round((v - BLACK) / (255 - BLACK) * 255) for v in (200, 100, 50))
dark = tuple(round((30 - BLACK) / (255 - BLACK) * 255) for _ in range(3))

with tempfile.TemporaryDirectory() as tmp:
    os.environ["ALIA_CAMERA_SOCKET"] = path = os.path.join(tmp, "camera.sock")
    e = ae.Engine(headless=True, size=(640, 360))
    cam = e.lens
    check(cam.state == "closed" and cam.size is None and cam.photo().error, "closed: no image; a photo fails at once")

    # no service: unavailable, the background and the box black
    cam.open()
    check(cam.state == "unavailable" and "no camera service" in cam.message, "no service: unavailable")
    e.frame.lens_background = "cover"
    frames(e, 2, lambda e: e.hud_lens(10, 10, 50, 50))
    shot = os.path.join(tmp, "none.png")
    e.screenshot(shot, scene_only=True)
    check(pixel(shot, 320, 180) == (0, 0, 0) and pixel(shot, 30, 30) == (0, 0, 0),
          "no image: the background and the HUD box are black")
    cam.close()

    # the user is asked first (pending), then frames: developed, the right way up, buffers handed back
    fake = FakeCamera(path, pending=0.2)
    cam.open()
    frames(e, 3)
    check(cam.state == "pending", f"asking the user: pending ({cam.state})")
    deadline = time.time() + 5
    while cam.state != "running" and time.time() < deadline:
        frames(e, 1)
    frames(e, 20)
    check(cam.state == "running" and fake.opened == (640, 360, 1), f"running; OPEN asked for the scene's size ({fake.opened})")
    check(cam.size == (320, 240) and cam.mode.width == W and cam.mode.order == "RGGB" and not cam.mode.measured,
          f"the image: the camera's 4:3, at most the frame's size ({cam.size}); the mode")
    check(cam.frame and cam.frame.sequence > 0 and cam.frame.exposure_us == 8000, "the newest frame's metadata")
    check(fake.released >= 5 and len(fake.held) <= 2, f"buffers handed back ({fake.released} released)")
    shot = os.path.join(tmp, "cover.png")
    e.screenshot(shot, scene_only=True)
    # cover: the 4:3 image scaled to 640x480, the middle 360 rows shown; the coloured quarter is the top-left
    check(near(pixel(shot, 100, 60), expect) and near(pixel(shot, 500, 300), dark),
          f"cover: developed colours, top-left up ({pixel(shot, 100, 60)} ~ {expect}; {pixel(shot, 500, 300)} ~ {dark})")
    e.frame.lens_background = "fit"
    frames(e, 2)
    e.screenshot(shot, scene_only=True)
    check(pixel(shot, 40, 180) == (0, 0, 0) and near(pixel(shot, 150, 60), expect),
          "fit: black bars at the sides, the image between")
    e.frame.lens_background = "off"
    frames(e, 2, lambda e: e.hud_lens(0, 0, 160, 120))
    e.screenshot(shot, scene_only=True)
    check(near(pixel(shot, 40, 30), expect) and near(pixel(shot, 120, 90), dark), "hud_lens: the image in a box")
    frames(e, 2, lambda e: e.hud_image(cam.texture, 0, 0, 160, 120))
    e.screenshot(shot, scene_only=True)
    check(cam.texture.ready and cam.texture.width == 320 and near(pixel(shot, 40, 30), expect),
          "the camera's texture in hud_image, the same as hud_lens")
    import alia_kit as kit
    tv = e.create_mesh(kit.quad(4, 3, e.create_material(ae.Material(shading=ae.Shading.UNLIT, color=ae.ColorSource.TEXTURE,
                                                                     texture=cam.texture))))
    e.frame.eye, e.frame.yaw, e.frame.pitch, e.frame.lens_background = (0.0, 0.0, 0.0), 0.0, 0.0, "off"
    frames(e, 2, lambda e: e.draw(tv, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 2.4]))
    e.screenshot(shot, scene_only=True)
    check(near(pixel(shot, 200, 100), expect, 10) and near(pixel(shot, 420, 250), dark, 10),
          f"the camera on a material (a TV screen): its coloured corner top-left ({pixel(shot, 200, 100)})")
    e.frame.lens_background, e.frame.eye = "cover", (0.0, 2.0, 0.0)  # back as it was

    # a photo: PAUSED while the sensor changes mode, a new MODE (imports redone), RESUMED, the JPEG
    shot_ = cam.photo()
    states = set()
    deadline = time.time() + 5
    while not shot_.done and time.time() < deadline:
        frames(e, 1)
        states.add(cam.state)
    check(shot_.jpeg == JPEG and shot_.error is None, "photo: the JPEG bytes")
    check("paused" in states, f"photo: paused on the way ({sorted(states)})")
    frames(e, 10)
    check(cam.state == "running", "after the photo: running again (new buffers)")
    inflight = cam.photo()
    check(inflight.done is False, "a photo while open is in flight, not failed")
    deadline = time.time() + 5
    while not inflight.done and time.time() < deadline:
        frames(e, 1)

    # photo mode: the frame rendered again at the size asked for, the photo in place of the live image
    def composite(**kw):
        p = cam.photo(**kw)
        deadline = time.time() + 10
        while not p.done and time.time() < deadline:
            frames(e, 1, lambda e: e.hud_rect(0, 0, 40, 40, 0x00FF00FF))  # the HUD: a green box top-left
        return p
    def look(jpeg, at):
        """Pixels of a JPEG, drawn by the engine (no Pillow in the runtime image): at = [(x, y) in 0..1]."""
        t = e.load_texture(jpeg, filter="nearest")
        e.poll()
        e.hud_image(t, 0, 0, 640, 360)
        e.end_frame()
        path = os.path.join(tmp, "look.png")
        e.screenshot(path, scene_only=True)
        size = (t.width, t.height)
        t.destroy()
        return size, [pixel(path, int(x * 639), int(y * 359)) for x, y in at]
    e.frame.lens_background = "cover"
    p = composite(scene=False, hud=True, size=(320, 180))
    check(p.jpeg and p.jpeg[:2] == b"\xff\xd8", f"photo mode: a JPEG ({p.error})")
    if p.jpeg:
        size, (box, left, right) = look(p.jpeg, [(0.05, 0.08), (0.3, 0.6), (0.8, 0.6)])
        check(size == (320, 180), f"photo mode: the size asked for ({size})")
        check(near(box, (0, 255, 0), 40) and near(left, RED, 40) and near(right, BLUE, 40),
              f"photo mode: the photo fills it, the HUD on top ({box} {left} {right})")
    e.frame.lens_background = "off"
    e.frame.sky_top = e.frame.sky_horizon = 0x804020
    e.frame.fog_steps = 0
    import alia_kit as kit
    box = e.world.spawn(e.create_mesh(kit.box(2, 2, 2, kit.pal.WHITE)), position=(0, 2, 6))
    p = composite(scene=True, hud=False, size=(320, 180))
    if p.jpeg:
        size, (corner, middle) = look(p.jpeg, [(0.05, 0.08), (0.5, 0.5)])
        check(near(corner, (0x80, 0x40, 0x20), 40) and not near(middle, (0x80, 0x40, 0x20), 60),
              f"photo mode, scene without the HUD: the sky and the 3D box as on screen, no HUD box ({corner} {middle})")
    else:
        check(False, f"photo mode, scene: {p.error}")
    p = cam.photo()
    deadline = time.time() + 5
    while not p.done and time.time() < deadline:
        frames(e, 1)
    check(p.jpeg == JPEG, "the photo alone: the camera's JPEG as it is")
    box.destroy()

    cam.close()
    fake.join(2)
    check(fake.closed and cam.state == "closed", "close: the service sees CLOSE (or the hang-up), closed")

    # denied
    fake = FakeCamera(path + "2", pending=0.05, deny=True)
    os.environ["ALIA_CAMERA_SOCKET"] = path + "2"
    cam.open()
    deadline = time.time() + 5
    while cam.state in ("starting", "pending") and time.time() < deadline:
        frames(e, 1)
    check(cam.state == "denied" and "no" in cam.message, f"denied: {cam.state} ({cam.message})")
    cam.close()

    # revoked while streaming: the image goes at once
    fake = FakeCamera(path + "3", revoke_after=0.3)
    os.environ["ALIA_CAMERA_SOCKET"] = path + "3"
    cam.open()
    deadline = time.time() + 5
    while cam.state != "revoked" and time.time() < deadline:
        frames(e, 1)
    check(cam.state == "revoked" and cam.size is None, "revoked: stopped, no image")
    cam.close()

    # mounted a quarter turn off: the image turned upright, clockwise (the raw top-left ends up top-right)
    fake = FakeCamera(path + "4", turn=90)
    os.environ["ALIA_CAMERA_SOCKET"] = path + "4"
    cam.open()
    deadline = time.time() + 5
    while cam.state != "running" and time.time() < deadline:
        frames(e, 1)
    frames(e, 3, lambda e: e.hud_lens(0, 0, 120, 160))
    shot = os.path.join(tmp, "turn.png")
    e.screenshot(shot, scene_only=True)
    check(cam.size == (240, 320) and near(pixel(shot, 90, 40), expect) and near(pixel(shot, 30, 40), dark)
          and near(pixel(shot, 90, 120), dark), f"turn 90: {cam.size}, the coloured corner top-right")
    cam.close()

    # half the frame's size or less: one colour per Bayer cell (the SDK's cheaper path), the same colours
    fake = FakeCamera(path + "5")
    os.environ["ALIA_CAMERA_SOCKET"] = path + "5"
    cam.open(160, 120)
    deadline = time.time() + 5
    while cam.state != "running" and time.time() < deadline:
        frames(e, 1)
    frames(e, 3, lambda e: e.hud_lens(0, 0, 160, 120))
    e.screenshot(shot, scene_only=True)
    check(cam.size == (160, 120) and near(pixel(shot, 40, 30), expect) and near(pixel(shot, 120, 90), dark),
          f"at half size: {cam.size}, per-cell colours the same")
    cam.close()
    e.close()
    del cam, shot_, e

sys.exit(1 if failures else 0)
