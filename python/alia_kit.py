"""alia_kit: small reusable pieces for games on alia-engine (docs/api.md §7.1), in plain Python next to the engine
module. None of them is a base class: a game holds one (a camera, a mover) and calls it every step, the same way
it holds a rig or an emitter (docs/api.md §9, decisions 5 and 7). Everything here is what the sample, the sandbox
and Megalith each wrote for themselves before.

    import alia_kit as kit
    cam = kit.FollowCamera(e.world.camera)              # third person over the shoulder
    mover = kit.GroundMover(e.world, radius=0.4)        # a character controller: ground, colliders, bounds
    mover.move(kit.stick_to_world(act.axis("move"), cam.yaw), dt)
    cam.turn(act.axis("turn")[0], dt); cam.follow(e.world, mover.position, mover.yaw, dt)
"""

import math
from array import array

__all__ = ["FollowCamera", "FlyCamera", "FirstPersonCamera", "stick_to_world", "GroundMover", "Smoothed", "Gaze",
           "world_to_screen", "text_width", "say", "centred", "Pool",
           "box", "sphere", "cylinder", "plane", "hills", "Terrain", "environment", "ENVIRONMENTS",
           "wrap_angle", "angle_between", "turn_towards", "aim", "yaw_to", "forward", "right",
           "clamp", "lerp", "inverse_lerp", "remap", "smoothstep", "approach", "damp",
           "add", "sub", "scale", "dot", "cross", "length", "normalize", "distance", "distance_xz", "lerp3",
           "noise", "fbm", "WHITE", "BLACK", "SHADOW", "pal"]


# ---- numbers and angles --------------------------------------------------------------------------------

def clamp(v, lo=0.0, hi=1.0):
    return lo if v < lo else hi if v > hi else v


def lerp(a, b, t):
    """a at t 0, b at t 1 (not clamped)."""
    return a + (b - a) * t


def inverse_lerp(a, b, v):
    """Where v is between a and b: 0 at a, 1 at b (not clamped; 0 when a == b)."""
    return (v - a) / (b - a) if b != a else 0.0


def remap(v, a, b, c, d, clamped=True):
    """v from the range a..b to the range c..d (clamped to it unless clamped=False)."""
    t = inverse_lerp(a, b, v)
    return lerp(c, d, clamp(t) if clamped else t)


def smoothstep(a, b, v):
    """0 below a, 1 above b, an S-curve between (fades, blends)."""
    t = clamp(inverse_lerp(a, b, v))
    return t * t * (3 - 2 * t)


def approach(v, target, step):
    """Move v towards target by at most step (a steady speed: step = speed x dt); never overshoots."""
    return min(v + step, target) if v < target else max(v - step, target)


def damp(v, target, rate, dt):
    """Ease v towards target, closing 1 - e^(-rate dt) of the gap: the same at any step length (Smoothed for a
    number; rate per s, about 3 / the time to get most of the way)."""
    return target + (v - target) * math.exp(-rate * dt)


def wrap_angle(a):
    """An angle into -pi..pi (rad)."""
    return (a + math.pi) % (2 * math.pi) - math.pi


def angle_between(a, b):
    """The signed shortest turn from angle a to angle b (rad, -pi..pi): positive turns left (yaw rises)."""
    return wrap_angle(b - a)


def turn_towards(a, target, step):
    """Turn angle a towards target the short way round by at most step (rad); the result is wrapped."""
    d = angle_between(a, target)
    return wrap_angle(target if abs(d) <= step else a + math.copysign(step, d))


def yaw_to(origin, target):
    """The yaw that faces from origin towards target on the ground (x, z of either a 2- or 3-tuple: (x, y, z) or
    (x, z) both work; the last is z)."""
    return math.atan2(target[0] - origin[0], target[-1] - origin[-1])


def aim(direction, camera=False):
    """(yaw, pitch) that point +Z along `direction`: for an object, set_rotation(yaw, pitch, 0) (its pitch is
    positive with +Z down); camera=True gives the camera's pitch (positive up). docs/api.md §0.1."""
    x, y, z = direction
    pitch = math.atan2(y, math.hypot(x, z))
    return math.atan2(x, z), pitch if camera else -pitch


# ---- vectors: tuples in, tuples out -------------------------------------------------------------------

def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def sub(a, b):
    """a - b: the vector from b to a."""
    return tuple(x - y for x, y in zip(a, b))


def scale(v, k):
    return tuple(x * k for x in v)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def length(v):
    return math.sqrt(sum(x * x for x in v))


def normalize(v, fallback=None):
    """v at length 1; a zero vector gives `fallback` (default: the zero vector back)."""
    n = length(v)
    if n < 1e-12:
        return tuple(v) if fallback is None else fallback
    return tuple(x / n for x in v)


def distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def distance_xz(a, b):
    """The distance on the ground, ignoring height ((x, y, z) or (x, z): the last is z)."""
    return math.hypot(a[0] - b[0], a[-1] - b[-1])


def lerp3(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


# ---- noise --------------------------------------------------------------------------------------------

def _lattice(seed, x, z):
    h = ((seed * 2654435761) ^ (x * 374761393) ^ (z * 668265263)) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 32767.5 - 1.0


def noise(x, z=0.0, seed=0):
    """Smooth value noise, -1..1, about one bump per unit; the same for the same (x, z, seed). Give only x for a 1D
    wobble over time (noise(t * 4, seed=3): camera shake, a flame's flicker, a drift)."""
    x0, z0 = math.floor(x), math.floor(z)
    fx, fz = x - x0, z - z0
    fx, fz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
    x0, z0 = int(x0), int(z0)
    a, b = _lattice(seed, x0, z0), _lattice(seed, x0 + 1, z0)
    c, d = _lattice(seed, x0, z0 + 1), _lattice(seed, x0 + 1, z0 + 1)
    top, bottom = a + (b - a) * fx, c + (d - c) * fx
    return top + (bottom - top) * fz


def fbm(x, z=0.0, seed=0, octaves=3):
    """Layers of noise (each twice as fine, half as strong), still -1..1: rougher, more natural shapes."""
    total = weight = 0.0
    for o in range(octaves):
        k = 2 ** o
        total += noise(x * k, z * k, seed + 1013 * o) / k
        weight += 1 / k
    return total / weight


# ---- HUD colours (0xRRGGBBAA) and the default palette's indices ---------------------------------------

WHITE, BLACK, SHADOW = 0xFFFFFFFF, 0x000000FF, 0x000000C0



class pal:
    """The default palette's indices by name (meshes, tint, particles): kit.pal.RED is 6. Only for the default
    palette: e.set_palette changes the colours, not these numbers. (HUD colours are 0xRRGGBBAA: kit.WHITE.)"""
    BLACK, WHITE, LIGHT_GREY, GREY, DARK_GREY, CHARCOAL = range(0, 6)
    RED, ORANGE, YELLOW, LIME, GREEN, TEAL = range(6, 12)
    BLUE, NAVY, VIOLET, MAGENTA, PINK, BROWN = range(12, 18)
    TAN, CREAM, GRASS, FOREST, STONE, SLATE = range(18, 24)
    SKY, DUSK_BLUE, GLOW_YELLOW, CORAL, GLOW_CYAN, GLOW_LIME = range(24, 30)
    GLOW_PINK, NEAR_WHITE = 30, 31


def forward(yaw, pitch=0.0):
    """The unit direction a camera at (yaw, pitch) looks along: +Z ahead at yaw 0, pitch positive up (docs/api.md
    §0.1). The inverse of aim(d, camera=True)."""
    return (math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))


def right(yaw):
    """Screen-right on the ground plane for a camera at `yaw`, unit length (+X is on the left when looking along
    +Z, so at yaw 0 this is -X)."""
    return (-math.cos(yaw), 0.0, math.sin(yaw))


_forward, _right = forward, right  # what the classes below use: a parameter named `forward` can't hide it


# ---- cameras ------------------------------------------------------------------------------------------------

class FollowCamera:
    """Third person: behind and above a target, a little to the side (so the crosshair isn't on the character),
    turned by a stick, kept above the ground; optionally easing back behind the target's heading when the stick
    is let go (recenter: the share of the angle made up per second; 0 = never). .yaw is where it looks now."""

    def __init__(self, camera, distance=6.5, height=3.0, shoulder=0.9, pitch=-0.22, turn_speed=2.2,
                 clearance=1.0, recenter=0.0, recenter_after=0.8):
        self.camera, self.distance, self.height, self.shoulder = camera, distance, height, shoulder
        self.pitch, self.turn_speed, self.clearance = pitch, turn_speed, clearance
        self.recenter, self.recenter_after = recenter, recenter_after
        self.yaw, self._idle = 0.0, 0.0

    def turn(self, x, dt):
        """x: the stick, -1..1 (right turns right)."""
        self.yaw = wrap_angle(self.yaw - x * self.turn_speed * dt)
        self._idle = 0.0 if abs(x) > 1e-3 else self._idle + dt

    def follow(self, world, target, target_yaw=None, dt=0.0):
        """Place the camera for a target at `target` (x, y, z) facing `target_yaw`."""
        if self.recenter > 0 and target_yaw is not None and self._idle > self.recenter_after:
            self.yaw = wrap_angle(self.yaw + wrap_angle(target_yaw - self.yaw) * min(1.0, self.recenter * dt))
        f, r = _forward(self.yaw), _right(self.yaw)
        x = target[0] - f[0] * self.distance + r[0] * self.shoulder
        z = target[2] - f[2] * self.distance + r[2] * self.shoulder
        y = max(target[1] + self.height, world.ground_height(x, z) + self.clearance)
        self.camera.position, self.camera.yaw, self.camera.pitch = (x, y, z), self.yaw, self.pitch

    def ray(self, lift=0.0):
        """(origin, direction) through the middle of the screen (`lift` rad above it), for shots and picking."""
        return self.camera.position, _forward(self.yaw, self.pitch + lift)


class FlyCamera:
    """A free camera: move along the view (level: on the ground plane, the way the sandbox's does), strafe, rise
    and sink, look with a stick."""

    def __init__(self, camera, speed=8.0, look_speed=1.6, min_pitch=-1.4, max_pitch=1.4, level=False):
        self.camera, self.speed, self.look_speed = camera, speed, look_speed
        self.min_pitch, self.max_pitch, self.level = min_pitch, max_pitch, level

    def update(self, move, look, rise, dt, floor=None):
        """move, look: sticks (x right, y down); rise: -1..1; floor: the lowest y allowed."""
        c = self.camera
        c.yaw = wrap_angle(c.yaw - look[0] * self.look_speed * dt)
        c.pitch = max(self.min_pitch, min(self.max_pitch, c.pitch - look[1] * self.look_speed * dt))
        f, r = _forward(c.yaw, 0.0 if self.level else c.pitch), _right(c.yaw)
        step = self.speed * dt
        x, y, z = c.position
        x += (f[0] * -move[1] + r[0] * move[0]) * step
        y += (f[1] * -move[1] + rise) * step
        z += (f[2] * -move[1] + r[2] * move[0]) * step
        c.position = (x, y if floor is None else max(floor, y), z)


class FirstPersonCamera:
    """Eyes at a height above a moving point; the stick looks around (yaw unlimited, pitch clamped)."""

    def __init__(self, camera, eye_height=1.6, look_speed=2.0, min_pitch=-1.2, max_pitch=1.2):
        self.camera, self.eye_height, self.look_speed = camera, eye_height, look_speed
        self.min_pitch, self.max_pitch = min_pitch, max_pitch
        self.yaw, self.pitch = 0.0, 0.0

    def update(self, position, look, dt):
        self.yaw = wrap_angle(self.yaw - look[0] * self.look_speed * dt)
        self.pitch = max(self.min_pitch, min(self.max_pitch, self.pitch - look[1] * self.look_speed * dt))
        x, y, z = position
        self.camera.position, self.camera.yaw, self.camera.pitch = (x, y + self.eye_height, z), self.yaw, self.pitch

    def ray(self):
        return self.camera.position, _forward(self.yaw, self.pitch)


def stick_to_world(stick, yaw):
    """A stick (x right, y down, as Actions.axis gives it) turned to the ground plane for a camera at `yaw`:
    (dx, dz), length 0..1. Up on the stick walks away from the camera."""
    x, y = stick
    f, r = _forward(yaw), _right(yaw)
    return (f[0] * -y + r[0] * x, f[2] * -y + r[2] * x)


# ---- a character controller ------------------------------------------------------------------------------------

class GroundMover:
    """Walks on the world's ground (World.ground_height) at up to `speed`, turns to face where it goes, and is
    pushed out of the colliders on `layers` (spheres and capsules, as circles on the ground plane, when they reach
    its height), inside `bounds` ((min_x, min_z, max_x, max_z)). No physics: a kinematic controller, like most
    handheld games want. `position` is given on the ground, (x, z); .position reads back (x, y, z) with y on the
    ground; .yaw is where it faces; .moving is how much of its speed it used this step (0..1, for a walk cycle)."""

    def __init__(self, world, position=(0.0, 0.0), radius=0.4, height=1.8, speed=4.5, turn_rate=10.0, layers=1,
                 bounds=None):
        self.world, self.radius, self.height, self.speed = world, radius, height, speed
        self.turn_rate, self.layers, self.bounds = turn_rate, layers, bounds
        self.x, self.z = position
        self.y = world.ground_height(self.x, self.z)
        self.yaw, self.moving = 0.0, 0.0  # moving: 0..1 of full speed, this step (for the walk animation)

    @property
    def position(self):
        return (self.x, self.y, self.z)

    def move(self, direction, dt):
        """direction: (dx, dz) of length 0..1 (stick_to_world); pushed out of what it walks into."""
        dx, dz = direction
        amount = min(1.0, math.hypot(dx, dz))
        self.moving = amount
        if amount > 1e-3:
            self.x += dx / max(1.0, amount) * self.speed * dt
            self.z += dz / max(1.0, amount) * self.speed * dt
            want = math.atan2(dx, dz)
            self.yaw = wrap_angle(self.yaw + wrap_angle(want - self.yaw) * min(1.0, self.turn_rate * dt))
        for _ in range(2):  # twice, in case leaving one shape lands in another
            self._push_out()
        if self.bounds:
            x0, z0, x1, z1 = self.bounds
            self.x, self.z = min(x1, max(x0, self.x)), min(z1, max(z0, self.z))
        self.y = self.world.ground_height(self.x, self.z)

    def _push_out(self):
        for c in self.world.colliders(self.layers):
            ax, ay, az = c.a
            bx, by, bz = c.b
            if max(ay, by) + c.r < self.y or min(ay, by) - c.r > self.y + self.height:
                continue  # above or below the character
            # The closest point of the shape's axis on the ground plane.
            ex, ez = bx - ax, bz - az
            length2 = ex * ex + ez * ez
            t = 0.0 if length2 < 1e-9 else max(0.0, min(1.0, ((self.x - ax) * ex + (self.z - az) * ez) / length2))
            px, pz = ax + ex * t, az + ez * t
            vx, vz = self.x - px, self.z - pz
            d, need = math.hypot(vx, vz), c.r + self.radius
            if d >= need:
                continue
            if d < 1e-6:
                vx, vz, d = 1.0, 0.0, 1.0
            self.x, self.z = px + vx / d * need, pz + vz / d * need


# ---- smoothing ----------------------------------------------------------------------------------------------

class Smoothed:
    """A point that follows another smoothly (a camera's target, a marker). `rate`: how quickly it catches up, per
    second (the gap shrinks to 1/e in 1/rate s). For where a head looks, use Gaze: a point smoothed in space cuts
    across a circle, through the head itself when the target goes round it."""

    def __init__(self, start=(0.0, 0.0, 0.0), rate=6.0):
        self.value, self.rate = tuple(start), rate

    def update(self, target, dt):
        k = 1.0 - math.exp(-self.rate * dt)
        self.value = tuple(a + (b - a) * k for a, b in zip(self.value, target))
        return self.value

    def reset(self, value):
        self.value = tuple(value)


class Gaze:
    """Where a head looks, smoothed as a direction (yaw and pitch, wrapping round), not as a point: aim a look_at
    (docs/api.md §4.1) at gaze.update(head, target, dt). A target that switches, jumps or circles the head then
    turns it at most `speed` rad/s (and eases in with `rate`), the way a head turns, instead of snapping."""

    def __init__(self, rate=6.0, speed=4.0, reach=10.0):
        self.rate, self.speed, self.reach = rate, speed, reach
        self.yaw = self.pitch = None

    def update(self, origin, target, dt, forward=None, limit=None):
        """forward, limit: the body's heading and how far (rad) to either side of it the head may turn. Give them,
        and the gaze stays inside: a target going round behind is followed up to the limit and no further (a
        look_at would flip from one side to the other there)."""
        d = tuple(t - o for t, o in zip(target, origin))
        flat = math.hypot(d[0], d[2])
        want_yaw, want_pitch = math.atan2(d[0], d[2]), math.atan2(d[1], max(flat, 1e-6))
        if forward is not None and limit is not None:  # aim inside the limit, then turn there
            want_yaw = wrap_angle(forward + max(-limit, min(limit, wrap_angle(want_yaw - forward))))
        if self.yaw is None:  # the first look (or after reset): straight at it, inside the limit too
            self.yaw, self.pitch = want_yaw, want_pitch
        k, most = 1.0 - math.exp(-self.rate * dt), self.speed * dt
        step = lambda gap: max(-most, min(most, gap * k))  # noqa: E731
        self.yaw = wrap_angle(self.yaw + step(wrap_angle(want_yaw - self.yaw)))
        if forward is not None and limit is not None:  # the body turned under it: keep it inside
            self.yaw = wrap_angle(forward + max(-limit, min(limit, wrap_angle(self.yaw - forward))))
        self.pitch += step(want_pitch - self.pitch)
        f = _forward(self.yaw, self.pitch)
        return tuple(o + c * self.reach for o, c in zip(origin, f))

    def reset(self):
        self.yaw = self.pitch = None


# ---- the screen -------------------------------------------------------------------------------------------

def world_to_screen(engine, point):
    """Where a world point is on the screen, in HUD pixels (e.scene_size), or None behind the camera: for labels,
    markers and reticles pinned to things (the current camera, not interpolated)."""
    cam = engine.world.camera
    w, h = engine.scene_size
    eye = cam.position
    f, r = _forward(cam.yaw, cam.pitch), _right(cam.yaw)
    u = (r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0])  # up = right x forward
    d = tuple(p - e for p, e in zip(point, eye))
    depth = sum(a * b for a, b in zip(d, f))
    if depth <= cam.near:
        return None
    tx = math.tan(cam.fov_x / 2)
    ty = tx * h / w
    x, y = sum(a * b for a, b in zip(d, r)), sum(a * b for a, b in zip(d, u))
    return w / 2 + x / depth * (w / 2) / tx, h / 2 - y / depth * (h / 2) / ty


def text_width(text, scale=1):
    """Width in HUD pixels of hud_text with the built-in 5x7 font (6 px a character, less the last gap)."""
    return max(0, len(text) * 6 - 1) * scale


def say(engine, x, y, text, color=WHITE, scale=1, shadow=SHADOW):
    engine.hud_text(x, y, text, color, scale, shadow)


def centred(engine, y, text, color=WHITE, scale=1, shadow=SHADOW, x=None):
    """Text centred on x (default: the middle of the screen)."""
    cx = engine.scene_size[0] / 2 if x is None else x
    engine.hud_text(int(cx - text_width(text, scale) / 2), y, text, color, scale, shadow)


# ---- reuse ------------------------------------------------------------------------------------------------

class Pool:
    """Objects made once and reused (bullets, effects, enemies): get() hands back a released one or makes a new
    one with make(); release(obj) gives it back (hide it, or deactivate it, in your own release hook)."""

    def __init__(self, make, on_get=None, on_release=None):
        self.make, self.on_get, self.on_release = make, on_get, on_release
        self.free, self.used = [], []

    def get(self):
        obj = self.free.pop() if self.free else self.make()
        self.used.append(obj)
        if self.on_get:
            self.on_get(obj)
        return obj

    def release(self, obj):
        self.used.remove(obj)
        self.free.append(obj)
        if self.on_release:
            self.on_release(obj)

    def release_all(self):
        for obj in list(self.used):
            self.release(obj)


# ---- shapes: meshes to pass to e.create_mesh -----------------------------------------------------------

def _mesh(positions, indices, colours, material=None):
    """An alia_engine.Mesh: one palette index per triangle (flat-shaded with the default material), or one part in
    `material` (e.g. a SMOOTH or emissive material with its own palette_index)."""
    import alia_engine as ae
    pos, idx = array("f", positions).tobytes(), array("H", indices).tobytes()
    if material is not None:
        return ae.Mesh(pos, idx, submeshes=[(0, len(indices) // 3, material)])
    return ae.Mesh(pos, idx, tri_palette=bytes(colours))


def box(sx=1.0, sy=1.0, sz=1.0, colour=1, centre=(0.0, 0.0, 0.0), material=None):
    """A box sx x sy x sz (m) around `centre` (move the centre to put the origin at a face or an end: a joint, a
    floor). colour: a palette index, or six of them for the faces -Z, +Z, -Y, +Y, +X, -X."""
    p = [-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1, -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1]
    i = [0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 2, 3, 7, 2, 7, 6, 1, 2, 6, 1, 6, 5, 0, 4, 7, 0, 7, 3]
    half = (sx / 2, sy / 2, sz / 2)
    faces = [colour] * 6 if isinstance(colour, int) else list(colour)
    return _mesh([v * half[k % 3] + centre[k % 3] for k, v in enumerate(p)], i, [c for c in faces for _ in (0, 1)], material)


def sphere(radius=1.0, colour=1, rings=6, segments=10, centre=(0.0, 0.0, 0.0), material=None):
    """A low-poly ball: faceted with the default material (the handheld look); give a SMOOTH material for a round one.
    2 x segments x (rings - 1) triangles (6 x 10: 100)."""
    p, i = [], []
    for r in range(rings + 1):
        v = math.pi * r / rings
        for s in range(segments + 1):
            u = 2 * math.pi * s / segments
            p += [centre[0] + radius * math.sin(v) * math.cos(u), centre[1] + radius * math.cos(v),
                  centre[2] + radius * math.sin(v) * math.sin(u)]
    for r in range(rings):
        for s in range(segments):
            a = r * (segments + 1) + s
            b, c = a + 1, a + segments + 1
            if r > 0:
                i += [a, b, c]
            if r < rings - 1:          # the rows at the poles have one triangle a quad: the other has no area
                i += [b, c + 1, c]
    return _mesh(p, i, [colour] * (len(i) // 3), material)


def cylinder(radius=0.5, height=1.0, colour=1, segments=10, cap=None, material=None):
    """An upright cylinder from y 0 to `height` (pillars, trunks, posts), capped top and bottom (cap: their colour,
    default the side's)."""
    p, i, cols = [0.0, 0.0, 0.0, 0.0, height, 0.0], [], []
    for s in range(segments):
        u = 2 * math.pi * s / segments
        p += [radius * math.cos(u), 0.0, radius * math.sin(u), radius * math.cos(u), height, radius * math.sin(u)]
    for s in range(segments):
        a, b = 2 + 2 * s, 2 + 2 * ((s + 1) % segments)
        i += [a, b + 1, b, a, a + 1, b + 1]          # the side, outwards
        i += [0, a, b, 1, b + 1, a + 1]              # bottom (seen from below), top (seen from above)
        cols += [colour, colour, colour if cap is None else cap, colour if cap is None else cap]
    return _mesh(p, i, cols, material)


def plane(width=10.0, depth=10.0, colour=20, divisions=1, checker=None, material=None):
    """A flat ground at y 0, centred, facing up; divisions x divisions cells (checker: the other cells' colour)."""
    p, i, cols = [], [], []
    n = max(1, divisions)
    for j in range(n + 1):
        for k in range(n + 1):
            p += [-width / 2 + width * k / n, 0.0, -depth / 2 + depth * j / n]
    for j in range(n):
        for k in range(n):
            a = j * (n + 1) + k
            i += [a, a + n + 1, a + 1, a + 1, a + n + 1, a + n + 2]
            c = checker if checker is not None and (j + k) % 2 else colour
            cols += [c, c]
    return _mesh(p, i, cols, material)


# ---- terrain ------------------------------------------------------------------------------------------

def hills(seed=1, amplitude=2.0, scale=0.06, octaves=3):
    """A height function (x, z) -> y (m) of rolling hills within -amplitude..amplitude: fbm at `scale` (about one
    hill every 1 / scale m), the same for the same seed."""
    return lambda x, z: amplitude * fbm(x * scale, z * scale, seed, octaves)


class Terrain:
    """Ground you can see and stand on in one go: a heightmap mesh in the world and the same heights as the world's
    ground (World.set_ground: ground_height, ray_ground, GroundMover, particles landing), which set_ground alone
    doesn't draw. size: the side of the square (m), centred on `centre`; cell: the spacing of the heights (m);
    height: a function (x, z) -> y (default: flat; hills() for hills); colours: (low, high) palette indices
    split at the middle height, or a function (x, y, z) -> index for each triangle."""

    def __init__(self, engine, size=64.0, cell=4.0, height=None, colours=(21, 20), centre=(0.0, 0.0)):
        w = engine.world
        self.n = n = max(2, int(round(size / cell)) + 1)
        self.cell, self.origin = cell, (centre[0] - (n - 1) * cell / 2, centre[1] - (n - 1) * cell / 2)
        fn = height or (lambda x, z: 0.0)
        ox, oz = self.origin
        heights = [fn(ox + i * cell, oz + j * cell) for j in range(n) for i in range(n)]
        w.set_ground(heights, n, cell, self.origin)
        lo, hi = min(heights), max(heights)
        pick = colours if callable(colours) else (lambda x, y, z: colours[1] if y > (lo + hi) / 2 else colours[0])
        p, idx, cols = [], [], []
        for j in range(n):
            for i in range(n):
                p += [ox + i * cell, heights[j * n + i], oz + j * cell]
        for j in range(n - 1):
            for i in range(n - 1):
                a = j * n + i
                idx += [a, a + n, a + 1, a + 1, a + n, a + n + 1]
                c = pick(ox + (i + 0.5) * cell, (heights[a] + heights[a + 1] + heights[a + n] + heights[a + n + 1]) / 4,
                         oz + (j + 0.5) * cell)
                cols += [c, c]
        self.node = w.spawn(engine.create_mesh(_mesh(p, idx, cols)))
        self.world = w

    def height(self, x, z):
        return self.world.ground_height(x, z)


# ---- light and sky ------------------------------------------------------------------------------------

# Presets for e.frame's two directional lights (the sun, and the character light that follows the camera), the
# sky and the fog. alia-engine has no point lights: a lamp is an emissive material (docs/api.md §2) out of the fog.
ENVIRONMENTS = {
    "day": dict(sky_top=0x4A78C8, sky_horizon=0xB8CCE0, fog_color=0xB8CCE0, fog_near=40.0, fog_full=110.0,
                sun=((0.35, -0.82, 0.45), 0.5, 0.6), character=(0.55, 0.45), ground_fog=0.0),
    "dusk": dict(sky_top=0x1C2348, sky_horizon=0xD9845A, fog_color=0x6E5874, fog_near=18.0, fog_full=55.0,
                 sun=((0.55, -0.45, 0.7), 0.45, 0.6), character=(0.6, 0.5), ground_fog=0.45),
    "night": dict(sky_top=0x05070F, sky_horizon=0x1A2238, fog_color=0x151B2C, fog_near=12.0, fog_full=45.0,
                  sun=((-0.3, -0.85, 0.4), 0.3, 0.3), character=(0.55, 0.4), ground_fog=0.3),
    "overcast": dict(sky_top=0x8A929C, sky_horizon=0xC2C6CA, fog_color=0xB8BCC0, fog_near=25.0, fog_full=80.0,
                     sun=((0.1, -0.98, 0.15), 0.65, 0.25), character=(0.6, 0.35), ground_fog=0.2),
}


def environment(engine, name="day"):
    """Set the sky, the fog and both lights from a preset (ENVIRONMENTS: day, dusk, night, overcast); change any
    e.frame field afterwards to tune it."""
    env = ENVIRONMENTS[name]
    f = engine.frame
    f.sky_top, f.sky_horizon, f.fog_color = env["sky_top"], env["sky_horizon"], env["fog_color"]
    f.fog_near, f.fog_full, f.ground_fog = env["fog_near"], env["fog_full"], env["ground_fog"]
    f.sun.travel, f.sun.ambient, f.sun.diffuse = env["sun"]
    f.character.ambient, f.character.diffuse = env["character"]

