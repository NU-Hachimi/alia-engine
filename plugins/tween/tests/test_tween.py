"""alia_tween (plugins/tween): tweens against a headless engine's nodes and e.frame."""

import math
import sys

import alia_engine as ae
import alia_tween as tween
from alia_tween import ease

failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


def run(tw, seconds, dt=1 / 60):
    for _ in range(int(round(seconds / dt))):
        tw.update(dt)


near = lambda a, b, eps=1e-6: abs(a - b) < eps  # noqa: E731

# Easing curves: 0 at 0, 1 at 1, and the named ones exist.
check(all(near(fn(0.0), 0.0) and near(fn(1.0), 1.0) for fn in ease.NAMES.values()), f"{len(ease.NAMES)} curves go 0 -> 1")
check(ease.out_back(0.8) > 1.0 and ease.in_back(0.2) < 0.0, "back overshoots")
try:
    ease.get("wobble")
    check(False, "an unknown curve is refused")
except ValueError as err:
    check("out_cubic" in str(err), "an unknown curve is refused, and the error lists the curves")

e = ae.Engine(headless=True)
w = e.world
tw = tween.Tweens()

# A node's position, linearly: halfway at half time, exactly the end at the end, then gone.
node = w.spawn(position=(0, 0, 0))
done = []
tw.to(node, "position", (2, 4, 6), 1.0, ease="linear").then(lambda: done.append(1))
run(tw, 0.5)
check(all(near(a, b, 1e-5) for a, b in zip(node.position, (1, 2, 3))), "position halfway at half time")
run(tw, 0.5)
check(node.position == (2.0, 4.0, 6.0) and done == [1] and len(tw) == 0, "at the end exactly, then called back once, then gone")

# A number on scale broadcasts over the tuple; delay waits; start= overrides the current value.
tw.to(node, "scale", 2.0, 0.5, ease="linear", delay=0.25)
run(tw, 0.25)
check(node.scale == (1.0, 1.0, 1.0), "nothing before the delay")
run(tw, 0.5)
check(all(near(c, 2.0, 1e-5) for c in node.scale), "a number tweens every part of a tuple")
cam = w.camera
tw.to(cam, "fov_x", 1.0, 0.1, start=0.5, ease="linear")
tw.update(0.05)
check(near(cam.fov_x, 0.75, 1e-5), "start= instead of the current value")
run(tw, 0.1)

# Colours channel by channel; angles the short way round; ints stay ints.
f = e.frame
f.sky_top = 0x000000
tw.to(f, "sky_top", 0xFF8040, 1.0, ease="linear", colour="rgb")
run(tw, 0.5)
check(all(abs(((f.sky_top >> k) & 0xFF) - ((0x804020 >> k) & 0xFF)) <= 1 for k in (0, 8, 16)), f"a colour halfway is halfway in each channel ({f.sky_top:06X})")
run(tw, 0.5)
node.yaw = math.radians(170)
tw.to(node, "yaw", math.radians(-170), 1.0, ease="linear", angle=True)
run(tw, 0.5)
check(near(math.cos(node.yaw), -1.0, 1e-5), "an angle goes the short way, through pi")


class Score:
    shown = 0


s = Score()
tw.to(s, "shown", 100, 1.0, ease="linear")
run(tw, 0.333)
check(isinstance(s.shown, int) and s.shown == 33, "an int stays an int")
run(tw, 1.0)

# Loops and yoyo; stop; stop(complete); timers; tweening into a function.
box = Score()
box.v = 0.0
t = tw.to(box, "v", 1.0, 0.5, ease="linear").loop(2, yoyo=True)
run(tw, 0.75)
check(near(box.v, 0.5, 1e-5), "yoyo: on the way back at 0.75 s")
run(tw, 0.5)
check(t.done and near(box.v, 0.0), "yoyo twice: back where it began, done")
t = tw.to(box, "v", 10.0, 1.0).loop()
run(tw, 5.0)
check(not t.done, "loop() runs until stopped")
tw.stop(box)
run(tw, 0.1)
check(t.done and len(tw) == 0, "stop(target) stops its tweens")
called = []
tw.to(box, "v", 5.0, 1.0).then(lambda: called.append("end"))
tw.update(0.1)
tw.stop(box, "v", complete=True)
check(box.v == 5.0 and called == ["end"], "stop(complete=True) jumps to the end and calls then")
timer = tw.call(0.5, lambda: called.append("timer"))
run(tw, 0.4)
check("timer" not in called, "a timer waits")
run(tw, 0.2)
check(called[-1] == "timer" and timer.done, "and then calls")
got = []
tw.to(got.append, None, 1.0, 0.5, start=0.0, ease="linear")
run(tw, 0.5)
check(near(got[-1], 1.0) and len(got) == 30, "into a function: given each value")

# Sequences: steps one after another, waits between, a callback at the end.
order = []
seq = tw.sequence(lambda: tw.to(box, "v", 0.0, 0.2, start=1.0).then(lambda: order.append("a")),
                  0.3,
                  lambda: order.append("b"),
                  lambda: tw.to(box, "v", 2.0, 0.2).then(lambda: order.append("c"))).then(lambda: order.append("end"))
run(tw, 0.4)
check(order == ["a"], f"sequence: the first step, then waiting ({order})")
run(tw, 0.5)
check(order == ["a", "b", "c", "end"] and seq.done and box.v == 2.0, f"sequence: all of it in order ({order})")
check(tw.busy() is False, "nothing busy at the end")

e.close()
sys.exit(1 if failures else 0)
