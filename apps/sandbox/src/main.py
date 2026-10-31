"""alia-engine sandbox: one exhibit per engine feature, a free camera and live stats, for looking at
what the engine draws (PC window, the board, headless screenshots). It uses only the public
alia_engine API (docs/api.md), so it is also an example; each E4 step adds its exhibits here.
Run from apps/sandbox/ (docs/project.md):

    alia run                                     PC window
    alia run --headless --frames 120 --screenshot shot.png
    alia run --logic-hz 10                       interpolation at work
    alia run --logic-hz 10 --no-interpolate      ... and without it
    alia cart && alia install && alia launch     as a cart on the board

Everything but the HUD and the far beacon is a world node (docs/api.md §3): spawned once, changed
in fixed_update, drawn and interpolated by the engine. The beacon goes through draw() each frame,
to show both paths side by side.

Controls (gamepad / keyboard):
    left stick  / WASD         move            right stick / IJKL     look
    L1, R1      / E, U         down, up        L3          / F        back to the start
    X (north)   / C            distance fog    Y (west)    / V        ground fog
    A (east)    / X            sun turns       B (south)   / Z        help on / off
    START       / Enter        next palette    SELECT      / Backspace  red overlay (damage flash)
"""

import argparse
import math
import os
import sys
from array import array

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "python"))

import alia_engine as ae  # noqa: E402
import alia_kit as kit  # noqa: E402
from alia_pack import PackWriter  # noqa: E402

MOVE_SPEED, LOOK_SPEED = 8.0, 1.6  # m/s, rad/s
START = ((0.0, 4.0, -24.0), 0.0, -0.1)  # eye, yaw, pitch
SPACING = 3.5
WHITE, SHADOW, DIM = 0xFFFFFFFF, 0x000000C0, 0xB0B8C8FF

# 32-colour palettes (index 0 unused): the sandbox's "Looks".
BASE = [(0, 0, 0), (200, 60, 40), (60, 160, 70), (230, 200, 70), (80, 110, 210), (210, 210, 210), (120, 120, 130),
        (60, 60, 70), (230, 130, 40), (90, 220, 230), (240, 90, 180), (150, 90, 50), (40, 90, 60), (250, 250, 250),
        (255, 220, 120), (130, 200, 255), (95, 150, 80), (78, 125, 66)] + [(128, 128, 128)] * 14
PALETTES = (
    ("DAY", BASE),
    ("DUSK", [(min(255, int(r * 1.1 + 25)), int(g * 0.75), int(b * 0.6 + 20)) for r, g, b in BASE]),
    ("MONO", [(v, v, v) for v in (int(0.3 * r + 0.59 * g + 0.11 * b) for r, g, b in BASE)]),
)
SKIES = {"DAY": (0x5A7FC0, 0xC8D4E0), "DUSK": (0x4A3060, 0xF0A070), "MONO": (0x404040, 0xC0C0C0)}
RED, GREEN, YELLOW, BLUE, LIGHT, GREY, DARK, ORANGE, CYAN, PINK = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
GRASS, GRASS_DARK = 16, 17
WALKER = 100  # collider user id of the walking rig


def mesh(positions, indices, tri_palette=None, **kw):
    palette = bytes(tri_palette) if tri_palette is not None else None
    return ae.Mesh(array("f", positions).tobytes(), array("H", indices).tobytes(), tri_palette=palette, **kw)


def cube():
    """Unit cube (side 2), counter-clockwise outward faces; triangles 0-3 front/back, 4-7 bottom/top, 8-11 sides."""
    p = [-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1]
    i = [0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 2, 3, 7, 2, 7, 6, 1, 2, 6, 1, 6, 5, 0, 4, 7, 0, 7, 3]
    return p, i


def sphere(rings=12, segments=16):
    p, i = [], []
    for r in range(rings + 1):
        v = math.pi * r / rings
        for s in range(segments + 1):
            u = 2 * math.pi * s / segments
            p += [math.sin(v) * math.cos(u), math.cos(v), math.sin(v) * math.sin(u)]
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b, c = a + 1, a + segments + 1
            i += [a, b, c, b, c + 1, c]
    return p, i


def ground(n=24, cell=4.0):
    """A checkerboard: both triangles of a cell share a palette colour (flat, lit by the sun). Vertex
    colours can't make a checkerboard: they blend along each cell's diagonal into stripes."""
    p, i, colors = [], [], []
    half = n * cell / 2
    for j in range(n + 1):
        for k in range(n + 1):
            p += [k * cell - half, 0.0, j * cell - half]
    for j in range(n):
        for k in range(n):
            a = j * (n + 1) + k
            i += [a, a + n + 1, a + 1, a + 1, a + n + 1, a + n + 2]
            colors += [GRASS if (j + k) % 2 else GRASS_DARK] * 2
    return p, i, colors


def box(sx, sy, sz, color, centre=(0.0, 0.0, 0.0), faces=None):
    """A box sx x sy x sz around `centre` (so its joint, the origin, can be at an end), in one palette
    colour or per-face colours. Shapes come from the mesh, not from scaling nodes: a non-uniform scale
    under a rotation would shear the children."""
    p, i = cube()
    half = (sx / 2, sy / 2, sz / 2)
    p = [v * half[k % 3] + centre[k % 3] for k, v in enumerate(p)]
    return mesh(p, i, tri_palette=faces or [color] * 12)


def walker_rig():
    """A little figure that walks: legs and arms swing in opposite phase, the hips bob; a "step"
    event at each footfall, with the foot's attach point."""
    legs = {"leg_l": [[30, 0, 0], [0, 0, 0], [-30, 0, 0], [0, 0, 0]],
            "leg_r": [[-30, 0, 0], [0, 0, 0], [30, 0, 0], [0, 0, 0]]}
    arms = {"arm_l": [[-25, 0, 0], [0, 0, 0], [25, 0, 0], [0, 0, 0]],
            "arm_r": [[25, 0, 0], [0, 0, 0], [-25, 0, 0], [0, 0, 0]]}
    return {
        "bones": [{"name": "hips", "parent": None, "offset": [0, 1.0, 0], "mesh": "hips"},
                  {"name": "torso", "parent": "hips", "offset": [0, 0.125, 0], "mesh": "torso",  # on top of the hips
                   "colliders": [{"type": "capsule", "a": [0, 0.15, 0], "b": [0, 0.6, 0], "r": 0.4, "user": WALKER}]},
                  {"name": "head", "parent": "torso", "offset": [0, 0.75, 0], "mesh": "head"},
                  {"name": "leg_l", "parent": "hips", "offset": [0.2, 0, 0], "mesh": "leg"},
                  {"name": "leg_r", "parent": "hips", "offset": [-0.2, 0, 0], "mesh": "leg"},
                  {"name": "arm_l", "parent": "torso", "offset": [0.45, 0.7, 0], "mesh": "arm"},  # at the shoulders
                  {"name": "arm_r", "parent": "torso", "offset": [-0.45, 0.7, 0], "mesh": "arm"},
                  {"name": "forearm_l", "parent": "arm_l", "offset": [0, -0.35, 0], "mesh": "arm"},  # elbows
                  {"name": "forearm_r", "parent": "arm_r", "offset": [0, -0.35, 0], "mesh": "arm"}],
        "attach": {"foot_l": {"bone": "leg_l", "offset": [0, -1.0, 0]}, "foot_r": {"bone": "leg_r", "offset": [0, -1.0, 0]},
                   "hand_l": {"bone": "forearm_l", "offset": [0, -0.35, 0]},
                   "hand_r": {"bone": "forearm_r", "offset": [0, -0.35, 0]}},
        "anims": [{"name": "walk", "loop": True, "keys": 4, "tracks": {**legs, **arms},
                   "root_y": [0.0, 0.06, 0.0, 0.06],
                   "events": [{"phase": 0, "type": "step", "foot": "foot_r"}, {"phase": 128, "type": "step", "foot": "foot_l"}]}],
    }


def at(x, y, z, s=1.0, yaw=0.0):
    c, n = math.cos(yaw) * s, math.sin(yaw) * s
    return (c, 0, n, x, 0, s, 0, y, -n, 0, c, z)


class Sandbox:
    def __init__(self, logic_hz=60.0):
        self.logic_hz = logic_hz

    def init(self, e):
        self.e, self.t, self.help = e, 0.0, True
        self.fog, self.ground_fog, self.sun_turns, self.flash, self.palette = True, True, False, False, 0
        M = ae.Material
        mat = {
            "flat": 0,  # the default material: flat shading, the mesh's palette colours
            "smooth": e.create_material(M(shading=ae.Shading.SMOOTH, palette_index=LIGHT)),
            "vertex": e.create_material(M(shading=ae.Shading.SMOOTH, color=ae.ColorSource.VERTEX)),
            "unlit": e.create_material(M(shading=ae.Shading.UNLIT, palette_index=ORANGE)),
            "glass": e.create_material(M(palette_index=CYAN, alpha_mode=ae.AlphaMode.BLEND, opacity=0.45)),
            "two_sided": e.create_material(M(palette_index=PINK, double_sided=True)),
            "character": e.create_material(M(palette_index=LIGHT, light=ae.Light.CHARACTER)),
            "beacon": e.create_material(M(shading=ae.Shading.UNLIT, palette_index=YELLOW, fog=False)),
        }
        self.glow = M(palette_index=PINK, emissive=0.0)
        mat["glow"] = e.create_material(self.glow)
        cp, ci = cube()
        sp, si = sphere()
        faces = [RED] * 4 + [YELLOW] * 4 + [GREEN] * 4
        gp, gi, gcolors = ground()
        rgb = bytes(c for k in range(len(sp) // 3) for c in (120 + int(120 * sp[3 * k]), 160, 120 + int(120 * sp[3 * k + 2])))
        one = lambda n, m: [(0, n, m)]  # noqa: E731
        self.ground = e.create_mesh(mesh(gp, gi, tri_palette=gcolors))  # default material: flat, lit
        self.cube = e.create_mesh(mesh(cp, ci, tri_palette=faces))
        ns = len(si) // 3
        exhibits = [  # (label, mesh); the list reads left to right on screen (+X is screen-left)
            ("FLAT", self.cube),
            ("SMOOTH", e.create_mesh(mesh(sp, si, submeshes=one(ns, mat["smooth"])))),
            ("VERTEX", e.create_mesh(mesh(sp, si, vertex_rgb=rgb, submeshes=one(ns, mat["vertex"])))),
            ("UNLIT", e.create_mesh(mesh(sp, si, submeshes=one(ns, mat["unlit"])))),
            ("EMISSIVE", e.create_mesh(mesh(sp, si, submeshes=one(ns, mat["glow"])))),
            ("PARTS", e.create_mesh(mesh(cp, ci, tri_palette=faces, submeshes=[(0, 8, 0), (8, 4, mat["unlit"])]))),
            ("CHARACTER", e.create_mesh(mesh(sp, si, submeshes=one(ns, mat["character"])))),
            ("TINT", self.cube),
            ("FADE", self.cube),
            ("GLASS", None),
            ("TWO-SIDED", e.create_mesh(mesh([-1, -1, 0, 1, -1, 0, 0, 1.2, 0], [0, 1, 2], submeshes=one(1, mat["two_sided"])))),
        ]
        glass = e.create_mesh(mesh(cp, ci, submeshes=one(12, mat["glass"])))
        self.beacon = e.create_mesh(mesh(cp, ci, submeshes=one(12, mat["beacon"])))
        self.mat = mat

        w = e.world
        w.spawn(self.ground)
        w.set_ground([0.0] * 4, 2, 100.0, (-50.0, -50.0))  # flat: the crosshair also measures the ground
        self.names = {k + 1: label for k, (label, _) in enumerate(exhibits)} | {WALKER: "WALKER (RIG COLLIDER)"}
        self.aim = None  # (text, hit point) under the crosshair
        self.labels, self.spinning = [], []  # labels: (node or position, height, text)
        for k, (label, m) in enumerate(exhibits):
            x = (len(exhibits) - 1) * SPACING / 2 - k * SPACING
            if label == "GLASS":  # two overlapping see-through cubes: drawn back to front
                self.spinning += [(w.spawn(glass, position=(x + 0.5, 1.2, 0.6), scale=0.7), 0.5),
                                  (w.spawn(glass, position=(x - 0.5, 1.0, -0.6), scale=0.7), -0.4)]
            else:
                node = w.spawn(m, position=(x, 1.2, 0.0), scale=0.9, tint=BLUE if label == "TINT" else 0)
                node.collider = ae.sphere((0.0, 0.0, 0.0), 1.1, user=k + 1)  # what the crosshair ray hits
                self.spinning.append((node, 0.5))  # the leaf turns to show its back
                if label == "FADE":
                    self.fading = node
            self.labels.append(((x, 2.7, 0.0), label))
        for k in range(6):  # low cubes beyond the left end, further and further out: they sink into the ground fog
            w.spawn(self.cube, position=(21.0 + 1.5 * k, 0.6, 8.0 + 7 * k), scale=0.6)
        self.labels += [((25.0, 3.2, 26.0), "GROUND FOG"), ((0.0, 8.5, 60.0), "NO FOG (DRAW)")]

        # Hierarchy: a turning base, two arms turning on it and lifting, a ball at the end of each arm.
        base_mesh = e.create_mesh(box(2.4, 0.8, 2.4, 0, faces=faces))
        arm_mesh = e.create_mesh(box(1.8, 0.25, 0.25, RED, (0.9, 0, 0)))  # its joint at one end
        self.base = w.spawn(base_mesh, name="base", position=(-5.0, 0.4, -14.0))
        self.arms = []
        for side in (1, -1):
            pivot = w.spawn(parent=self.base, position=(0.0, 0.6, 0.0))  # an empty node: the arm's joint
            arm = w.spawn(arm_mesh, parent=pivot, yaw=0.0 if side > 0 else math.pi)
            w.spawn(exhibits[1][1], parent=arm, position=(1.8, 0.0, 0.0), scale=0.35)
            self.arms.append((pivot, side))
        self.labels.append(((-5.0, 2.3, -14.0), "HIERARCHY"))

        # Particles (simulated in C++, drawn as one instanced draw per template): glowing balls, a
        # fountain that bounces on the ground and, every 1.5 s, a burst that settles in a new colour.
        # The glow is the ball's material: emissive (full colour whatever the light) and out of both
        # fogs, so the balls stay bright far away. A burst's tint replaces the material's colour.
        bp, bi = sphere(4, 6)  # 48 triangles: a particle is drawn hundreds of times
        glow_ball = e.create_mesh(mesh(bp, bi, submeshes=[(0, len(bi) // 3, e.create_material(
            M(shading=ae.Shading.SMOOTH, palette_index=CYAN, emissive=1.0, fog=False, ground_fog=False)))]))
        fountain = e.particles(glow_ball, lifetime=(2.0, 2.6), speed=(4.0, 5.5), size=(0.05, 0.07),
                               gravity=1.0, shape="cone", angle=0.25, ground="bounce", bounce=0.45,
                               fade_over_life=[(0.75, 0.0), (1.0, 1.0)], max_particles=400)
        debris = e.particles(glow_ball, lifetime=(1.6, 2.2), speed=(1.5, 3.5), velocity=(0, 2.0, 0), size=(0.06, 0.09),
                             rotation=(0, 6.3), spin=(-3, 3), gravity=1.0, shape="hemisphere", radius=0.3, ground="stop",
                             size_over_life=[(0.8, 1.0), (1.0, 0.2)], max_particles=200)
        self.fountain = w.spawn_emitter(fountain, position=(0.5, 0.0, -14.0), rate=80, seed=1)
        self.debris = w.spawn_emitter(debris, position=(0.5, 2.6, -14.0), seed=2)
        self.next_burst, self.burst_tint = 0.0, 0
        self.labels.append(((0.5, 3.4, -14.0), "PARTICLES"))

        # A rig made in code: it walks in a circle, and each footfall (an animation event) leaves a
        # footprint at the foot's attach point that fades and goes.
        limbs = {"hips": box(0.6, 0.25, 0.35, BLUE), "torso": box(0.7, 0.75, 0.4, RED, (0, 0.375, 0)),
                 "head": box(0.4, 0.4, 0.4, LIGHT, (0, 0.2, 0)), "leg": box(0.22, 1.0, 0.22, DARK, (0, -0.5, 0)),
                 "arm": box(0.16, 0.35, 0.16, LIGHT, (0, -0.175, 0))}  # limbs hang from their joints, the torso stands on its
        limb_ids = {name: e.create_mesh(m) for name, m in limbs.items()}
        pack = PackWriter()
        pack.add_rig("walker", walker_rig())
        rig = e.load_rigs(pack.bytes())["walker"]
        self.walker = w.spawn_rig(rig, position=(6.0, 0.0, -14.0), meshes=limb_ids)
        self.walker.animator.play("walk", speed=160.0)  # phase units per second: about 0.6 strides per second
        # Rigid IK on top of the walk: the head follows the camera (within 70 degrees), the right hand
        # reaches for a ball bobbing in front of it, blending in and out of the arm swing.
        self.walker.look_at("head", w.camera, max_angle=math.radians(70))
        self.ball = w.spawn(exhibits[1][1], parent=self.walker, position=(-0.55, 1.6, 0.45), scale=0.12)
        self.reach = self.walker.two_bone("arm_r", "forearm_r", "hand_r", self.ball, pole=(0, 0, -1))
        self.footprint = e.create_mesh(mesh([-0.15, 0.02, -0.25, 0.15, 0.02, -0.25, 0.15, 0.02, 0.25, -0.15, 0.02, 0.25],
                                            [0, 3, 2, 0, 2, 1], tri_palette=[DARK, DARK]))
        self.prints = []  # (node, age)
        self.labels.append(((6.0, 3.1, -14.0), "RIG + EVENTS + IK"))

        cam = w.camera
        cam.position, cam.yaw, cam.pitch, cam.fov_x, cam.far = START[0], START[1], START[2], math.radians(75), 90.0
        self.cam = cam
        self.fly = kit.FlyCamera(cam, speed=MOVE_SPEED, look_speed=LOOK_SPEED, min_pitch=-1.3, max_pitch=1.3, level=True)
        self.apply_look()

    def apply_look(self):
        name, colors = PALETTES[self.palette]
        self.e.set_palette(bytes(c for rgb in colors for c in rgb))
        f = self.e.frame
        f.sky_top, f.sky_horizon = SKIES[name]
        f.fog_color = SKIES[name][1]
        f.fog_near, f.fog_full = 32.0, 75.0

    # ---- logic: 60 Hz (--logic-hz) ----
    def fixed_update(self, e, dt):
        i, pressed = e.input, e.input.pressed
        self.t += dt
        if pressed & ae.NORTH:
            self.fog = not self.fog
        if pressed & ae.WEST:
            self.ground_fog = not self.ground_fog
        if pressed & ae.EAST:
            self.sun_turns = not self.sun_turns
        if pressed & ae.SOUTH:
            self.help = not self.help
        if pressed & ae.SELECT:
            self.flash = not self.flash
        if pressed & ae.START:
            self.palette = (self.palette + 1) % len(PALETTES)
            self.apply_look()
        cam = self.cam
        if pressed & ae.L3:
            cam.position, cam.yaw, cam.pitch = START
            cam.teleport()  # a jump: don't slide there
        dead = lambda v: 0.0 if abs(v) < 0.15 else v  # noqa: E731
        up = (1 if i.held & ae.R1 else 0) - (1 if i.held & ae.L1 else 0)
        self.fly.update((dead(i.lx), dead(i.ly)), (dead(i.rx), dead(i.ry)), up, dt, floor=0.3)  # alia_kit
        # Exhibits turn, the fading cube breathes, the hierarchy spins: all world nodes, interpolated.
        for node, speed in self.spinning:
            node.yaw = self.t * speed
        self.fading.fade = 0.5 + 0.5 * math.sin(self.t * 1.5)
        self.base.yaw = self.t * 0.4
        # The walker goes round a circle of 2.5 m, facing along it; footfalls leave fading prints.
        a = self.t * 0.35
        self.walker.position = (6.0 + 2.5 * math.cos(a), 0.0, -14.0 + 2.5 * math.sin(a))
        self.walker.yaw = -a  # tangent to the circle
        self.ball.position = (-0.55 + 0.15 * math.sin(self.t * 0.9), 1.6 + 0.25 * math.sin(self.t * 1.7), 0.45)
        self.reach.weight = 0.5 + 0.5 * math.sin(self.t * 0.6)
        if self.t >= self.next_burst:  # the debris, in the next palette colour each time
            self.burst_tint = self.burst_tint % 6 + 1
            self.debris.burst(30, tint=self.burst_tint)
            self.next_burst = self.t + 1.5
        for event in self.walker.animator.fired:
            x, y, z = self.walker.attach_point(event["foot"]).world_position
            self.prints.append([self.e.world.spawn(self.footprint, position=(x, 0.0, z), yaw=-a), 0.0])
        for p in self.prints:
            p[1] += dt
            p[0].fade = min(1.0, p[1] / 2.0)
        # Collision: a ray from the camera through the crosshair.
        cy, sy, cp, sp = math.cos(cam.yaw), math.sin(cam.yaw), math.cos(cam.pitch), math.sin(cam.pitch)
        ray = (sy * cp, sp, cy * cp)
        hit = self.e.world.ray_cast(cam.position, ray, 80.0)
        if hit:
            self.aim = (f"{self.names.get(hit.user, '?')}  {hit.distance:.1f} M", hit.point)
        else:
            t = self.e.world.ray_ground(cam.position, ray, 80.0)
            self.aim = (f"GROUND  {t:.1f} M", tuple(p + d * t for p, d in zip(cam.position, ray))) if t else None
        for node, age in [p for p in self.prints if p[1] >= 2.0]:
            node.destroy()
        self.prints = [p for p in self.prints if p[1] < 2.0]
        for pivot, side in self.arms:
            lift = 0.25 * (1 - math.cos(self.t * 2 + side))  # 0..0.5 rad up from level: clear of the base
            pivot.set_rotation(self.t * 1.3 * side, 0.0, lift * side)  # the -X arm lifts with a negative roll
        # A material changed while meshes use it: the emissive sphere breathes.
        self.glow.emissive = 0.5 + 0.5 * math.sin(self.t * 2.0)
        e.update_material(self.mat["glow"], self.glow)

    # ---- drawing: every displayed frame ----
    def render(self, e, alpha):
        f, t = e.frame, self.t
        f.fog_steps = 16 if self.fog else 0
        f.ground_fog, f.ground_top, f.ground_ramp = (0.8 if self.ground_fog else 0.0), 1.5, 45.0
        sun_yaw = t * 0.6 if self.sun_turns else 0.8
        f.sun.travel = (-math.cos(0.9) * math.sin(sun_yaw), -math.sin(0.9), -math.cos(0.9) * math.cos(sun_yaw))
        f.sun.ambient, f.sun.diffuse = 0.45, 0.65
        f.character.travel, f.character.ambient, f.character.diffuse = (0.6, -0.5, 0.6), 0.3, 0.9
        f.overlay = 0xC0202050 if self.flash else 0
        # Far out, where everything else is already lost in the fog: a beacon that ignores it, drawn
        # through draw() each frame (not a world node, not interpolated).
        e.draw(self.beacon, at(0.0, 5.0, 60.0, 2.0, yaw=t))
        labels = []
        for where, *rest in self.labels:
            if isinstance(where, ae.Node):  # follows a node
                x, y, z = where.world_position
                labels.append(((x, y + rest[0], z), rest[1]))
            else:
                labels.append((where, rest[0]))
        self.hud(e, labels, alpha)

    def hud(self, e, labels, alpha):
        w, h = e.scene_size
        for p, text in labels:
            if not text:
                continue
            s = kit.world_to_screen(e, p)
            if s and 0 <= s[0] < w and 0 <= s[1] < h:
                e.hud_text(s[0] - len(text) * 3, s[1], text, WHITE, 1, SHADOW)
        st = e.stats
        fps = 1000 / st["frame_ms"] if st["frame_ms"] > 0 else 0
        e.hud_rect(0, 0, w, 12, 0x00000080)
        e.hud_text(4, 3, f"{fps:5.1f} FPS  DRAWS {st['draws']:3d}  TRIS {st['triangles']:5d}  "
                   f"INST {st['instances']:3d}  CULLED {st['culled']:3d}  SUBMIT {st['submit_ms']:4.1f} MS  "
                   f"LOGIC {self.logic_hz:g} HZ  ALPHA {alpha:.2f}", WHITE, 1)
        # The crosshair and what it hits (the collision queries).
        e.hud_rect(w / 2 - 6, h / 2, 12, 1, WHITE)
        e.hud_rect(w / 2, h / 2 - 6, 1, 12, WHITE)
        if self.aim:
            text, point = self.aim
            s = kit.world_to_screen(e, point)
            if s:
                e.hud_rect(s[0] - 2, s[1] - 2, 5, 5, 0xFF4040FF)
            e.hud_text(w / 2 + 10, h / 2 + 6, text, WHITE, 1, SHADOW)
        if not self.help:
            return
        on = lambda b: "ON " if b else "OFF"  # noqa: E731
        lines = [f"X FOG {on(self.fog)}   Y GROUND FOG {on(self.ground_fog)}   A SUN TURNS {on(self.sun_turns)}",
                 f"START PALETTE {PALETTES[self.palette][0]}   SELECT FLASH {on(self.flash)}   B HELP   L3 RESET",
                 "L STICK MOVE   R STICK LOOK   L1 / R1 DOWN / UP"]
        e.hud_rect(0, h - 12 * len(lines) - 6, w, 12 * len(lines) + 6, 0x00000080)
        for k, line in enumerate(lines):
            e.hud_text(4, h - 12 * (len(lines) - k) - 1, line, DIM, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--logic-hz", type=float, default=60.0, help="logic steps per second (10 shows interpolation)")
    args = parser.parse_args(ae.strip_runtime_flags(sys.argv[1:]))
    ae.run(Sandbox(args.logic_hz), argv=sys.argv[1:], title="alia-engine sandbox", fixed_hz=args.logic_hz,
           app_id="pad.alia.engine.sandbox")


if __name__ == "__main__":
    main()
