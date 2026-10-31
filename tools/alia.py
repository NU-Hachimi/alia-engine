#!/usr/bin/env python3
"""alia: build, run, test and pack a game made with alia-engine (docs/project.md).

    alia new DIR                 a new project in the recommended layout (a small game that runs)
    alia run [ARGS...]           play it on the PC (window; ARGS go to the game: --demo, --headless, --frames N, ...)
    alia test [PYTEST ARGS...]   its tests, against a headless engine
    alia exec SCRIPT [ARGS...]   one of its tools (tools/...) with the game and the engine importable
    alia cart [--pc]             the cart: build/cart/ (board) or build/cart-pc/, checked; the board's packed too
    alia install | launch | stop | logs     the cart on the board (the devkit's acart)
    alia setup                   put an `alia` command in ~/.local/bin
    alia engine [pc|board]       build the engine module (run, test and cart do it when needed)
    alia plugin check DIR        is a plugin's plugin.toml right (docs/plugins.md)

Run it from the project (or anywhere under it): it finds alia.toml. The engine is the alia-engine checkout this
tool lives in; its builds are shared by every project (build/ and build-board/ in that checkout). The board build
needs the board's sysroot (ALIA_SYSROOT) and the cart commands the Alia Pad devkit's acart (on PATH, or ACART), both
from the devkit (docs/getting-started.md). PYTHON is the interpreter for run and test (default: the project's
.venv if it has one, else python3).

What a cart holds (and `run` / `test` lay out the same way under build/dev/, with links, so edits show at once):
    cart.toml and the files next to it in cart/      the manifest and the art
    content/                                         the project's src/ (main.py at its top)
    content/assets/                                  the project's assets/
    content/python/                                  alia_engine, alia_kit, alia_pack and the plugins
"""

import argparse
import os
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path

ENGINE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ENGINE / "tools"))
import alia_plugin  # noqa: E402

ACART = os.environ.get("ACART") or shutil.which("acart") or "acart"
BUILDS = {"pc": ENGINE / "build", "board": ENGINE / "build-board"}


class Fail(Exception):
    pass


# ---- the project ---------------------------------------------------------------------------------

class Project:
    def __init__(self, root):
        self.root = root
        with open(root / "alia.toml", "rb") as f:
            doc = tomllib.load(f)
        game, engine = doc.get("game", {}), doc.get("engine", {})
        self.name = game.get("name") or root.name
        self.src = root / game.get("src", "src")
        self.assets = root / game.get("assets", "assets")
        self.cart_dir = root / game.get("cart", "cart")
        self.tests = root / game.get("tests", "tests")
        self.art = root / game["art"] if game.get("art") else None
        self.engine_spec = engine.get("version", ">=0.1")
        self.plugins = engine.get("plugins", [])
        self.build = root / "build"
        if not (self.src / "main.py").is_file():
            raise Fail(f"{self.src}/main.py: no game (alia.toml [game] src)")
        if not alia_plugin.version_ok(alia_plugin.engine_version(), self.engine_spec):
            raise Fail(f"{self.name} needs alia-engine {self.engine_spec}; {ENGINE} is {alia_plugin.engine_version()}")

    @staticmethod
    def find(start=None):
        here = Path(start or Path.cwd()).resolve()
        for d in [here, *here.parents]:
            if (d / "alia.toml").is_file():
                return Project(d)
        raise Fail("no alia.toml here or above (alia new DIR makes a project)")

    def plugin_dirs(self):
        """Each plugin of alia.toml: a first-party one by name (the engine's plugins/<name>), or a directory in the
        project (plugins/<name>, or a path)."""
        out = []
        for p in self.plugins:
            for d in (self.root / p, self.root / "plugins" / p, ENGINE / "plugins" / p):
                if (d / "plugin.toml").is_file():
                    out.append(d)
                    break
            else:
                raise Fail(f"plugin {p!r}: not in {self.root / 'plugins'} nor among alia-engine's plugins/")
        return out


def run(cmd, **kw):
    print("+ " + " ".join(str(c) for c in cmd), file=sys.stderr)
    if subprocess.run([str(c) for c in cmd], **kw).returncode:
        raise Fail(f"{Path(str(cmd[0])).name} failed")


def build_engine(target="pc"):
    """The engine module for the PC or the board, built in the engine checkout (configured the first time)."""
    out = BUILDS[target]
    if not (out / "build.ninja").exists():
        sysroot = os.environ.get("ALIA_SYSROOT", "")
        if target == "board" and not sysroot:
            raise Fail("the board build needs the board's sysroot: ALIA_SYSROOT (docs/getting-started.md)")
        extra = ["-DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-board.cmake", f"-DALIA_SYSROOT={sysroot}"] if target == "board" else []
        run(["cmake", "-S", ENGINE, "-B", out, "-G", "Ninja", *extra])
    run(["cmake", "--build", out, "--target", "alia_engine"], stdout=subprocess.DEVNULL)
    return out


def python_for(project):
    venv = project.root / ".venv" / "bin" / "python"
    return os.environ.get("PYTHON") or (str(venv) if venv.exists() else sys.executable)


def _place(src, dest, link):
    if dest.is_symlink() or dest.is_file():
        dest.unlink()
    elif dest.is_dir():
        shutil.rmtree(dest)
    if link:
        dest.symlink_to(src.resolve(), target_is_directory=src.is_dir())
    elif src.is_dir():
        shutil.copytree(src, dest, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    else:
        shutil.copy2(src, dest)


def lay_out(project, out, target="pc", link=False):
    """The cart's layout in `out` (see the module doc): copied for a cart, linked for run and test."""
    engine = build_engine(target)
    content = out / "content"
    content.mkdir(parents=True, exist_ok=True)
    for path in project.src.iterdir():
        if path.name not in ("__pycache__", "assets", "python"):
            _place(path, content / path.name, link)
    if project.assets.is_dir():
        _place(project.assets, content / "assets", link)
    py = content / "python"
    if py.exists():
        shutil.rmtree(py)
    run(["cmake", "--install", engine, "--prefix", content], stdout=subprocess.DEVNULL)  # content/python/...
    alia_plugin.add(project.plugin_dirs(), out, alia_plugin.engine_version(), quiet=True)
    return content


# ---- commands --------------------------------------------------------------------------------------

def cmd_run(project, args):
    content = lay_out(project, project.build / "dev", link=True)
    env = dict(os.environ, PYTHONPATH=str(content / "python"))
    run([python_for(project), "main.py", *args], cwd=content, env=env)


def dev_env(project):
    """The development layout (build/dev/content, linked) and an environment that imports the game and the engine."""
    content = lay_out(project, project.build / "dev", link=True)
    return dict(os.environ, PYTHONPATH=os.pathsep.join([str(content), str(content / "python")]))


def cmd_exec(project, args):
    if not args:
        raise Fail("alia exec SCRIPT [ARGS...]")
    env = dev_env(project)
    run([python_for(project), *args], env=env)


def cmd_test(project, args):
    py = python_for(project)
    if subprocess.run([py, "-c", "import pytest"], capture_output=True).returncode:
        raise Fail(f"{py} has no pytest: python3 -m venv .venv && .venv/bin/pip install pytest (alia uses .venv)")
    run([py, "-m", "pytest", "-q", project.tests, *args], cwd=project.root, env=dev_env(project))


def cmd_cart(project, pc=False):
    out = project.build / ("cart-pc" if pc else "cart")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    for path in project.cart_dir.iterdir():
        _place(path, out / path.name, link=False)
    if not (out / "cart.toml").is_file():
        raise Fail(f"{project.cart_dir}/cart.toml: missing")
    lay_out(project, out, "pc" if pc else "board")
    if project.art:  # rendered with the PC engine, headless, from the real game
        run([python_for(project), project.art, out], cwd=project.root, env=dev_env(project))
    run([ACART, "check", out])
    if not pc:
        run([ACART, "pack", out, "-o", project.build / f"{project.name}.acart"])
    print(f"cart: {out}")


def cart_id(project):
    with open(project.cart_dir / "cart.toml", "rb") as f:
        return tomllib.load(f)["cart"]["id"]


def cmd_new(target):
    target = Path(target)
    if target.exists() and any(target.iterdir()):
        raise Fail(f"{target} is not empty")
    name = target.name
    app_id = "com.example." + "".join(c for c in name.lower() if c.isalnum())
    files = {
        "alia.toml": f'''# An alia-engine project (alia-engine docs/project.md).
[game]
name = "{name}"

[engine]
version = ">={alia_plugin.engine_version().rsplit(".", 1)[0]}, <1"
plugins = []        # e.g. ["tween"]: alia-engine's plugins by name, or a directory under plugins/
''',
        "cart/cart.toml": f'''[cart]
id          = "{app_id}"
title       = "{name}"
version     = "0.1.0"
publisher   = "me"
description = "A game made with alia-engine."

[runtime]
type  = "python"
entry = "main.py"

[display]
render = "640x360"

[input]
uses = ["gamepad"]
''',
        "src/main.py": f'''"""{name}: an alia-engine game (alia-engine docs/api.md §0)."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "python"))  # the cart's engine

import alia_engine as ae  # noqa: E402
import alia_kit as kit  # noqa: E402


class Game:
    def init(self, e):
        w = e.world
        e.actions.load({{"move": {{"stick": "left", "dead_zone": 0.15}}, "turn": {{"stick": "right"}}}})
        kit.environment(e, "day")
        kit.Terrain(e, size=100, cell=4, height=kit.hills(seed=1))
        self.me = w.spawn(e.create_mesh(kit.box(0.8, 0.8, 0.8, kit.pal.BLUE, centre=(0, 0.4, 0))))
        self.mover = kit.GroundMover(w, position=(0.0, 0.0), radius=0.4)
        self.cam = kit.FollowCamera(w.camera)

    def fixed_update(self, e, dt):
        a = e.actions
        self.cam.turn(a.axis("turn")[0], dt)
        self.mover.move(kit.stick_to_world(a.axis("move"), self.cam.yaw), dt)
        self.me.position, self.me.yaw = self.mover.position, self.mover.yaw
        self.cam.follow(e.world, self.mover.position, dt=dt)

    def render(self, e, alpha):
        kit.say(e, 8, 8, "{name.upper()}", kit.WHITE, 2)


if __name__ == "__main__":
    ae.run(Game(), argv=sys.argv[1:], title="{name}", app_id="{app_id}")
''',
        "tests/test_game.py": '''import alia_engine as ae

from main import Game


def test_it_runs():
    e = ae.Engine(headless=True)
    game = Game()
    game.init(e)
    for _ in range(60):
        game.fixed_update(e, 1 / 60)
    e.close()
''',
        ".gitignore": "build/\n.venv/\n__pycache__/\n",
    }
    for rel, text in files.items():
        path = target / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    for d in ("assets", "plugins", "tools", "pipeline"):
        (target / d).mkdir(exist_ok=True)
    print(f"made {target}: cd there and `alia run` (docs/project.md)")


def cmd_setup():
    """An `alia` command in ~/.local/bin that runs this tool (with the Python running it now)."""
    bin_dir = Path.home() / ".local" / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    launcher = bin_dir / "alia"
    launcher.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{Path(__file__).resolve()}" "$@"\n')
    launcher.chmod(0o755)
    on_path = str(bin_dir) in os.environ.get("PATH", "").split(os.pathsep)
    print(f"made {launcher}" + ("" if on_path else f"; add {bin_dir} to PATH"))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="alia", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("new", "run", "test", "exec", "cart", "install", "launch", "stop", "logs",
                                        "engine", "plugin", "setup"))
    ap.add_argument("rest", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    try:
        if a.command == "new":
            if len(a.rest) != 1:
                raise Fail("alia new DIR")
            return cmd_new(a.rest[0]) or 0
        if a.command == "setup":
            return cmd_setup()
        if a.command == "engine":
            build_engine(a.rest[0] if a.rest else "pc")
            return 0
        if a.command == "plugin":
            return alia_plugin.main(a.rest)
        project = Project.find()
        if a.command == "run":
            cmd_run(project, a.rest)
        elif a.command == "test":
            cmd_test(project, a.rest)
        elif a.command == "exec":
            cmd_exec(project, a.rest)
        elif a.command == "cart":
            cmd_cart(project, pc="--pc" in a.rest)
        elif a.command == "install":
            run([ACART, "--board", "install", project.build / f"{project.name}.acart"])
        elif a.command == "launch":
            run([ACART, "--board", "launch", "--replace", cart_id(project), *a.rest])
        elif a.command == "stop":
            run([ACART, "--board", "stop"])
        elif a.command == "logs":
            run(["adb", "shell", "-t", f"tail -n 50 -F $HOME/.local/share/alia/logs/{cart_id(project)}.log"])
    except Fail as err:
        print(f"alia: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
