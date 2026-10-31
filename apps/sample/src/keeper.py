"""The keeper: a small figure (a rig, made here as a template and spawned once) that walks the meadow with the
left stick, its head turning towards the nearest wisp (rigid IK, docs/api.md §4.1), and casts a beam where the
camera looks. Walking and the camera are alia_kit's (docs/api.md §7.1): a GroundMover keeps it on the ground and
out of the standing stones, a FollowCamera follows it over the shoulder; the right stick turns the view."""

import math

import alia_engine as ae
import alia_kit as kit
from alia_pack import PackWriter

import field

SPEED = 4.5                # m/s at full stick
AIM_LIFT = 0.12            # rad above the crosshair's centre line: the wisps float
GAZE_RATE = 5.0            # per s: how quickly the head's look catches up with its wisp
LOOK_LIMIT = math.radians(70)  # how far the head turns from where the body faces
BEAM_RANGE = 32.0          # m
BEAM_SHOWN = 0.12          # s the beam stays visible
COOLDOWN = 0.35            # s between beams
# Palette indices (main.PALETTE).
CLOAK, SKIN, DARK, BEAM = 5, 6, 7, 8


def keeper_template(e):
    """The keeper's rig in alia_pack's form (docs/formats.md type 3): bones, two attach points, two animations.
    Made in code for the sample; a game would bake it into its resource pack."""
    swing = lambda a: [[a, 0, 0], [0, 0, 0], [-a, 0, 0], [0, 0, 0]]  # noqa: E731
    pack = PackWriter()
    pack.add_rig("keeper", {
        "bones": [{"name": "hips", "parent": None, "offset": [0, 0.8, 0], "mesh": "hips"},
                  {"name": "body", "parent": "hips", "offset": [0, 0.1, 0], "mesh": "body"},
                  {"name": "head", "parent": "body", "offset": [0, 0.75, 0], "mesh": "head"},
                  {"name": "leg_l", "parent": "hips", "offset": [0.15, 0, 0], "mesh": "leg"},
                  {"name": "leg_r", "parent": "hips", "offset": [-0.15, 0, 0], "mesh": "leg"},
                  {"name": "arm_l", "parent": "body", "offset": [0.32, 0.65, 0], "mesh": "arm"},
                  {"name": "arm_r", "parent": "body", "offset": [-0.32, 0.65, 0], "mesh": "arm"}],
        "attach": {"lantern": {"bone": "arm_r", "offset": [0, -0.6, 0.15]}},
        "anims": [{"name": "idle", "loop": True, "keys": 2, "tracks": {"arm_l": [[0, 0, 4], [0, 0, 8]], "arm_r": [[0, 0, -4], [0, 0, -8]]},
                   "root_y": [0.0, 0.02]},
                  {"name": "walk", "loop": True, "keys": 4,
                   "tracks": {"leg_l": swing(28), "leg_r": swing(-28), "arm_l": swing(-22), "arm_r": swing(22)},
                   "root_y": [0.0, 0.05, 0.0, 0.05]}],
    })
    meshes = {"hips": e.create_mesh(kit.box(0.45, 0.2, 0.3, DARK)),
              "body": e.create_mesh(kit.box(0.55, 0.7, 0.35, CLOAK, (0, 0.35, 0))),
              "head": e.create_mesh(kit.box(0.32, 0.32, 0.32, SKIN, (0, 0.16, 0))),
              "leg": e.create_mesh(kit.box(0.16, 0.8, 0.16, DARK, (0, -0.4, 0))),
              "arm": e.create_mesh(kit.box(0.13, 0.6, 0.13, CLOAK, (0, -0.3, 0)))}
    return e.load_rigs(pack.bytes())["keeper"], meshes


class Keeper:
    def __init__(self, e, glow_material):
        w = e.world
        rig, meshes = keeper_template(e)
        self.mover = kit.GroundMover(w, position=(0.0, -8.0), radius=0.4, speed=SPEED, layers=field.SOLID,
                                     bounds=(-28.0, -28.0, 28.0, 28.0))
        self.camera = kit.FollowCamera(w.camera, distance=6.5, height=3.0, shoulder=0.9, pitch=-0.22, turn_speed=2.2)
        self.node = w.spawn_rig(rig, position=self.mover.position, meshes=meshes)
        self.node.set_subtree(light=ae.Light.CHARACTER)  # lit from the camera, so it reads against the dusk
        # The head looks along a direction that follows the wisp smoothly (kit.Gaze), not at the wisp itself: a new
        # wisp to watch, a jumpy one or one circling the keeper turns the head instead of snapping it.
        self.gaze = kit.Gaze(rate=GAZE_RATE, speed=4.0)
        self.look = self.node.look_at("head", (0, 0, 0), max_angle=LOOK_LIMIT, weight=0.0)
        self.lantern = self.node.attach_point("lantern")
        # The beam: one thin glowing ball from the lantern, stretched to where it lands, shown for a moment.
        beam_mesh = e.create_mesh(kit.sphere(1.0, rings=2, segments=4, material=glow_material))
        self.beam = w.spawn(beam_mesh, tint=BEAM, visible=False)
        self.beam_left = self.cooldown = 0.0
        self.walking = None

    @property
    def x(self):
        return self.mover.x

    @property
    def z(self):
        return self.mover.z

    def update(self, e, move, turn, dt):
        """move: the left stick (walks relative to the camera); turn: the right stick's x."""
        self.camera.turn(turn, dt)
        self.mover.move(kit.stick_to_world(move, self.camera.yaw), dt)
        self.node.position, self.node.yaw = self.mover.position, self.mover.yaw
        # play() starts an animation from its beginning: only when it changes; the pace follows the stick.
        walking = self.mover.moving > 0.05
        if walking != self.walking:
            self.node.animator.play("walk" if walking else "idle")
            self.walking = walking
        self.node.animator.speed = 200 * self.mover.moving if walking else 64
        self.cooldown = max(0.0, self.cooldown - dt)
        self.beam_left = max(0.0, self.beam_left - dt)
        self.beam.visible = self.beam_left > 0
        self.camera.follow(e.world, self.mover.position, dt=dt)

    def watch(self, wisp_node, dt):
        """Turn the head towards this wisp (None: look ahead again); the weight eases in and out."""
        if wisp_node is not None:
            if self.look.weight == 0.0:
                self.gaze.reset()  # from nothing: start on it, the weight eases in
            self.look.target = self.gaze.update(self.node.bone("head").world_position, wisp_node.world_position, dt,
                                                forward=self.mover.yaw, limit=LOOK_LIMIT)
        step = 3.0 * dt
        self.look.weight = min(1.0, self.look.weight + step) if wisp_node is not None else max(0.0, self.look.weight - step)

    def cast(self, w, layers):
        """Fire the beam if it is ready: the hit (alia_engine.Hit) or None, and whether it fired at all."""
        if self.cooldown > 0:
            return None, False
        self.cooldown, self.beam_left = COOLDOWN, BEAM_SHOWN
        origin, direction = self.camera.ray(lift=AIM_LIFT)
        hit = w.ray_cast(origin, direction, BEAM_RANGE, layers)
        end = hit.point if hit else tuple(o + d * BEAM_RANGE for o, d in zip(origin, direction))
        start = self.lantern.world_position
        v = kit.sub(end, start)
        # A unit ball stretched along the beam: centre halfway, long axis (+Z) along it.
        self.beam.position = kit.lerp3(start, end, 0.5)
        self.beam.set_rotation(*kit.aim(v), 0.0)
        self.beam.scale = (0.09, 0.09, kit.length(v) / 2)
        self.beam.teleport()
        return hit, True
