"""alia_kit (docs/api.md §7.1): the helpers games share, against a headless engine."""

import math
import sys

import alia_engine as ae
import alia_kit as kit

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


e = ae.Engine(headless=True)
w = e.world

# stick_to_world: up on the stick walks away from the camera; right is screen-right.
fx, fz = kit.stick_to_world((0.0, -1.0), 0.0)
rx, rz = kit.stick_to_world((1.0, 0.0), 0.0)
check(abs(fz - 1) < 1e-9 and abs(fx) < 1e-9 and abs(rx + 1) < 1e-9, "stick_to_world: up is ahead (+Z at yaw 0), right is -X")
fx, fz = kit.stick_to_world((0.0, -1.0), math.pi / 2)
check(abs(fx - 1) < 1e-9, "stick_to_world turns with the camera")

# GroundMover: on the ground, out of colliders that reach its height, inside its bounds.
w.set_ground([0.0, 0.0, 2.0, 2.0], 2, 10.0, (-5.0, -5.0))  # a slope along z
post = w.spawn(position=(0, 0, 2))
post.collider = ae.capsule((0, 0, 0), (0, 3, 0), 0.5, layers=1)
high = w.spawn(position=(0, 10, -2))
high.collider = ae.sphere((0, 0, 0), 1.0, layers=1)  # far above: not in the way
m = kit.GroundMover(w, position=(0.0, 0.0), radius=0.4, speed=4.0, layers=1, bounds=(-4, -4, 4, 4))
for _ in range(60):
    m.move((0.0, 1.0), 1 / 60)  # walk at the post
check(abs(m.z - (2 - 0.9)) < 1e-6 and abs(m.y - w.ground_height(m.x, m.z)) < 1e-6, "GroundMover stops at a capsule and stays on the ground")
m2 = kit.GroundMover(w, position=(0.0, -3.0), radius=0.4, layers=1)
for _ in range(20):
    m2.move((0.0, 1.0), 1 / 60)
check(m2.z > -3.0 + 1.0, "a collider above its height doesn't stop it")
for _ in range(200):
    m.move((1.0, 0.0), 1 / 60)
check(m.x == 4 and abs(m.yaw - math.pi / 2) < 0.01 and m.moving == 1.0, "bounds; it faces where it goes; moving is 0..1")

# FollowCamera: behind, above, to the right; ray through the middle of the screen.
cam = kit.FollowCamera(w.camera, distance=5.0, height=2.0, shoulder=1.0, pitch=-0.2)
cam.follow(w, (0.0, 0.0, 0.0))
x, y, z = w.camera.position
check(abs(z + 5) < 1e-5 and abs(x + 1) < 1e-5 and y >= 2.0 - 1e-5, "FollowCamera: behind, over the right shoulder")
origin, d = cam.ray()
check(abs(d[1] - math.sin(-0.2)) < 1e-6 and abs(math.hypot(*d) - 1) < 1e-6, "FollowCamera.ray along its view")
cam.turn(1.0, 0.5)
check(abs(cam.yaw + 0.5 * cam.turn_speed) < 1e-9, "turn: the stick to the right turns right (yaw falls)")

# Gaze: turns at most `speed`, and never further than `limit` from the body's heading.
g = kit.Gaze(rate=100.0, speed=2.0, reach=1.0)
g.update((0, 0, 0), (0, 0, 10), 1 / 60)
g.update((0, 0, 0), (10, 0, 0), 1 / 60)
check(abs(g.yaw - 2.0 / 60) < 1e-6, "Gaze turns at most speed x dt a step")
g = kit.Gaze(rate=100.0, speed=100.0)
for _ in range(10):
    g.update((0, 0, 0), (0, 0, -10), 1 / 60, forward=0.0, limit=math.radians(70))
check(abs(abs(g.yaw) - math.radians(70)) < 1e-6, "a target behind: the gaze stops at the limit")
s = kit.Smoothed((0, 0, 0), rate=10.0)
s.update((1, 0, 0), 0.1)
check(abs(s.value[0] - (1 - math.exp(-1))) < 1e-9, "Smoothed closes the gap by 1 - e^-(rate dt)")

# world_to_screen, text, Pool.
w.camera.position, w.camera.yaw, w.camera.pitch = (0, 0, 0), 0.0, 0.0
sx, sy = kit.world_to_screen(e, (0, 0, 10))
check(abs(sx - e.scene_size[0] / 2) < 1e-3 and abs(sy - e.scene_size[1] / 2) < 1e-3, "world_to_screen: straight ahead is the middle")
check(kit.world_to_screen(e, (0, 0, -10)) is None and kit.world_to_screen(e, (1, 0, 10))[0] < e.scene_size[0] / 2,
      "behind: None; +X is on the left")
check(kit.text_width("ABC", 2) == 34, "text_width: 6 px a character, less the last gap")
made = []
pool = kit.Pool(lambda: made.append(1) or len(made), on_release=lambda o: None)
a, b = pool.get(), pool.get()
pool.release(a)
check(pool.get() == a and len(made) == 2, "Pool reuses what was released")

# Shapes: every triangle faces out (counter-clockwise from outside), none without area, one colour per triangle.
real_mesh = kit._mesh
kit._mesh = lambda p, i, c, material=None: (p, i, c)  # the data, not an engine mesh


def faces_out(shape, outward):
    p, i, c = shape
    ok = len(c) == len(i) // 3
    for t in range(0, len(i), 3):
        a, b, d = ([p[3 * v + k] for k in range(3)] for v in i[t:t + 3])
        u, v = [b[k] - a[k] for k in range(3)], [d[k] - a[k] for k in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        mid = [(a[k] + b[k] + d[k]) / 3 for k in range(3)]
        ok = ok and math.sqrt(sum(x * x for x in n)) > 1e-9 and sum(x * y for x, y in zip(n, outward(mid))) > 0
    return ok


check(faces_out(kit.box(2, 1, 3, centre=(1, 2, 3)), lambda m: (m[0] - 1, m[1] - 2, m[2] - 3)), "box: 12 triangles facing out")
check(len(kit.box(colour=(1, 2, 3, 4, 5, 6))[2]) == 12, "box: a colour per face")
check(faces_out(kit.sphere(2.0, rings=6, segments=10), lambda m: m) and len(kit.sphere()[1]) == 300, "sphere: 100 triangles facing out")
check(faces_out(kit.cylinder(0.5, 2.0), lambda m: (m[0], 0.0 if 0 < m[1] < 2 else m[1] - 1, m[2])), "cylinder: sides and caps face out")
pl = kit.plane(4, 4, 1, divisions=2, checker=2)
check(faces_out(pl, lambda m: (0, 1, 0)) and pl[2] == [1, 1, 2, 2, 2, 2, 1, 1], "plane: faces up, checkered")
kit._mesh = real_mesh

h = kit.hills(seed=3, amplitude=2.0)
check(h(1.5, 2.5) == kit.hills(seed=3, amplitude=2.0)(1.5, 2.5) and h(1.5, 2.5) != kit.hills(seed=4)(1.5, 2.5)
      and max(abs(h(x * 1.7, x * 0.9)) for x in range(-40, 40)) <= 2.0, "hills: the same for a seed, within the amplitude")
t = kit.Terrain(e, size=40, cell=4, height=h)
check(t.n == 11 and abs(w.ground_height(4.0, -8.0) - h(4.0, -8.0)) < 1e-5 and t.node.mesh != 0,
      "Terrain: one mesh, and the same heights as the world's ground")
kit.environment(e, "night")
check(e.frame.sky_top == kit.ENVIRONMENTS["night"]["sky_top"] and abs(e.frame.sun.ambient - 0.3) < 1e-6, "environment sets e.frame")

# Numbers, angles, vectors, noise.
near = lambda a, b: abs(a - b) < 1e-9  # noqa: E731
check(kit.clamp(5, 0, 1) == 1 and kit.lerp(2, 4, 0.25) == 2.5 and kit.inverse_lerp(2, 4, 3) == 0.5
      and kit.remap(15, 10, 20, 0, 100) == 50 and kit.remap(30, 10, 20, 0, 100) == 100, "clamp, lerp, inverse_lerp, remap")
check(kit.smoothstep(0, 1, -1) == 0 and kit.smoothstep(0, 1, 0.5) == 0.5 and kit.smoothstep(0, 1, 2) == 1, "smoothstep")
check(kit.approach(0.0, 1.0, 0.3) == 0.3 and kit.approach(0.9, 1.0, 0.3) == 1.0 and kit.approach(1.0, -1.0, 0.5) == 0.5,
      "approach: a steady step, never past the target")
v1 = kit.damp(0.0, 1.0, 5.0, 0.1)
v2 = kit.damp(kit.damp(0.0, 1.0, 5.0, 0.05), 1.0, 5.0, 0.05)
check(near(v1, v2) and near(v1, 1 - math.exp(-0.5)), "damp: the same however the time is cut up")
check(near(kit.wrap_angle(3 * math.pi / 2), -math.pi / 2) and near(kit.angle_between(math.radians(170), math.radians(-170)),
      math.radians(20)), "wrap_angle, angle_between: the short way round")
check(near(kit.turn_towards(math.radians(170), math.radians(-170), 0.1), math.radians(170) + 0.1)
      and near(kit.turn_towards(0.0, 0.05, 0.1), 0.05), "turn_towards: across pi, and stops at the target")
check(near(kit.yaw_to((0, 0, 0), (0, 5, 3)), 0.0) and near(kit.yaw_to((0, 0), (1, 0)), math.pi / 2), "yaw_to: +Z is 0, +X is pi/2")
yaw, pitch = kit.aim((0, 1, 1))
check(near(yaw, 0) and near(pitch, -math.pi / 4) and near(kit.aim((0, 1, 1), camera=True)[1], math.pi / 4),
      "aim: up and ahead is pitch -45 degrees for an object, +45 for the camera")
probe = w.spawn(position=(0, 0, 0))
probe.set_rotation(*kit.aim((1.0, -2.0, 0.5)), 0.0)
fwd = kit.normalize(kit.sub(w.spawn(position=(0, 0, 1), parent=probe).world_position, probe.world_position))
check(all(abs(a - b) < 1e-5 for a, b in zip(fwd, kit.normalize((1.0, -2.0, 0.5)))), "aim: the object's +Z points along the direction")
check(kit.add((1, 2, 3), (1, 1, 1)) == (2, 3, 4) and kit.sub((1, 2, 3), (1, 1, 1)) == (0, 1, 2) and kit.scale((1, 2), 2) == (2, 4)
      and kit.dot((1, 2, 3), (4, 5, 6)) == 32 and kit.cross((1, 0, 0), (0, 1, 0)) == (0, 0, 1), "add, sub, scale, dot, cross")
check(kit.length((3, 4, 0)) == 5 and kit.normalize((0, 0, 0)) == (0, 0, 0) and kit.normalize((0, 0, 2)) == (0, 0, 1)
      and kit.distance((0, 0, 0), (2, 3, 6)) == 7 and kit.distance_xz((0, 9, 0), (3, 0, 4)) == 5
      and kit.lerp3((0, 0, 0), (2, 4, 6), 0.5) == (1, 2, 3), "length, normalize, distance, distance_xz, lerp3")
samples = [kit.noise(x * 0.37, z * 0.53, 7) for x in range(30) for z in range(30)]
check(max(samples) <= 1 and min(samples) >= -1 and max(samples) - min(samples) > 1, "noise is within -1..1 and varies")
check(kit.noise(1.3, 2.7, 7) == kit.noise(1.3, 2.7, 7) and kit.noise(1.3, 2.7, 7) != kit.noise(1.3, 2.7, 8)
      and abs(kit.noise(1.3) - kit.noise(1.301)) < 0.01, "noise: the same for a seed, different across seeds, smooth")
check(all(abs(kit.fbm(x * 0.29, x * 0.41, 2, 4)) <= 1 for x in range(200)), "fbm stays within -1..1")
yaw, pitch = 0.7, -0.3
check(all(abs(a - b) < 1e-9 for a, b in zip(kit.aim(kit.forward(yaw, pitch), camera=True), (yaw, pitch)))
      and all(abs(a - b) < 1e-9 for a, b in zip(kit.right(0.0), (-1.0, 0.0, 0.0))), "forward is the inverse of aim(camera=True); right at yaw 0 is -X")
check(kit.pal.RED == 6 and kit.pal.GRASS == 20 and kit.pal.NEAR_WHITE == 31 and kit.WHITE == 0xFFFFFFFF, "palette names, HUD colours")
e.close()
sys.exit(1 if failures else 0)
