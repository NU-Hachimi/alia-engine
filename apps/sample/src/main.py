"""Wisp Keeper (捕光): an alia-engine sample game. At dusk, wisps drift over a meadow and hide behind standing stones;
the keeper has 60 seconds to catch as many as it can with a beam of light. The best score is saved.

    alia run                                     PC window (keyboard or gamepad), from apps/sample/
    alia run --demo --headless --frames 600 --screenshot shot.png
    alia cart && alia install && alia launch     as a cart on the board

Controls: left stick walks, right stick turns the view, B (south) casts the beam, A (east) starts, Start pauses.

How it is organised (docs/api.md §9, decisions 5 and 7): no engine base classes. The game is plain classes that
hold engine objects (Field: the ground and the stones; Keeper: a rig and its beam; Wisp: a node, a collider and
a trail emitter) and a Game that runs them: in fixed_update it reads the input actions, moves its objects and
asks the engine (rays, ground heights); the engine draws, interpolates, animates and simulates. Things made
many times come from templates (the keeper's rig, the particle templates, the wisp's ball mesh).
"""

import argparse
import json
import math
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "python"))  # a cart's content/python (the engine module)

import alia_engine as ae  # noqa: E402
import alia_kit as kit  # noqa: E402
import alia_tween as tween  # noqa: E402  a plugin (docs/plugins.md): the nightfall

import field  # noqa: E402
import sounds  # noqa: E402
import wisps  # noqa: E402
from keeper import Keeper  # noqa: E402

ROUND = 60.0               # s
COUNT = 6                  # wisps out at a time
SAVE = "best.json"         # ae.save, docs/api.md §7
GOLD = 0xFFD870FF          # HUD colours are 0xRRGGBBAA (kit.WHITE for white)

PALETTE = [(0, 0, 0), (52, 78, 58), (74, 104, 66), (122, 114, 120), (150, 142, 146), (70, 60, 110), (226, 190, 160),
           (40, 34, 48), (230, 250, 255), (120, 240, 255), (255, 214, 110), (255, 140, 190)] + [(128, 128, 128)] * 20

ACTIONS = {"move": {"stick": "left", "dead_zone": 0.15}, "turn": {"stick": "right", "dead_zone": 0.15},
           "beam": ["SOUTH"], "start": ["EAST"], "pause": ["START"]}


class DemoInput:
    """Scripted input for --demo (tests, screenshots): start, then walk in a wide circle casting the beam."""

    def __init__(self):
        self.t = 0.0

    def __call__(self, dt):
        self.t += dt
        return {"move": (0.35, -0.9) if self.t > 0.5 else (0.0, 0.0), "turn": (0.25, 0.0),
                "beam": self.t > 1.0 and int(self.t * 3) % 2 == 0, "start": 0.2 < self.t < 0.3, "pause": False}


class Game:
    """alia_engine.run() calls these: logic at a fixed 60 Hz, the HUD once per displayed frame."""

    def __init__(self, demo=False):
        self.demo = DemoInput() if demo else None

    def init(self, e):
        self.rng = random.Random(7)
        e.set_palette(bytes(c for rgb in PALETTE for c in rgb))
        e.actions.load(ACTIONS)
        glow = e.create_material(ae.Material(shading=ae.Shading.UNLIT, palette_index=9, fog=False, ground_fog=False))
        ball = e.create_mesh(kit.sphere(1.0, rings=4, segments=6, material=glow))  # 36 triangles: drawn many times
        self.field = field.Field(e, self.rng)
        self.keeper = Keeper(e, glow)
        # Particle templates (docs/api.md §6): one trail per wisp colour, one burst for a catch.
        trails = {tint: e.particles(ball, lifetime=(0.5, 0.8), speed=(0.1, 0.4), size=(0.08, 0.12), tint=tint,
                                    size_over_life=[(0, 1), (1, 0.2)], max_particles=24) for tint in wisps.GLOWS}
        self.burst = e.world.spawn_emitter(
            e.particles(ball, lifetime=(0.5, 0.9), speed=(2.0, 5.0), size=(0.07, 0.12), gravity=0.4, shape="sphere",
                        size_over_life=[(0, 1), (1, 0.1)], max_particles=200), seed=1)
        self.wisps = [wisps.Wisp(e, ball, trails[wisps.GLOWS[k % 3]], self.rng, k) for k in range(COUNT)]
        au = e.audio  # docs/api.md §7: the mixer
        self.sounds = {name: au.load(make()) for name, make in
                       (("beam", sounds.beam), ("chime", sounds.chime), ("thud", sounds.thud), ("dusk", sounds.dusk))}
        au.play_music(self.sounds["dusk"], volume=0.35)
        saved = ae.save.read(SAVE)
        self.best = json.loads(saved)["best"] if saved else 0
        self.state, self.score, self.left, self.combo, self.t = "title", 0, ROUND, 0, 0.0
        self.watching = None
        self.tweens = tween.Tweens()

    def fixed_update(self, e, dt):
        a = e.actions
        inp = self.demo(dt) if self.demo else {"move": a.axis("move"), "turn": a.axis("turn"), "beam": a.held("beam"),
                                                "start": a.pressed("start"), "pause": a.pressed("pause")}
        if self.state == "play" and inp["pause"]:
            self.state = "paused"
        elif self.state == "paused":
            if inp["pause"] or inp["start"]:
                self.state = "play"
            return
        self.t += dt
        for w in self.wisps:
            w.update(self.keeper, self.t, dt)
        if self.state != "play":
            self.keeper.update(e, (0.0, 0.0), inp["turn"][0] * 0.3, dt)
            if inp["start"]:
                self.state, self.score, self.left, self.combo = "play", 0, ROUND, 0
                self.tweens.stop()
                field.Field.dusk(e)  # back to dusk, then night falls over the round
                field.Field.nightfall(e, self.tweens, ROUND)
            return
        self.tweens.update(dt)  # not while paused or on the title: the night waits too
        self.keeper.update(e, inp["move"], inp["turn"][0], dt)
        self.keeper.watch(self._watched(), dt)
        if inp["beam"]:
            self._beam(e)
        self.left -= dt
        if self.left <= 0:
            self.state, self.left = "over", 0.0
            if self.score > self.best:
                self.best = self.score
                if not self.demo:  # a scripted run doesn't touch the player's record
                    ae.save.write(SAVE, json.dumps({"best": self.best}).encode())

    def _watched(self):
        """The wisp the keeper's head follows: the nearest one in front of it within 14 m (and 90 degrees of where it
        faces), but it keeps the one it has until that one is past 16 m or 110 degrees, or another is clearly (25%)
        nearer. Without those margins two wisps at about the same distance take turns step by step and the head
        jitters; and a wisp that goes round behind would swing the head from one side to the other (the look_at
        flips there, docs/api.md §4.1), so it lets go of it instead and the weight eases out."""
        k = self.keeper
        far = lambda w: kit.distance_xz((w.x, w.z), (k.x, k.z))  # noqa: E731
        off = lambda w: abs(kit.angle_between(k.mover.yaw, kit.yaw_to((k.x, k.z), (w.x, w.z))))  # noqa: E731
        ahead = [w for w in self.wisps if off(w) < math.radians(90) and far(w) < 14]
        nearest = min(ahead, key=far) if ahead else None
        current = self.watching
        if (current is not None and far(current) < 16 and off(current) < math.radians(110)
                and (nearest is None or far(nearest) > 0.75 * far(current))):
            return current.node
        self.watching = nearest
        return nearest.node if nearest else None

    def _beam(self, e):
        hit, fired = self.keeper.cast(e.world, field.SOLID | wisps.WISP)
        if not fired:
            return
        au = e.audio
        au.play(self.sounds["beam"], volume=0.5, pitch=self.rng.uniform(0.95, 1.05))
        if hit is None:
            self.combo = 0
        elif hit.layers & wisps.WISP:
            caught = self.wisps[hit.user]
            self.burst.burst(40, at=hit.point, tint=caught.tint)
            self.combo += 1
            self.score += self.combo  # catches in a row count more
            au.play(self.sounds["chime"], volume=0.8, pitch=min(2.0, 1.0 + 0.08 * (self.combo - 1)))
            caught.place(far_from=(self.keeper.x, self.keeper.z))
        else:  # a stone
            self.combo = 0
            self.burst.burst(6, at=hit.point, tint=field.STONE_TOP)
            au.play(self.sounds["thud"], volume=0.6)

    def render(self, e, alpha):
        w, h = e.scene_size
        say = lambda x, y, s, c=kit.WHITE: kit.say(e, x, y, s, c, 2)  # noqa: E731
        centred = lambda y, s, c=kit.WHITE, k=2: kit.centred(e, y, s, c, k)  # noqa: E731
        if self.state == "title":
            centred(h // 2 - 40, "WISP KEEPER", GOLD, 4)
            centred(h // 2 + 10, "CATCH THE WISPS BEFORE NIGHTFALL")
            centred(h // 2 + 34, f"PRESS A    BEST {self.best}")
            return
        say(10, 10, f"WISPS {self.score}")
        say(w - 10 - 5 * 12, 10, f"{int(self.left) // 60}:{int(self.left) % 60:02d}")
        if self.combo > 1:
            say(10, 30, f"x{self.combo}", GOLD)
        e.hud_rect(w // 2 - 1, h // 2 - 6, 2, 12, kit.WHITE)  # the crosshair
        e.hud_rect(w // 2 - 6, h // 2 - 1, 12, 2, kit.WHITE)
        if self.state == "paused":
            centred(h // 2 - 30, "PAUSED")
            centred(h // 2, "START TO GO ON")
        elif self.state == "over":
            centred(h // 2 - 30, f"NIGHT FALLS: {self.score} WISPS", GOLD)
            centred(h // 2, f"BEST {self.best}    A TO PLAY AGAIN")

    def shutdown(self, e):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--demo", action="store_true", help="scripted input (tests, screenshots)")
    args = parser.parse_args(ae.strip_runtime_flags(sys.argv[1:]))
    ae.run(Game(demo=args.demo), argv=sys.argv[1:], title="Wisp Keeper", app_id="pad.alia.engine.sample")


if __name__ == "__main__":
    main()
