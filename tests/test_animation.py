"""Rig animation beyond playing one clip (docs/api.md §4): crossfades (play(anim, blend=seconds)) and, later,
layers. A rig made in code, stepped by hand like ae.run does (begin_step, end_step)."""

import math
import os
import sys

import alia_engine as ae

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from alia_pack import PackWriter  # noqa: E402

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


def rig():
    still = lambda rz: [[0, 0, rz], [0, 0, rz]]  # noqa: E731  (two keys, the same angle: a held pose)
    return {"bones": [{"name": "hips", "parent": None, "offset": [0, 1, 0], "mesh": None},
                      {"name": "arm", "parent": "hips", "offset": [0.5, 0, 0], "mesh": None},
                      {"name": "leg", "parent": "hips", "offset": [0, -0.5, 0], "mesh": None}],
            "anims": [{"name": "rest", "loop": True, "keys": 2, "tracks": {"arm": still(0), "leg": still(0)}},
                      {"name": "up", "loop": True, "keys": 2, "tracks": {"arm": still(90), "leg": still(-40)},
                       "root_y": [0.3, 0.3]}]}


e = ae.Engine(headless=True, size=(64, 36))
w = e.world
pack = PackWriter()
pack.add_rig("r", rig())
r = e.load_rigs(pack.bytes())["r"]
node = w.spawn_rig(r, position=(0, 0, 0))
arm = node.bone("arm")
DT = 1 / 60


def step(n=1):
    for _ in range(n):
        w.begin_step()
        w.end_step(DT)


def roll(b):
    return math.degrees(b.roll)


node.animator.play("rest", speed=0)
step(2)
check(abs(roll(arm)) < 1e-3, "rest: the arm down")
node.animator.play("up", speed=0)
step(1)
check(abs(roll(arm) - 90) < 1e-3, "play without blend: at once")

node.animator.play("rest", speed=0)
step(2)
node.animator.play("up", speed=0, blend=0.5)
step(15)  # 0.25 s: half way, smoothstep(0.5) = 0.5
mid = roll(arm)
check(40 < mid < 50 and node.animator.blending, f"blend=0.5: half way after 0.25 s ({mid:.1f} deg)")
hips_y = node.bone("hips").position[1]
check(1.05 < hips_y < 1.25, f"blend: root_y eases too ({hips_y:.3f}, from 1.0 to 1.3)")
step(10)  # 0.42 s: smoothstep eases out, more than linear
late = roll(arm)
check(late > 90 * 25 / 30 and late < 90, f"blend: eased (smoothstep) ({late:.1f} deg at 0.42 s)")
step(10)
check(abs(roll(arm) - 90) < 1e-3 and not node.animator.blending, "blend: done after 0.5 s, the animation alone")

# a crossfade started during another starts from the pose shown, not the old clip
node.animator.play("rest", speed=0, blend=0.5)
step(15)
shown = roll(arm)
node.animator.play("up", speed=0, blend=0.5)
step(1)
check(abs(roll(arm) - shown) < 5, f"a blend from a blend: no jump ({shown:.1f} -> {roll(arm):.1f})")
e.close()
sys.exit(1 if failures else 0)
