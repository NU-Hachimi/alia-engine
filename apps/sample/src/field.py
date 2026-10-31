"""The meadow: rolling ground (a heightmap the engine walks and casts rays on, and a mesh to see it), a ring of
standing stones the wisps hide behind (colliders on the SOLID layer: they stop the beam), and the dusk sky.
Everything here is spawned once and never touched again: the engine draws it."""

import math

import alia_engine as ae
import alia_kit as kit

# Palette indices (main.PALETTE).
GRASS_DARK, GRASS_LIGHT, STONE, STONE_TOP = 1, 2, 3, 4
SOLID = 1                 # collision layers: what the beam stops at
N, CELL = 17, 4.0         # 17 x 17 heights, 4 m apart: a 64 m square
ORIGIN = -(N - 1) * CELL / 2
RADIUS = 26.0             # m: where the wisps may go


def height(x, z):
    """The ground's shape: low rolling hills, flatter towards the middle."""
    r = min(1.0, math.hypot(x, z) / 30)
    return (0.6 + 1.6 * r * r) * (math.sin(x * 0.21) * math.cos(z * 0.17) + 0.5 * math.sin(x * 0.09 + z * 0.13))


class Field:
    def __init__(self, e, rng):
        w = e.world
        # The ground to see and the world's ground (docs/api.md §5: ground_height, ray_ground) from one height function.
        kit.Terrain(e, size=(N - 1) * CELL, cell=CELL, height=height,
                    colours=lambda x, y, z: GRASS_LIGHT if y > 0.3 else GRASS_DARK)
        # The standing stones: a box each, with a capsule the beam can't pass.
        self.stones = []
        for k in range(9):
            a = 2 * math.pi * k / 9 + rng.uniform(-0.2, 0.2)
            x, z = 15 * math.cos(a), 15 * math.sin(a)
            tall = rng.uniform(3.0, 4.5)
            stone = w.spawn(e.create_mesh(kit.box(1.4, tall, 1.0, STONE, (0, tall / 2, 0))),
                            position=(x, w.ground_height(x, z) - 0.2, z), yaw=rng.uniform(0, math.pi))
            stone.collider = ae.capsule((0, 0.5, 0), (0, tall - 0.5, 0), 0.75, layers=SOLID)
            self.stones.append(stone)
        self.dusk(e)

    @staticmethod
    def dusk(e):
        """Sky, fog and light: kit's dusk, a little tuned; e.frame keeps them from frame to frame (docs/api.md §1.2)."""
        kit.environment(e, "dusk")
        f = e.frame
        f.sky_span, f.fog_steps, f.ground_top, f.ground_ramp = 0.5, 10, 1.2, 25.0
        cam = e.world.camera
        cam.near, cam.far, cam.fov_x = 0.3, 70.0, 1.25

    @staticmethod
    def nightfall(e, tw, seconds):
        """Over the round the dusk turns to night (alia_tween, docs/plugins.md): the sky, the fog and the sun ease
        towards kit's night, slowly at first."""
        night, f = kit.ENVIRONMENTS["night"], e.frame
        for attr in ("sky_top", "sky_horizon", "fog_color"):
            tw.to(f, attr, night[attr], seconds, ease="in_sine", colour="rgb")
        tw.to(f.sun, "ambient", 0.3, seconds, ease="in_sine")
        tw.to(f.sun, "diffuse", 0.35, seconds, ease="in_sine")
