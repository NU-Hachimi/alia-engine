"""The sample's sounds, synthesized at start-up into WAV bytes for e.audio.load (docs/api.md §7): a real game
ships WAV files in its cart; a sample makes them so it carries no assets."""

import io
import math
import random
import wave
from array import array

RATE = 16000  # the mixer resamples any rate to 48 kHz


def _wav(samples):
    top = max(1e-9, max(abs(x) for x in samples))
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1), w.setsampwidth(2), w.setframerate(RATE)
        w.writeframes(array("h", (int(x / top * 0.8 * 32767) for x in samples)).tobytes())
    return buf.getvalue()


def beam():
    """A bright rising zap."""
    n = int(0.22 * RATE)
    out, phase = [], 0.0
    for i in range(n):
        phase += 2 * math.pi * (600 + 1800 * i / n) / RATE
        out.append((math.sin(phase) + 0.3 * math.sin(3 * phase)) * math.exp(-4 * i / n))
    return _wav(out)


def chime():
    """A caught wisp: two bell partials ringing out."""
    n = int(0.6 * RATE)
    return _wav([(math.sin(2 * math.pi * 880 * i / RATE) + 0.5 * math.sin(2 * math.pi * 1320 * i / RATE)) *
                 math.exp(-5 * i / n) for i in range(n)])


def thud():
    """The beam hits a pillar: a short dull knock."""
    rng, y, out = random.Random(3), 0.0, []
    for i in range(int(0.12 * RATE)):
        y += 0.2 * (rng.uniform(-1, 1) - y)
        out.append((y + 0.6 * math.sin(2 * math.pi * 110 * i / RATE)) * math.exp(-30 * i / RATE))
    return _wav(out)


def dusk():
    """The music: a soft chord slowly breathing, 6 s, looped (seamless: whole cycles of everything)."""
    n, out = 6 * RATE, []
    chord = (110.0, 165.0, 220.0, 277.5)  # A, E, A, C#: whole numbers of cycles in 6 s
    for i in range(n):
        t = i / RATE
        swell = 0.6 + 0.4 * math.sin(2 * math.pi * t / 6)
        out.append(swell * sum(math.sin(2 * math.pi * f * t) / (k + 1) for k, f in enumerate(chord)))
    return _wav(out)
