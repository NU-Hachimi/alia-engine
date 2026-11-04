"""alia-engine stick test: both analog sticks' raw values (e.input lx ly rx ry, no dead zone), the range each
axis has reached, and a trail. For checking new sticks: centre drift, range, a dead axis. Run from apps/sticks/
(docs/project.md):

    alia run                                     PC window
    alia cart && alia install                    as a cart on the board

Controls: the sticks; START resets both ranges, L3 / R3 one. +y is down (pushing forward is negative).
"""

import os
import sys
from collections import deque

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "python"))  # the cart's engine

import alia_engine as ae  # noqa: E402

R = 120  # pad radius, scene pixels
WHITE, DIM, GRID, BG, SHADOW = 0xFFFFFFFF, 0x8090A0FF, 0x405060FF, 0x101820FF, 0x000000C0
COLOURS = (0x40C0FFFF, 0xFFA040FF)
DEAD_ZONE = 0.15  # a typical one (the sample's), drawn as a box


class Stick:
    def __init__(self, name, cx, colour):
        self.name, self.cx, self.colour = name, cx, colour
        self.x = self.y = 0.0
        self.trail = deque(maxlen=90)
        self.reset()

    def reset(self):
        self.lo, self.hi = [0.0, 0.0], [0.0, 0.0]

    def update(self, x, y):
        self.x, self.y = x, y
        for i, v in enumerate((x, y)):
            self.lo[i], self.hi[i] = min(self.lo[i], v), max(self.hi[i], v)
        self.trail.append((x, y))

    def box(self, e, x0, y0, x1, y1, colour):
        e.hud_rect(x0, y0, x1 - x0 + 1, 1, colour)
        e.hud_rect(x0, y1, x1 - x0 + 1, 1, colour)
        e.hud_rect(x0, y0, 1, y1 - y0 + 1, colour)
        e.hud_rect(x1, y0, 1, y1 - y0 + 1, colour)

    def draw(self, e, cy):
        cx = self.cx
        e.hud_rect(cx - R, cy - R, 2 * R, 2 * R, BG)
        e.hud_rect(cx - R, cy, 2 * R, 1, GRID)
        e.hud_rect(cx, cy - R, 1, 2 * R, GRID)
        d = int(R * DEAD_ZONE)
        self.box(e, cx - d, cy - d, cx + d, cy + d, GRID)
        self.box(e, cx + self.lo[0] * R, cy + self.lo[1] * R, cx + self.hi[0] * R, cy + self.hi[1] * R, DIM)
        for k, (x, y) in enumerate(self.trail):
            a = 0x20 + 0xA0 * k // len(self.trail)
            e.hud_rect(cx + x * R - 1, cy + y * R - 1, 2, 2, (self.colour & 0xFFFFFF00) | a)
        e.hud_rect(cx + self.x * R - 4, cy + self.y * R - 4, 9, 9, self.colour)
        mag = (self.x ** 2 + self.y ** 2) ** 0.5
        e.hud_text(cx - R, cy - R - 14, self.name, WHITE, 1, SHADOW)
        e.hud_text(cx - R, cy + R + 6, f"x {self.x:+.3f}  y {self.y:+.3f}  |{mag:.3f}|", WHITE, 1, SHADOW)
        e.hud_text(cx - R, cy + R + 18, f"x {self.lo[0]:+.2f}..{self.hi[0]:+.2f}  y {self.lo[1]:+.2f}..{self.hi[1]:+.2f}",
                   DIM, 1)


class Game:
    def init(self, e):
        e.frame.sky_top = e.frame.sky_horizon = 0x080C10
        w, _ = e.scene_size
        self.sticks = (Stick("LEFT STICK", w // 4, COLOURS[0]), Stick("RIGHT STICK", 3 * w // 4, COLOURS[1]))

    def fixed_update(self, e, dt):
        i = e.input
        left, right = self.sticks
        left.update(i.lx, i.ly)
        right.update(i.rx, i.ry)
        for s, bit in ((left, ae.L3), (right, ae.R3)):
            if i.pressed & (bit | ae.START):
                s.reset()

    def render(self, e, alpha):
        w, h = e.scene_size
        for s in self.sticks:
            s.draw(e, h // 2 - 4)
        state = "GAMEPAD" if e.input.gamepad else "NO GAMEPAD"
        e.hud_text(8, h - 12, f"{state}   START: reset ranges (L3 / R3: one)   +y is down", DIM, 1)


if __name__ == "__main__":
    ae.run(Game(), argv=sys.argv[1:], title="alia-engine stick test", app_id="pad.alia.engine.sticks")
