"""alia_tween: an alia-engine plugin (docs/plugins.md). Ease any attribute to a value over time: a node's position
or scale, e.frame's sky and fog, a material's tint, the game's own numbers.

    import alia_tween as tween
    tw = tween.Tweens()                                    # the game holds one (or one per screen)
    tw.to(door, "position", (0, 3, 0), 0.6, ease="out_back").then(lambda: sfx.play("clunk"))
    tw.to(e.frame, "sky_top", 0x05070F, 60.0, colour="rgb")    # 0xRRGGBB, channel by channel
    tw.to(boss, "yaw", target_yaw, 0.4, angle=True)        # the short way round
    tw.sequence(lambda: tw.to(cam, "fov_x", 0.9, 0.2), 0.5, lambda: tw.to(cam, "fov_x", 1.25, 0.4))
    ...
    def fixed_update(self, e, dt):
        tw.update(dt)                                      # every step, with the rest of the game

Like everything on alia-engine it is a library, not a hook: nothing runs unless the game calls update(). Call it in
fixed_update, so the engine interpolates what it moves between steps. Values are numbers or tuples of numbers
(read from the attribute when the tween starts, after its delay); ints stay ints.
"""

import math

import alia_engine as _ae

from . import ease

__version__ = "0.1.0"
__all__ = ["Tweens", "Tween", "Sequence", "ease"]


def _engine_version_ok(have, lo=(0, 1), hi=(1, 0)):
    return lo <= tuple(int(p) for p in have.split(".")[:2]) < hi


if not _engine_version_ok(_ae.__version__):  # plugin.toml's engine = ">=0.1, <1"
    raise ImportError(f"alia_tween {__version__} needs alia-engine >=0.1, <1; this is {_ae.__version__}")


def _channels(v, n):
    return [(v >> (8 * (n - 1 - k))) & 0xFF for k in range(n)]


def _pack(cs):
    v = 0
    for c in cs:
        v = (v << 8) | max(0, min(255, int(round(c))))
    return v


class Tween:
    """One attribute going from where it is to `end`. Made by Tweens.to (or Tweens.call for a timer); its methods
    return it, so they chain: tw.to(...).loop(3, yoyo=True).then(done)."""

    def __init__(self, owner, target, attr, end, duration, curve, delay, start, colour, angle, setter):
        self._owner, self.target, self.attr, self.end = owner, target, attr, end
        self.duration, self._curve, self.delay = max(0.0, float(duration)), ease.get(curve), max(0.0, float(delay))
        self._start, self._colour, self._angle, self._setter = start, colour, angle, setter
        self.elapsed, self.done, self._loops, self._yoyo, self._forward = 0.0, False, 1, False, True
        self._then = []
        self._from = self._to = None

    # ---- chaining -----------------------------------------------------------------------------------------
    def then(self, fn):
        """Call fn() when it finishes (after its last loop); not when it is stopped. fn may start more tweens."""
        self._then.append(fn)
        return self

    def loop(self, times=-1, yoyo=False):
        """Play it `times` times (-1: until stopped); yoyo: every other time backwards."""
        self._loops, self._yoyo = times, yoyo
        return self

    def stop(self, complete=False):
        """Stop it where it is (complete=True: jump to the end first, and call its then)."""
        if self.done:
            return
        if complete:
            if self._from is None:
                self._begin()
            self._apply(1.0 if self._forward else 0.0)
            self._finish()
        else:
            self.done = True

    @property
    def progress(self):
        """0..1 through the current play (0 while waiting for its delay)."""
        return 1.0 if self.done else min(1.0, self.elapsed / self.duration) if self.duration > 0 else 0.0

    # ---- inside ------------------------------------------------------------------------------------------
    def _read(self):
        if self._setter is not None or self.attr is None:
            return self._start
        return getattr(self.target, self.attr)

    def _begin(self):
        a = self._start if self._start is not None else self._read()
        b = self.end
        if self._colour:
            n = 4 if self._colour == "rgba" else 3
            a, b = _channels(int(a), n), _channels(int(b), n)
        elif isinstance(b, (tuple, list)) or isinstance(a, (tuple, list)):
            a = list(a) if isinstance(a, (tuple, list)) else [a] * len(b)
            b = list(b) if isinstance(b, (tuple, list)) else [b] * len(a)  # scale 2.0 on a (sx, sy, sz): all three
            if len(a) != len(b):
                raise ValueError(f"tween of {self.attr!r}: from {tuple(a)} to {tuple(b)}, not the same length")
        elif self._angle:
            b = a + (b - a + math.pi) % (2 * math.pi) - math.pi  # the short way round
        self._from, self._to = a, b
        self._ints = (isinstance(self.end, int) and isinstance(a, int)) if not isinstance(a, list) else \
            [isinstance(x, int) and isinstance(y, int) for x, y in zip(a, b)]

    def _value(self, s):
        a, b = self._from, self._to
        if self._colour:
            return _pack(x + (y - x) * s for x, y in zip(a, b))
        if isinstance(a, list):
            out = [x + (y - x) * s for x, y in zip(a, b)]
            return tuple(int(round(v)) if i else v for v, i in zip(out, self._ints))
        v = a + (b - a) * s
        return int(round(v)) if self._ints else v

    def _apply(self, t):
        if self.attr is None and self._setter is None:
            return  # a timer
        v = self._value(self._curve(t))
        if self._setter is not None:
            self._setter(v)
        else:
            setattr(self.target, self.attr, v)

    def _finish(self):
        self.done = True
        for fn in self._then:
            fn()

    def _step(self, dt):
        """Advance by dt; False once it has finished."""
        if self.done:
            return False
        if self.delay > 0:
            self.delay -= dt
            if self.delay > 0:
                return True
            dt = -self.delay
            self.delay = 0.0
        if self._from is None:
            self._begin()
        self.elapsed += dt
        while True:
            t = 1.0 if self.duration <= 0 else min(1.0, self.elapsed / self.duration)
            self._apply(t if self._forward else 1.0 - t)
            if t < 1.0:
                return True
            if self._loops > 0:
                self._loops -= 1
            if self._loops == 0:
                self._finish()
                return False
            self.elapsed -= self.duration
            if self._yoyo:
                self._forward = not self._forward
            if self.duration <= 0:  # an endless zero-length loop would never return
                return True


class Sequence:
    """Steps one after another (Tweens.sequence): each a function that starts a tween (and returns it), a number
    of seconds to wait, or a function that returns None (called, and the sequence goes straight on)."""

    def __init__(self, owner, steps):
        self._owner, self._steps, self._then = owner, list(steps), []
        self.current, self.done, self._wait = None, False, 0.0

    def then(self, fn):
        self._then.append(fn)
        return self

    def stop(self):
        if self.current is not None:
            self.current.stop()
        self.done = True

    def _step(self, dt):
        while not self.done:
            if self.current is not None and not self.current.done:
                return True
            if self._wait > 0:
                self._wait -= dt
                if self._wait > 0:
                    return True
                dt = 0.0
            if not self._steps:
                self.done = True
                for fn in self._then:
                    fn()
                return False
            step = self._steps.pop(0)
            if isinstance(step, (int, float)):
                self._wait = float(step)
                continue
            got = step()
            self.current = got if isinstance(got, (Tween, Sequence)) else None
        return False


class Tweens:
    """The game's tweens: start them with to(), call(), sequence(); update(dt) every fixed step moves them all."""

    def __init__(self):
        self._running = []

    def to(self, target, attr, end, duration, ease="out_quad", delay=0.0, start=None, colour=None, angle=False):
        """Ease target.attr to `end` over `duration` s (after `delay` s), from `start` if given or else from its
        value when it starts. colour: "rgb" / "rgba" for a packed 0xRRGGBB / 0xRRGGBBAA int; angle: radians, the
        short way round. target may also be a function v -> None (attr None, `start` needed): it is given each value."""
        setter = target if callable(target) and attr is None else None
        if setter is not None and start is None:
            raise ValueError("a tween into a function needs start=")
        tw = Tween(self, target, attr, end, duration, ease, delay, start, colour, angle, setter)
        self._running.append(tw)
        return tw

    def call(self, delay, fn):
        """Call fn() after `delay` s (a timer; stop() it to cancel)."""
        tw = Tween(self, None, None, None, 0.0, "linear", delay, None, None, False, None)
        tw.then(fn)
        self._running.append(tw)
        return tw

    def sequence(self, *steps):
        """Run steps one after another (see Sequence); returns the Sequence (stop(), then())."""
        seq = Sequence(self, steps)
        self._running.append(seq)
        return seq

    def update(self, dt):
        """Move every tween by dt (s). Tweens started during it (from a then, a sequence) start moving on the next
        update."""
        running, self._running = self._running, []
        kept = [tw for tw in running if tw._step(dt)]
        self._running = kept + self._running

    def stop(self, target=None, attr=None, complete=False):
        """Stop the tweens of `target` (all of them if None), only those of `attr` if given."""
        for tw in self._running:
            if isinstance(tw, Tween) and (target is None or tw.target is target) and (attr is None or tw.attr == attr):
                tw.stop(complete)
            elif isinstance(tw, Sequence) and target is None:
                tw.stop()

    def busy(self, target=None, attr=None):
        """Whether any tween (of target, of attr) is still running or waiting."""
        return any(not tw.done and (target is None or getattr(tw, "target", None) is target)
                   and (attr is None or getattr(tw, "attr", None) == attr) for tw in self._running)

    def __len__(self):
        return sum(1 for tw in self._running if not tw.done)
