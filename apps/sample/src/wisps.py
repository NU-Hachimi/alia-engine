"""The wisps: glowing balls that drift over the meadow, flee when the keeper comes close, and trail light. Each
Wisp is a plain class holding its engine objects (a node with a mesh and a collider, a trail emitter under it):
the engine draws, interpolates and simulates them; the class only decides where the wisp wants to go."""

import math

import alia_engine as ae
import alia_kit as kit

import field

WISP = 2                  # collision layer (field.SOLID is 1): what the beam catches
SIZE = 0.28               # m, the ball's radius
DRIFT, FLEE = 2.2, 5.0    # m/s
SHY = 6.0                 # m: closer than this to the keeper and it flees
GLOWS = (9, 10, 11)       # palette indices (main.PALETTE): cyan, gold, rose


class Wisp:
    def __init__(self, e, ball, trail, rng, k):
        self.e, self.rng = e, rng
        self.tint = GLOWS[k % len(GLOWS)]
        self.node = e.world.spawn(ball, scale=SIZE, tint=self.tint)
        self.node.collider = ae.sphere((0, 0, 0), 1.5, layers=WISP, user=k)  # x the scale: a little larger than it looks
        self.trail = e.world.spawn_emitter(trail, parent=self.node, rate=18, seed=100 + k)
        self.bob = rng.uniform(0, 2 * math.pi)
        self.place(far_from=(0.0, -8.0))

    def place(self, far_from):
        """Somewhere on the meadow, away from the keeper (a fresh wisp, or a caught one coming back)."""
        while True:
            a, r = self.rng.uniform(0, 2 * math.pi), self.rng.uniform(4, field.RADIUS)
            x, z = r * math.cos(a), r * math.sin(a)
            if kit.distance_xz((x, z), far_from) > 12:
                break
        self.x, self.z, self.target = x, z, self._somewhere()
        self.height = self.rng.uniform(1.4, 3.0)
        self.node.position = (x, self.e.world.ground_height(x, z) + self.height, z)
        self.node.teleport()
        self.trail.clear()

    def _somewhere(self):
        a, r = self.rng.uniform(0, 2 * math.pi), self.rng.uniform(0, field.RADIUS)
        return r * math.cos(a), r * math.sin(a)

    def update(self, keeper, t, dt):
        dx, dz = self.x - keeper.x, self.z - keeper.z
        near = math.hypot(dx, dz)
        if near < SHY:  # away from the keeper
            speed, (vx, vz) = FLEE, (dx / max(near, 1e-3), dz / max(near, 1e-3))
        else:           # towards a wandering target, a new one when it gets there
            tx, tz = self.target[0] - self.x, self.target[1] - self.z
            d = math.hypot(tx, tz)
            if d < 1.0:
                self.target = self._somewhere()
            speed, (vx, vz) = DRIFT, (tx / max(d, 1e-3), tz / max(d, 1e-3))
        self.x = max(-field.RADIUS, min(field.RADIUS, self.x + vx * speed * dt))
        self.z = max(-field.RADIUS, min(field.RADIUS, self.z + vz * speed * dt))
        y = self.e.world.ground_height(self.x, self.z) + self.height + 0.35 * math.sin(2.1 * t + self.bob)
        self.node.position = (self.x, y, self.z)
