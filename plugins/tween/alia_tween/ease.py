"""Easing curves: t (0..1, the time) -> the share of the way (0 at 0, 1 at 1; back and elastic overshoot).
in_ starts slowly, out_ ends slowly, in_out_ does both. By name (Tweens.to(ease="out_cubic")) or as functions."""

import math


def linear(t):
    return t


def in_quad(t):
    return t * t


def out_quad(t):
    return 1 - (1 - t) * (1 - t)


def in_out_quad(t):
    return 2 * t * t if t < 0.5 else 1 - (-2 * t + 2) ** 2 / 2


def in_cubic(t):
    return t ** 3


def out_cubic(t):
    return 1 - (1 - t) ** 3


def in_out_cubic(t):
    return 4 * t ** 3 if t < 0.5 else 1 - (-2 * t + 2) ** 3 / 2


def in_sine(t):
    return 1 - math.cos(t * math.pi / 2)


def out_sine(t):
    return math.sin(t * math.pi / 2)


def in_out_sine(t):
    return -(math.cos(math.pi * t) - 1) / 2


def in_expo(t):
    return 0.0 if t <= 0 else 2 ** (10 * t - 10)


def out_expo(t):
    return 1.0 if t >= 1 else 1 - 2 ** (-10 * t)


def in_out_expo(t):
    if t <= 0 or t >= 1:
        return float(t >= 1)
    return 2 ** (20 * t - 10) / 2 if t < 0.5 else (2 - 2 ** (-20 * t + 10)) / 2


_C1 = 1.70158
_C3 = _C1 + 1


def in_back(t):
    return _C3 * t ** 3 - _C1 * t * t


def out_back(t):
    """Overshoots a little past the end and settles: a door that thuds open, a popup."""
    return 1 + _C3 * (t - 1) ** 3 + _C1 * (t - 1) ** 2


def in_out_back(t):
    c2 = _C1 * 1.525
    if t < 0.5:
        return (2 * t) ** 2 * ((c2 + 1) * 2 * t - c2) / 2
    return ((2 * t - 2) ** 2 * ((c2 + 1) * (t * 2 - 2) + c2) + 2) / 2


def out_elastic(t):
    """Springs past the end and back a few times."""
    if t <= 0 or t >= 1:
        return float(t >= 1)
    return 2 ** (-10 * t) * math.sin((t * 10 - 0.75) * (2 * math.pi) / 3) + 1


def out_bounce(t):
    """Lands and bounces: a dropped object."""
    n, d = 7.5625, 2.75
    if t < 1 / d:
        return n * t * t
    if t < 2 / d:
        t -= 1.5 / d
        return n * t * t + 0.75
    if t < 2.5 / d:
        t -= 2.25 / d
        return n * t * t + 0.9375
    t -= 2.625 / d
    return n * t * t + 0.984375


def in_bounce(t):
    return 1 - out_bounce(1 - t)


NAMES = {name: fn for name, fn in globals().items() if callable(fn) and not name.startswith("_") and name != "math"}


def get(ease):
    """A curve by name or a function (t -> share) as it is."""
    if callable(ease):
        return ease
    try:
        return NAMES[ease]
    except KeyError:
        raise ValueError(f"no easing curve {ease!r}; there are: {', '.join(sorted(NAMES))}") from None
