"""alia_engine from Python: ae.run() owns the loop (architecture §2.1). Run by ctest with the
build directory on PYTHONPATH; exits 77 (skipped) without headless EGL."""

import math
import os
import sys
import tempfile
from array import array

import alia_engine as ae

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


class Game:
    def __init__(self):
        self.calls = []

    def init(self, engine):
        self.calls.append("init")
        self.size = engine.scene_size

    def fixed_update(self, engine, dt):
        self.calls.append("step")
        self.dt = dt
        self.held = engine.input.held

    def render(self, engine, alpha):
        self.calls.append("render")
        engine.hud_text(4, 4, "HI", 0xFFFFFFFF, 2)

    def shutdown(self, engine):
        self.calls.append("shutdown")


check(ae.strip_runtime_flags(["--demo", "--frames", "3", "--stats", "x"]) == ["--demo", "x"],
      "strip_runtime_flags leaves the game's arguments")

shot = os.path.join(tempfile.mkdtemp(), "shot.png")
game = Game()
try:
    ae.run(game, argv=["--headless", "--frames", "5", "--render", "320x180", "--screenshot", shot, "--demo"],
           title="test", fixed_hz=30)
except RuntimeError as error:
    print(f"SKIP: {error}")
    sys.exit(77)
check(game.calls[0] == "init" and game.calls[-1] == "shutdown", "init first, shutdown last")
check(game.calls.count("step") == 5 and game.calls.count("render") == 5, "5 frames, one step each (headless)")
check(abs(game.dt - 1 / 30) < 1e-9, "dt is 1 / fixed_hz")
check(game.size == (320, 180), "--render sets the scene size")
check(os.path.getsize(shot) > 0, "--screenshot writes the last frame")


class Broken(Game):
    def fixed_update(self, engine, dt):
        raise ValueError("boom")


broken = Broken()
try:
    ae.run(broken, argv=["--headless", "--frames", "5"])
    check(False, "an exception in the game propagates")
except ValueError:
    check(broken.calls[-1] == "shutdown", "an exception in the game propagates, after shutdown")

# Resource packs: tools/alia_pack.py writes, the engine reads (docs/formats.md).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import alia_pack  # noqa: E402

w = alia_pack.PackWriter()
w.add_mesh("tri", [0, 0, 0, 1, 0, 0, 0, 1, 0], [0, 1, 2], tri_palette=[3])
w.add_mesh("quad", [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0], [0, 1, 2, 0, 2, 3], vertex_rgb=bytes(range(12)), double_sided=True)
w2 = alia_pack.PackWriter()
w2.add_mesh("quad", [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0], [0, 1, 2, 0, 2, 3], vertex_rgb=bytes(range(12)), double_sided=True)
w2.add_mesh("tri", [0, 0, 0, 1, 0, 0, 0, 1, 0], [0, 1, 2], tri_palette=[3])
check(w.bytes() == w2.bytes(), "pack bytes don't depend on the order meshes were added")
check(set(alia_pack.read(w.bytes())) == {(alia_pack.MESH, "tri"), (alia_pack.MESH, "quad")}, "pack reads back")
refused = 0
for bad in (lambda: w.add_mesh("x", [0, 0, 0], [0, 1, 2], tri_palette=[1]),
            lambda: w.add_mesh("tri", [0, 0, 0, 1, 0, 0, 0, 1, 0], [0, 1, 2], tri_palette=[1]),
            lambda: w.add_mesh("y", [0, 0, 0, 1, 0, 0, 0, 1, 0], [0, 1, 2])):
    try:
        bad()
    except alia_pack.PackError:
        refused += 1
check(refused == 3, "bad meshes refused by the writer (index range, duplicate, no colours)")

folder = tempfile.mkdtemp()
os.makedirs(os.path.join(folder, "content", "gen"))
w.write(os.path.join(folder, "content", "gen", "m.apak"))


class Loader(Game):
    def init(self, engine):
        os.environ["ALIA_CART_DIR"] = folder  # a relative path is taken from the cart's content directory
        self.meshes = engine.load_meshes("gen/m.apak")
        try:
            engine.load_meshes("gen/missing.apak")
            self.missing = False
        except RuntimeError:
            self.missing = True
        del os.environ["ALIA_CART_DIR"]

    def render(self, engine, alpha):
        engine.frame.eye = (0.5, 0.5, -3.0)
        for mesh in self.meshes.values():
            engine.draw(mesh, (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0))


loader = Loader()
ae.run(loader, argv=["--headless", "--frames", "2"])
check(set(loader.meshes) == {"tri", "quad"} and all(isinstance(v, int) and v > 0 for v in loader.meshes.values()),
      "engine.load_meshes reads the writer's pack from the cart's content directory")
check(loader.missing, "a missing pack raises")

# Materials: in code and in a pack (docs/api.md §2).
w = alia_pack.PackWriter()
w.add_material("glow", shading="unlit", palette_index=9, emissive=1.0, fog=False)
w.add_mesh("rune", [0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0], [0, 1, 2, 1, 3, 2], tri_palette=[1, 2],
           submeshes=[(0, 1, ""), (1, 1, "glow")])
w.write(os.path.join(folder, "content", "gen", "mat.apak"))
bad = alia_pack.PackWriter()
bad.add_mesh("x", [0, 0, 0, 1, 0, 0, 0, 1, 0], [0, 1, 2], submeshes=[(0, 1, "missing")])
try:
    bad.bytes()
    check(False, "a part naming a missing material is refused by the writer")
except alia_pack.PackError:
    check(True, "a part naming a missing material is refused by the writer")


class Materials(Game):
    def init(self, engine):
        self.meshes, self.materials = engine.load_pack(os.path.join(folder, "content", "gen", "mat.apak"))
        glass = engine.create_material(ae.Material(palette_index=3, alpha_mode=ae.AlphaMode.BLEND, opacity=0.5))
        self.glass = engine.create_mesh(ae.Mesh(array("f", [0, 0, 0, 1, 0, 0, 0, 1, 0]).tobytes(),
                                                array("H", [0, 1, 2]).tobytes(), submeshes=[(0, 1, glass)]))
        try:
            engine.create_material(ae.Material(light=ae.Light.UNLIT))
            self.refused = False
        except ValueError:
            self.refused = True

    def render(self, engine, alpha):
        engine.frame.eye = (0.5, 0.5, -3.0)
        engine.draw(self.meshes["rune"], (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0))
        engine.draw(self.glass, (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -1))


mats = Materials()
ae.run(mats, argv=["--headless", "--frames", "2"])
check(set(mats.meshes) == {"rune"} and set(mats.materials) == {"glow"}, "load_pack returns meshes and materials")
check(mats.refused, "Material(light=UNLIT) refused (unlit is a shading)")

# The world (docs/api.md §3): persistent nodes, hierarchy, camera; drawn like draw() would.
CUBE = (array("f", [-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1]).tobytes(),
        array("H", [0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 2, 3, 7, 2, 7, 6, 1, 2, 6, 1, 6, 5,
                    0, 4, 7, 0, 7, 3]).tobytes())


def yaw_model(x, y, z, yaw, s=1.0):
    import math
    c, n = math.cos(yaw) * s, math.sin(yaw) * s
    return (c, 0, n, x, 0, s, 0, y, -n, 0, c, z)


class Scene(Game):
    """The same picture twice: through the world, or through draw()."""

    def __init__(self, use_world):
        super().__init__()
        self.use_world = use_world

    def init(self, engine):
        engine.set_palette(bytes([0, 0, 0, 200, 60, 40, 60, 160, 70, 230, 210, 80] + [128] * 84))
        self.mesh = engine.create_mesh(ae.Mesh(*CUBE, tri_palette=bytes([1] * 4 + [2] * 4 + [3] * 4)))
        if self.use_world:
            w = engine.world
            self.base = w.spawn(name="base", position=(0, 1, 4), yaw=0.5)
            self.box = w.spawn(self.mesh, parent=self.base, position=(2, 0, 0), scale=0.5, tint=3)
            w.spawn(self.mesh, position=(-2, 1, 6), fade=0.4)
            cam = w.camera
            cam.position, cam.yaw, cam.pitch, cam.fov_x = (0, 2, -6), 0.1, -0.15, 1.2

    def render(self, engine, alpha):
        if not self.use_world:
            import math
            f = engine.frame
            f.eye, f.yaw, f.pitch, f.fov_x = (0, 2, -6), 0.1, -0.15, 1.2
            # The child: base (0,1,4) turned by 0.5, then (2,0,0) in its frame, scale 0.5.
            cx, cz = 0 + 2 * math.cos(0.5), 4 - 2 * math.sin(0.5)
            engine.draw(self.mesh, yaw_model(cx, 1, cz, 0.5, 0.5), tint=3)
            engine.draw(self.mesh, yaw_model(-2, 1, 6, 0.0), fade=0.4)


shots = []
for use_world in (True, False):
    shot = os.path.join(folder, f"scene_{use_world}.png")
    ae.run(Scene(use_world), argv=["--headless", "--frames", "2", "--screenshot", shot])
    shots.append(open(shot, "rb").read())
check(shots[0] == shots[1], "a world scene and the same scene through draw() give the same picture")


class Nodes(Game):
    def init(self, engine):
        w = engine.world
        a = w.spawn(name="a")
        b = w.spawn(parent=a, position=(1, 0, 0))
        c = w.spawn(parent=b, position=(0, 2, 0))
        a.position, a.yaw = (10, 0, 0), 0.0
        self.c_world = c.world_position
        self.names = (a.name, b.parent == a, c.parent == b)
        try:
            a.parent = c
            self.cycle = False
        except ValueError:
            self.cycle = True
        b.scale = (1, 2, 3)
        self.scale = b.scale
        b.yaw = 1.0
        self.yaw = round(b.yaw, 5)
        w.set_transforms([a], array("f", [5, 0, 0, 0, 0, 0, 1, 1, 1, 1]).tobytes())
        self.after_batch = a.position
        self.count_before = w.count
        a.destroy()
        self.count_after = w.count
        self.alive = (a.alive, b.alive, c.alive)
        try:
            b.position = (0, 0, 0)
            self.dead_raises = False
        except RuntimeError:
            self.dead_raises = True

    def render(self, engine, alpha):
        pass


n = Nodes()
ae.run(n, argv=["--headless", "--frames", "1"])
check(n.c_world == (11.0, 2.0, 0.0), "world_position goes through the hierarchy")
check(n.names == ("a", True, True), "name and parent")
check(n.cycle, "a parent that makes a cycle raises")
check(n.scale == (1.0, 2.0, 3.0) and n.yaw == 1.0, "scale tuple and yaw")
check(n.after_batch == (5.0, 0.0, 0.0), "set_transforms")
check(n.count_before == 3 and n.count_after == 0 and n.alive == (False, False, False), "destroy takes the children")
check(n.dead_raises, "a destroyed node raises")

# Rigs (docs/api.md §5): written with alia_pack, loaded from bytes, played by the runtime.
w = alia_pack.PackWriter()
w.add_mesh("body", *[list(array("f", CUBE[0])), list(array("H", CUBE[1]))], tri_palette=[1] * 12)
w.add_mesh("hat", *[list(array("f", CUBE[0])), list(array("H", CUBE[1]))], tri_palette=[2] * 12)
w.write(os.path.join(folder, "content", "gen", "bodies.apak"))
rig_pack = alia_pack.PackWriter()
rig_pack.add_rig("walker", {
    "bones": [{"name": "body", "parent": None, "offset": [0, 1, 0], "mesh": "body"},
              {"name": "arm", "parent": "body", "offset": [1, 0, 0], "mesh": "body"}],
    "slots": [{"bone": "arm", "offset": [0.5, 0, 0], "rot": [0, 0, 0], "variants": [None, "hat"]}],
    "attach": {"hand": {"bone": "arm", "offset": [1, 0, 0]}},
    "anims": [{"name": "swing", "loop": True, "keys": 2, "tracks": {"arm": [[0, 0, 0], [0, 0, 90]]},
               "events": [{"phase": 128, "type": "step", "foot": "left", "n": 2}]}],
})


class Rigs(Game):
    def init(self, engine):
        engine.load_pack(os.path.join(folder, "content", "gen", "bodies.apak"))
        self.rig = engine.load_rigs(rig_pack.bytes())["walker"]
        self.node = engine.world.spawn_rig(self.rig, position=(5, 0, 0))
        self.node.animator.play("swing", speed=128.0)  # half a loop per second
        self.node.slot_variant(0, 1)
        self.fired, self.steps = [], 0

    def fixed_update(self, engine, dt):
        self.steps += 1
        self.fired += self.node.animator.fired

    def render(self, engine, alpha):
        pass

    def shutdown(self, engine):  # handles only work while the engine is open
        r, n = self.rig, self.node
        self.result = (r.anims, r.bones, r.attach, r.events("swing"), len(r.pose("swing", 0)), n.slot(0).mesh)


rigs_game = Rigs()
ae.run(rigs_game, argv=["--headless", "--frames", "70"], fixed_hz=60)
anims, bones, attach, events, posed, slot_mesh = rigs_game.result
check(anims == ["swing"] and bones == ["body", "arm"] and attach == ["hand"], "Rig: anims, bones, attach")
check(events == [{"type": "step", "phase": 128, "anim": "swing", "foot": "left", "n": 2.0}], "Rig.events")
check(posed == 2, "Rig.pose: one transform per bone")
check(any(e["type"] == "step" for e in rigs_game.fired), "the runtime plays the animation and reports its events")
check(slot_mesh != 0, "slot_variant fits the rig's own variant by mesh name")

# Collision (docs/api.md §5).
coll_pack = alia_pack.PackWriter()
coll_pack.add_rig("target", {
    "bones": [{"name": "body", "parent": None, "offset": [0, 1, 0],
               "colliders": [{"type": "sphere", "c": [0, 0, 0], "r": 1.0, "layers": 1, "user": 3}]}],
    "slots": [{"bone": "body", "offset": [0, 0, -1], "rot": [0, 0, 0], "variants": [None, "hat"],
               "collider": {"type": "sphere", "c": [0, 0, 0], "r": 0.5, "layers": 2, "user": 0, "priority": 1}}],
    "anims": [{"name": "idle", "keys": 1}],
})


class Collision(Game):
    def init(self, engine):
        engine.load_pack(os.path.join(folder, "content", "gen", "bodies.apak"))
        w = engine.world
        ball = w.spawn(position=(0, 0, 10))
        ball.collider = ae.sphere((0, 0, 0), 1.0, layers=1, user=7)
        pill = w.spawn(position=(4, 0, 10))
        pill.collider = ae.capsule((0, -2, 0), (0, 2, 0), 0.5, layers=2, user=8)
        r = {}
        hit = w.ray_cast((0, 0, 0), (0, 0, 1))
        r["ray"] = (hit.node == ball, round(hit.distance, 4), hit.user, round(hit.point[2], 4))
        r["layers"] = w.ray_cast((0, 0, 0), (0, 0, 1), layers=2) is None
        r["exclude"] = w.ray_cast((0, 0, 0), (0, 0, 1), exclude=[ball]) is None
        r["overlap"] = [h.user for h in w.overlap_sphere((4, 2.5, 10), 0.2)]
        r["colliders"] = sorted(c.user for c in w.colliders())
        ball.collider_enabled = False
        r["disabled"] = w.ray_cast((0, 0, 0), (0, 0, 1)) is None
        r["collider_copy"] = (ball.collider.shape == ae.Shape.SPHERE, ball.collider.r)
        w.set_ground([0.0, 0.0, 0.0, 4.0], 2, 10.0, (0.0, 0.0))
        r["ground"] = (round(w.ground_height(10, 10), 4), round(w.ray_ground((10, 10, 10), (0, -1, 0)), 3))
        rig = engine.load_rigs(coll_pack.bytes())["target"]
        t = w.spawn_rig(rig, position=(0, 0, 30))
        r["rig_colliders"] = [c.collider.user for c in t.colliders("body")]
        before = w.ray_cast((0, 1, 20), (0, 0, 1), layers=2) is None
        t.slot_variant(0, 1)
        after = w.ray_cast((0, 1, 20), (0, 0, 1), layers=3)
        r["slot"] = (before, after is not None and after.layers == 2)
        self.r = r

    def render(self, engine, alpha):
        pass


col = Collision()
ae.run(col, argv=["--headless", "--frames", "1"])
r = col.r
check(r["ray"] == (True, 9.0, 7, 9.0), "ray_cast: node, distance, user, point")
check(r["layers"] and r["exclude"] and r["disabled"], "layers, exclude, disabled colliders")
check(r["overlap"] == [8] and r["colliders"] == [7, 8], "overlap_sphere and colliders()")
check(r["collider_copy"] == (True, 1.0), "node.collider reads back")
check(r["ground"] == (4.0, 6.0), "ground_height and ray_ground")
check(r["rig_colliders"] == [3], "a rig's bone colliders, from the pack")
check(r["slot"] == (True, True), "a slot's collider is off until a variant is fitted")


# Activity, parenting that keeps the world transform, attach points without spawning (docs/api.md §3.1).
class Attach(Game):
    def init(self, engine):
        w, r = engine.world, {}
        rig = engine.load_rigs(rig_pack.bytes())["walker"]
        horse = w.spawn_rig(rig, position=(5, 0, 0), yaw=0.5)
        rider = w.spawn(name="rider", position=(1, 2, 3))
        rider.collider = ae.sphere((0, 0, 0), 0.5)
        hand = horse.attach_point("hand")
        rider.set_parent(hand, keep_world=True)
        r["stays"] = all(abs(a - b) < 1e-5 for a, b in zip(rider.world_position, (1, 2, 3)))
        horse.position = (6, 0, 0)
        r["follows"] = abs(rider.world_position[0] - 2) < 1e-5
        rider.set_parent(None, keep_world=True)
        horse.position = (9, 0, 0)
        r["detached"] = rider.parent is None and abs(rider.world_position[0] - 2) < 1e-5
        rider.set_parent(hand)
        rider.position = (0, 0, 0)  # snap onto the attach point
        r["snap"] = all(abs(a - b) < 1e-5 for a, b in zip(rider.world_position, hand.world_position))
        # Rig.attach: the same point in rig space, without a node.
        local = rig.attach_position("hand", "swing", 0)
        r["attach"] = all(abs(a - b) < 1e-5 for a, b in zip(local, (2, 1, 0)))
        horse.active = False
        r["off"] = (not rider.active_in_hierarchy, rider.active, w.ray_cast((0, 5, 0), (1, -2, 0), 100) is None)
        horse.active = True
        r["on"] = rider.active_in_hierarchy
        r["spawn_off"] = not w.spawn(active=False).active
        self.r = r

    def render(self, engine, alpha):
        pass


att = Attach()
ae.run(att, argv=["--headless", "--frames", "1"])
r = att.r
check(r["stays"] and r["follows"] and r["detached"], "set_parent(keep_world=True): stays put, then follows; detach too")
check(r["snap"], "set_parent keeps the local transform; zero it to snap onto the attach point")
check(r["attach"], "Rig.attach_position: an attach point in rig space for a pose")
check(r["off"] == (True, True, True) and r["on"], "active: a parent off takes its children out (no colliders), flags kept")
check(r["spawn_off"], "spawn(active=False)")

# Rigid IK (docs/api.md §4.1): applied by the runtime after fixed_update, so the next step sees it.
ik_pack = alia_pack.PackWriter()
ik_pack.add_rig("person", {
    "bones": [{"name": "base", "parent": None, "offset": [0, 0, 0], "mesh": None},
              {"name": "upper", "parent": "base", "offset": [0, 2, 0], "mesh": None},
              {"name": "lower", "parent": "upper", "offset": [0, -1, 0], "mesh": None},
              {"name": "head", "parent": "base", "offset": [0, 3, 0], "mesh": None}],
    "attach": {"hand": {"bone": "lower", "offset": [0, -1, 0]}},
    "anims": [{"name": "rest", "loop": True, "keys": 1, "tracks": {}}],
})


class Ik(Game):
    def init(self, engine):
        w = engine.world
        self.person = w.spawn_rig(engine.load_rigs(ik_pack.bytes())["person"])
        self.ball = w.spawn(position=(1.2, 2.5, 0.5))
        self.reach = self.person.two_bone("upper", "lower", "hand", self.ball, pole=(0, 0, 1))
        self.look = self.person.look_at("head", (5, 3, 0), max_angle=math.radians(30))
        self.steps, self.r = 0, {}

    def fixed_update(self, engine, dt):
        self.steps += 1
        p = self.person
        if self.steps == 2:
            hand = p.attach_point("hand").world_position
            self.r["reach"] = math.dist(hand, self.ball.world_position) < 1e-3
            head = p.bone("head")
            ahead = engine.world.spawn(parent=head, position=(0, 0, 1)).world_position
            self.r["look"] = abs(math.atan2(ahead[0], ahead[2] - 0) - math.radians(30)) < 1e-3
            self.r["handle"] = (self.reach.target == self.ball, self.look.target, self.look.weight)
            self.ball.position = (0.5, 1.5, 1.0)  # moved: followed next step
        elif self.steps == 3:
            hand = p.attach_point("hand").world_position
            self.r["follow"] = math.dist(hand, (0.5, 1.5, 1.0)) < 1e-3
            self.reach.remove()
            try:
                self.reach.weight = 0.5
                self.r["removed"] = False
            except RuntimeError:
                self.r["removed"] = True

    def render(self, engine, alpha):
        pass


ik_game = Ik()
ae.run(ik_game, argv=["--headless", "--frames", "4"])
r = ik_game.r
check(r["reach"] and r["follow"], "two_bone: the hand reaches the ball, and follows it")
check(r["look"], "look_at with max_angle")
check(r["handle"][0] and all(abs(a - b) < 1e-6 for a, b in zip(r["handle"][1], (5, 3, 0))) and r["handle"][2] == 1.0,
      "Ik: target (a node or a point) and weight read back")
check(r["removed"], "Ik.remove(); the handle stops working")


# Particles (docs/api.md §6): templates on the engine, emitters are nodes, simulated by the runtime.
class Sparks(Game):
    def init(self, engine):
        w = engine.world
        cube = engine.create_mesh(ae.Mesh(*CUBE, tri_palette=bytes([2] * 12)))
        w.set_ground([0.0] * 4, 2, 100.0, (-50.0, -50.0))
        self.fx = engine.particles(cube, lifetime=(0.5, 1.0), speed=(1, 3), size=0.2, gravity=1.0, shape="cone",
                                   angle=0.4, ground="bounce", fade_over_life=[(0.6, 0), (1, 1)], max_particles=200)
        self.fountain = w.spawn_emitter(self.fx, position=(0, 1, 5), rate=60, seed=7)
        self.puff = w.spawn_emitter(self.fx, seed=8)
        self.puff.burst(12, at=(2, 3, 5), tint=5)
        self.counts = []

    def fixed_update(self, engine, dt):
        self.counts.append((self.fountain.count, self.puff.count))

    def render(self, engine, alpha):
        pass

    def shutdown(self, engine):
        self.stats = engine.stats


sp = Sparks()
ae.run(sp, argv=["--headless", "--frames", "30"], fixed_hz=60)
check(sp.counts[0] == (0, 12) and 25 <= sp.counts[-1][0] <= 30, "rate fills the fountain; burst(12, at=, tint=)")
check(sp.stats["instances"] >= 20, "particles are drawn as instances")
try:
    ae.Engine(headless=True).particles(1, shape="triangle")
    bad_shape = False
except ValueError:
    bad_shape = True
check(bad_shape, "an unknown shape is refused")

# Saves, input actions, rumble (docs/api.md §7).
save_dir = tempfile.mkdtemp()
ae.save.set_dir(save_dir)
for text in (b"one", b"two", b"three"):
    ae.save.write("slot1", text)
check(ae.save.read("slot1") == b"three" and ae.save.read("slot1", 2) == b"one" and ae.save.versions("slot1") == [0, 1, 2],
      "save.write keeps the two previous versions")
ae.save.rollback("slot1", 1)
check(ae.save.read("slot1") == b"two" and ae.save.names() == ["slot1"], "save.rollback")
try:
    ae.save.write("../escape", b"x")
    named = False
except OSError as error:
    named = error.errno == 22
check(named and ae.save.read("missing") is None, "bad names refused (EINVAL); a missing save reads None")
sdk = os.environ.get("ALIA_SDK_PYTHON", "")  # the Alia Pad cart SDK's Python modules, if at hand
if sdk and os.path.isdir(os.path.join(sdk, "alia")):  # the SDK's Python alia.save reads what the engine wrote
    sys.path.insert(0, sdk)
    from alia import save as system_save
    os.environ["ALIA_SAVE_DIR"] = save_dir
    check(system_save.read("slot1") == b"two", "the cart SDK's alia.save reads the engine's saves")
ae.save.delete("slot1")
check(ae.save.names() == [], "save.delete")

actions_engine = ae.Engine(headless=True)
act = actions_engine.actions
act.bind("shoot", ae.SOUTH)
act.bind("hook", "NORTH", ae.R1)
act.bind_axis("move", "left", dead_zone=0.15)
table = act.bindings()
check(table == {"shoot": ["SOUTH"], "hook": ["NORTH", "R1"], "move": {"stick": "left", "dead_zone": 0.15000000596046448}},
      "Actions.bindings(): a JSON-friendly table")
act.clear()
act.load(table)
check(act.bindings() == table and not act.held("shoot") and act.axis("move") == (0.0, 0.0), "Actions.load() restores it")
check(actions_engine.rumble(0.5, 0.5, 100) is False, "rumble: False when there is nothing to rumble (headless)")

# Defaults a game gets without setting anything (the docs test found the old ones unusable).
fresh = ae.Engine(headless=True)
fresh.frame.eye, fresh.frame.yaw, fresh.frame.pitch = (0.0, 1.5, 0.0), 0.0, 0.0
shots = []
for index in (6, 12):  # red and blue in the default palette; the old all-grey one drew them the same
    m = fresh.create_mesh(ae.Mesh(*CUBE, tri_palette=bytes([index] * 12)))
    fresh.draw(m, (1, 0, 0, 0, 0, 1, 0, 1.5, 0, 0, 1, 6))
    shots.append(os.path.join(tempfile.mkdtemp(), "fresh.png"))
    fresh.end_frame()
    fresh.screenshot(shots[-1])
check(open(shots[0], "rb").read() != open(shots[1], "rb").read(), "a default palette: two indices don't draw the same")
travel = fresh.frame.sun.travel
check(abs(travel[0]) > 0.1 and travel[1] < 0, "the default sun comes in at a slant, not straight down")
fresh.close()
new_dir = os.path.join(tempfile.mkdtemp(), "not", "there", "yet")
ae.save.set_dir(new_dir)
ae.save.write("made", b"1")
check(os.path.isdir(new_dir) and ae.save.read("made") == b"1", "save.set_dir makes the directory")

# Language (docs/api.md §7).
check(ae.choose_language(["en", "zh"], "zh-CN") == "zh" and ae.localized({"en": "START", "zh": "开始"}, "zh-TW") == "开始"
      and ae.localized({"en": "START"}, "ja") == "START" and ae.localized({}, "en") is None and isinstance(ae.language(), str),
      "language, choose_language, localized")

# The mixer (docs/api.md §7): headless there is no device, mix() renders by hand.
import io
import math
import wave


def tone_wav(hz, seconds, rate=22050):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1), w.setsampwidth(2), w.setframerate(rate)
        w.writeframes(array("h", [int(12000 * math.sin(2 * math.pi * hz * i / rate)) for i in range(int(seconds * rate))]).tobytes())
    return buf.getvalue()


au = actions_engine.audio
beep, theme = au.load(tone_wav(880, 0.05)), au.load(tone_wav(220, 1.0))
check(au.device is None and abs(beep.seconds - 0.05) < 1e-3 and beep.rate == 22050 and beep.channels == 1,
      "audio.load: a WAV from bytes; headless: no device")
voice = au.play(beep, volume=0.8, pan=-0.5)
samples = array("f", au.mix(1200))
check(voice.playing and max(abs(x) for x in samples) > 0.1, "play, then mix(): it sounds")
au.mix(2400)
check(not voice.playing, "a 50 ms sound has ended after 75 ms")
au.play_music(theme, volume=0.5)
au.mix(480)
au.play_music(theme, volume=0.5)
check(au.voices == 1, "play_music every step: the same track keeps playing, once")
au.music_volume = 0.0
check(max(abs(x) for x in array("f", au.mix(480))) < 0.01, "music_volume 0: silent")
au.play_music(None, fade=0)
au.mix(48)
check(au.voices == 0, "play_music(None): silence")
try:
    au.load(b"not a wav")
    loaded = True
except RuntimeError:
    loaded = False
check(not loaded, "not a WAV: refused")
actions_engine.close()

sys.exit(1 if failures else 0)
