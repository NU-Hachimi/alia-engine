"""alia-engine camera test (docs/camera.md): the camera's image behind a 3D scene, a viewfinder box, photos,
and the camera's state, mode and frame rate. Run from apps/camera/ (docs/project.md):

    alia cart && alia install                    as a cart on the board (the camera needs the handheld)
    alia run                                     on the PC: no camera service there yet ("unavailable")

Controls:
    A (east)       take a photo (saved in the cart's saves as photo-N.jpg)
    B (south)      background: cover / fit / off (with off, a viewfinder box instead)
    START          close the camera / open it again
    left stick     turn the cube
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "python"))  # the cart's engine

import alia_engine as ae  # noqa: E402
import alia_kit as kit  # noqa: E402

WHITE, DIM, SHADOW = 0xFFFFFFFF, 0xC0C8D0FF, 0x000000C0
BACKGROUNDS = ("cover", "fit", "off")


class Game:
    def init(self, e):
        self.cube = e.world.spawn(e.create_mesh(kit.box(1.2, 1.2, 1.2, kit.pal.ORANGE)), position=(0, 2, 5))
        e.world.camera.position, e.world.camera.yaw = (0, 2, 0), 0.0
        self.background = 0
        self.photos, self.shot, self.note = 0, None, ""
        self.rate, self.last_seq, self.last_t = 0.0, None, time.monotonic()
        e.camera.open()

    def fixed_update(self, e, dt):
        i = e.input
        self.cube.yaw += (0.6 + i.lx * 3) * dt
        self.cube.pitch += i.ly * 3 * dt
        cam = e.camera
        if i.pressed & ae.SOUTH:
            self.background = (self.background + 1) % len(BACKGROUNDS)
        if i.pressed & ae.START:
            if cam.state == "closed":
                cam.open()
            else:
                cam.close()
        if i.pressed & ae.EAST and self.shot is None:
            self.shot, self.note = cam.photo(), "taking a photo..."
        if self.shot is not None and self.shot.done:
            if self.shot.jpeg:
                self.photos += 1
                name = f"photo-{self.photos}.jpg"
                ae.save.write(name, self.shot.jpeg)
                self.note = f"saved {name} ({len(self.shot.jpeg) // 1024} KiB)"
            else:
                self.note = f"photo failed: {self.shot.error}"
            self.shot = None
        e.frame.camera_background = BACKGROUNDS[self.background]
        # the camera's frame rate, from its sequence numbers
        now = time.monotonic()
        if now - self.last_t >= 1.0:
            seq = cam.frame.sequence if cam.frame else None
            if seq is not None and self.last_seq is not None:
                self.rate = (seq - self.last_seq) / (now - self.last_t)
            self.last_seq, self.last_t = seq, now

    def render(self, e, alpha):
        cam = e.camera
        w, h = e.scene_size
        if BACKGROUNDS[self.background] == "off":
            e.hud_camera(w - 170, 30, 160, 120)
            e.hud_rect(w - 171, 29, 162, 1, WHITE); e.hud_rect(w - 171, 150, 162, 1, WHITE)
            e.hud_rect(w - 171, 29, 1, 122, WHITE); e.hud_rect(w - 10, 29, 1, 122, WHITE)
        e.hud_rect(0, 0, w, 24, 0x00000090)
        m = cam.mode
        mode = f"{m.width}x{m.height} {m.order} turn {m.turn}" if m else "-"
        size = "%dx%d" % cam.size if cam.size else "-"
        e.hud_text(4, 3, f"CAMERA {cam.state.upper()}  MODE {mode}  IMAGE {size}  {self.rate:4.1f} FPS  "
                         f"{'ZERO COPY' if cam.zero_copy else 'COPIED'}", WHITE, 1, SHADOW)
        e.hud_text(4, 13, cam.message or f"background {BACKGROUNDS[self.background]}", DIM, 1)
        e.hud_text(4, h - 22, self.note, WHITE, 1, SHADOW)
        e.hud_text(4, h - 11, "A: PHOTO   B: BACKGROUND   START: CLOSE / OPEN   LEFT STICK: CUBE", DIM, 1, SHADOW)


if __name__ == "__main__":
    ae.run(Game(), argv=sys.argv[1:], title="alia-engine camera test", app_id="pad.alia.engine.camera")
