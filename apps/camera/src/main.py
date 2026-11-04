"""alia-engine camera test (docs/camera.md): the camera's image behind a 3D scene, a viewfinder box, photos,
and the camera's state, mode and frame rate. Run from apps/camera/ (docs/project.md):

    alia cart && alia install                    as a cart on the board (the camera needs the handheld)
    alia run                                     on the PC: no camera service there yet ("unavailable")

Controls:
    A (east)       take a photo (saved in the cart's saves as photo-N.jpg)
    Y (west)       what goes into the photo: the camera only / + the 3D scene / + the scene and the HUD (photo mode)
    B (south)      background: cover / fit / off (with off, a viewfinder box instead)
    START          close the camera / open it again
    SELECT         a screenshot of what the game draws (screen-N.png in the cart's saves; photos hold only the camera)
    X (north)      the album: the photos and screenshots (src/album.py)
    left stick     turn the cube
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "python"))  # the cart's engine

import alia_engine as ae  # noqa: E402
import alia_kit as kit  # noqa: E402
from album import Album  # noqa: E402

WHITE, DIM, SHADOW = 0xFFFFFFFF, 0xC0C8D0FF, 0x000000C0
BACKGROUNDS = ("cover", "fit", "off")
PHOTO_KINDS = (("camera only", False, False), ("camera + scene", True, False), ("camera + scene + HUD", True, True))


class Game:
    def init(self, e):
        self.cube = e.world.spawn(e.create_mesh(kit.box(1.2, 1.2, 1.2, kit.pal.ORANGE)), position=(0, 2, 5))
        e.world.camera.position, e.world.camera.yaw = (0, 2, 0), 0.0
        self.background = 0
        # carry on after what's saved already (a relaunch must not write over photo-1)
        def last(prefix):
            nums = [int(n[len(prefix):].split(".")[0]) for n in ae.save.names()
                    if n.startswith(prefix) and n[len(prefix):].split(".")[0].isdigit()]
            return max(nums, default=0)
        self.photos, self.screens, self.shot, self.note = last("photo-"), last("screen-"), None, ""
        self.want_screen = False
        self.album = None
        self.kind = 0
        self.rate, self.last_seq, self.last_t = 0.0, None, time.monotonic()
        e.lens.open()

    def fixed_update(self, e, dt):
        i = e.input
        if self.album:
            if not self.album.step(i):
                self.album = None
            return
        if i.pressed & ae.NORTH:
            self.album = Album(e)
            return
        self.cube.yaw += (0.6 + i.lx * 3) * dt
        self.cube.pitch += i.ly * 3 * dt
        cam = e.lens
        if i.pressed & ae.SOUTH:
            self.background = (self.background + 1) % len(BACKGROUNDS)
        if i.pressed & ae.START:
            if cam.state == "closed":
                cam.open()
            else:
                cam.close()
        if i.pressed & ae.WEST:
            self.kind = (self.kind + 1) % len(PHOTO_KINDS)
            self.note = f"photos: {PHOTO_KINDS[self.kind][0]}"
        if i.pressed & ae.SELECT:
            self.want_screen = True
        if i.pressed & ae.EAST and self.shot is None:
            _, scene, hud = PHOTO_KINDS[self.kind]
            self.shot, self.note = cam.photo(scene=scene, hud=hud), "taking a photo..."
        if self.shot is not None and self.shot.done:
            if self.shot.jpeg:
                self.photos += 1
                name = f"photo-{self.photos}.jpg"
                ae.save.write(name, self.shot.jpeg)
                self.note = f"saved {name} ({len(self.shot.jpeg) // 1024} KiB)"
            else:
                self.note = f"photo failed: {self.shot.error}"
            self.shot = None
        e.frame.lens_background = BACKGROUNDS[self.background]
        # the camera's frame rate, from its sequence numbers
        now = time.monotonic()
        if now - self.last_t >= 1.0:
            seq = cam.frame.sequence if cam.frame else None
            if seq is not None and self.last_seq is not None:
                self.rate = (seq - self.last_seq) / (now - self.last_t)
            self.last_seq, self.last_t = seq, now

    def render(self, e, alpha):
        if self.album:
            self.album.update()
            self.album.render()
            return
        cam = e.lens
        w, h = e.scene_size
        if BACKGROUNDS[self.background] == "off":
            e.hud_lens(w - 170, 30, 160, 120)
            e.hud_rect(w - 171, 29, 162, 1, WHITE); e.hud_rect(w - 171, 150, 162, 1, WHITE)
            e.hud_rect(w - 171, 29, 1, 122, WHITE); e.hud_rect(w - 10, 29, 1, 122, WHITE)
        e.hud_rect(0, 0, w, 24, 0x00000090)
        m = cam.mode
        mode = f"{m.width}x{m.height} {m.order} turn {m.turn}" if m else "-"
        size = "%dx%d" % cam.size if cam.size else "-"
        e.hud_text(4, 3, f"CAMERA {cam.state.upper()}  MODE {mode}  IMAGE {size}  {self.rate:4.1f} FPS  "
                         f"{'ZERO COPY' if cam.zero_copy else 'COPIED'}", WHITE, 1, SHADOW)
        e.hud_text(4, 13, cam.message or f"background {BACKGROUNDS[self.background]}   photos: {PHOTO_KINDS[self.kind][0]}", DIM, 1)
        e.hud_text(4, h - 22, self.note, WHITE, 1, SHADOW)
        e.hud_text(4, h - 11, "A: PHOTO  Y: PHOTO KIND  B: BACKGROUND  X: ALBUM  START: CAMERA  SELECT: SCREEN", DIM, 1, SHADOW)
        if self.want_screen:  # the last frame shown (this one isn't drawn yet), into the saves
            self.want_screen = False
            self.screens += 1
            name = f"screen-{self.screens}.png"
            path = os.path.join(tempfile.gettempdir(), name)
            try:
                e.screenshot(path, scene_only=True)
                with open(path, "rb") as f:
                    ae.save.write(name, f.read())
                os.remove(path)
                self.note = f"saved {name}"
            except (OSError, RuntimeError) as err:
                self.note = f"screenshot failed: {err}"


if __name__ == "__main__":
    ae.run(Game(), argv=sys.argv[1:], title="alia-engine camera test", app_id="pad.alia.engine.camera")
