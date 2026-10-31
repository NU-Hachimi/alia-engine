#!/usr/bin/env python3
"""alia: build, run, test and pack a game made with alia-engine (docs/project.md).

    alia new DIR                 a new project in the recommended layout (a small game that runs)
    alia run [ARGS...]           play it on the PC (window; ARGS go to the game: --demo, --headless, --frames N, ...)
    alia run --sim [ARGS...]     ... in the Alia Pad simulator instead (the devkit's acart run: the handheld's system)
    alia test [PYTEST ARGS...]   its tests, against a headless engine
    alia exec SCRIPT [ARGS...]   one of its tools (tools/...) with the game and the engine importable
    alia cart [--pc]             the cart: build/cart/ (board) or build/cart-pc/, checked; the board's packed too
    alia install | launch | stop | logs     the cart on the board (the devkit's acart)
    alia setup [--board]         get ready: the engine for this version (and the board's), acart; from a checkout,
                                 also an `alia` command in ~/.local/bin
    alia engine [pc|board]       build the engine module from source (run, test and cart do it when needed)
    alia plugin check DIR        is a plugin's plugin.toml right (docs/plugins.md)

Run it from the project (or anywhere under it): it finds alia.toml. Where the engine comes from:
  - installed (pip install alia-engine, docs/getting-started.md): the release of the same version, downloaded once
    into ~/.cache/alia-engine/<version>/ (ALIA_RELEASES to download from elsewhere). Nothing is compiled.
  - run from an alia-engine checkout (tools/alia.py): built from that source, shared by every project (build/ and
    build-board/ in the checkout); the board build needs the board's sysroot (ALIA_SYSROOT, from the Alia Pad devkit).
The cart commands use the Alia Pad devkit's acart (on PATH, or ACART). PYTHON is the interpreter for run and test
(default: the project's .venv if it has one, else python3).

What a cart holds (and `run` / `test` lay out the same way under build/dev/, with links, so edits show at once):
    cart.toml and the files next to it in cart/      the manifest and the art
    content/                                         the project's src/ (main.py at its top)
    content/assets/                                  the project's assets/
    content/python/                                  alia_engine, alia_kit, alia_pack and the plugins
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
try:  # installed: the alia_cli package
    from . import alia_plugin
except ImportError:  # tools/alia.py in a checkout
    sys.path.insert(0, str(HERE))
    import alia_plugin

# Run from an alia-engine checkout: the engine is built from that source. Installed: release builds are downloaded.
SOURCE = HERE.parent if (HERE.parent / "CMakeLists.txt").is_file() and (HERE.parent / "src" / "bindings").is_dir() else None
_BESIDE = Path(sys.executable).parent / "acart"  # alia-sdk installed in the same environment as this alia
ACART = os.environ.get("ACART") or shutil.which("acart") or (str(_BESIDE) if _BESIDE.is_file() else "acart")
BUILDS = {"pc": "build", "board": "build-board"}  # in the checkout
ARCH = {"pc": "x86_64", "board": "aarch64"}
RELEASES = os.environ.get("ALIA_RELEASES", "https://github.com/NU-Hachimi/alia-engine/releases/download")
# The Alia Pad devkit's SDK (acart) that this engine version is made with; `alia setup` installs it when acart is missing.
ALIA_SDK = os.environ.get("ALIA_SDK", "https://github.com/NU-Hachimi/alia-devkit/releases/download/sdk-v0.1.0/"
                                      "alia_sdk-0.1.0-py3-none-any.whl")


def engine_version():
    if SOURCE:
        return alia_plugin.engine_version()
    from importlib.metadata import version
    return version("alia-engine")


VERSION = engine_version()
BOARD_PYTHON = (3, 13)   # what Alia Pad gives python carts (platform 1.0; the devkit's docs/platform.md)
MIN_PYTHON = (3, 12)     # the engine module is abi3: it imports on 3.12 and newer
CACHE = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "alia-engine" / VERSION


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
        if not alia_plugin.version_ok(VERSION, self.engine_spec):
            raise Fail(f"{self.name} needs alia-engine {self.engine_spec}; this alia is {VERSION} "
                       "(pip install the version it needs, docs/getting-started.md)")

    @staticmethod
    def find(start=None):
        here = Path(start or Path.cwd()).resolve()
        for d in [here, *here.parents]:
            if (d / "alia.toml").is_file():
                return Project(d)
        raise Fail("no alia.toml here or above (alia new DIR makes a project)")

    def plugin_dirs(self, first_party):
        """Each plugin of alia.toml: a directory in the project (plugins/<name>, or a path), or a first-party one by
        name (the engine's plugins/<name>, in `first_party`)."""
        out = []
        for p in self.plugins:
            for d in (self.root / p, self.root / "plugins" / p, first_party / p):
                if (d / "plugin.toml").is_file():
                    out.append(d)
                    break
            else:
                raise Fail(f"plugin {p!r}: not in {self.root / 'plugins'} nor among alia-engine's plugins/")
        return out


def run(cmd, **kw):
    print("+ " + " ".join(str(c) for c in cmd), file=sys.stderr)
    try:
        code = subprocess.run([str(c) for c in cmd], **kw).returncode
    except FileNotFoundError:
        hint = " (the Alia Pad devkit's: alia setup installs it)" if str(cmd[0]) == ACART else ""
        raise Fail(f"{cmd[0]}: not found{hint}") from None
    if code:
        raise Fail(f"{Path(str(cmd[0])).name} failed")


def build_engine(target="pc"):
    """The engine module for the PC or the board, built in the engine checkout (configured the first time)."""
    if not SOURCE:
        raise Fail("this alia is installed: it uses release builds (alia engine is for an alia-engine checkout)")
    out = SOURCE / BUILDS[target]
    if not (out / "build.ninja").exists():
        sysroot = os.environ.get("ALIA_SYSROOT", "")
        if target == "board" and not sysroot:
            raise Fail("the board build needs the board's sysroot: ALIA_SYSROOT (docs/getting-started.md)")
        extra = ["-DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-board.cmake", f"-DALIA_SYSROOT={sysroot}"] if target == "board" else []
        run(["cmake", "-S", SOURCE, "-B", out, "-G", "Ninja", *extra])
    run(["cmake", "--build", out, "--target", "alia_engine"], stdout=subprocess.DEVNULL)
    return out


def fetch(target):
    """This version's release build for the PC or the board, downloaded once (and checked against SHA256SUMS)."""
    cache = CACHE / ARCH[target]
    if (cache / "python" / "alia_engine.abi3.so").is_file():
        return cache
    name = f"alia-engine-{VERSION}-linux-{ARCH[target]}.tar.gz"
    base = f"{RELEASES}/v{VERSION}"
    print(f"alia: downloading {name}", file=sys.stderr)
    try:
        sums = urllib.request.urlopen(f"{base}/SHA256SUMS", timeout=30).read().decode()
        data = urllib.request.urlopen(f"{base}/{name}", timeout=300).read()
    except OSError as err:
        raise Fail(f"could not download {base}/{name}: {err}") from None
    want = next((line.split()[0] for line in sums.splitlines() if line.endswith(name)), None)
    if want != hashlib.sha256(data).hexdigest():
        raise Fail(f"{name}: its checksum doesn't match SHA256SUMS")
    with tempfile.TemporaryDirectory(dir=CACHE.parent if CACHE.parent.exists() else None) as tmp:
        archive = Path(tmp) / name
        archive.write_bytes(data)
        with tarfile.open(archive) as tar:
            tar.extractall(tmp, filter="data")
        cache.parent.mkdir(parents=True, exist_ok=True)
        if cache.exists():
            shutil.rmtree(cache)
        shutil.move(str(Path(tmp) / f"alia-engine-{VERSION}-linux-{ARCH[target]}"), cache)
    return cache


def build_engine_debian():
    """The PC module for carts, from a checkout: built in the release's Debian 13 container (tools/release/Dockerfile,
    the platform's baseline), since a module built on a newer host (e.g. Arch's glibc) can't load in the simulator
    or on another PC. Direct runs and tests keep using the fast local build."""
    image = "alia-engine-build:trixie"
    run(["docker", "build", "-q", "-t", image, SOURCE / "tools" / "release"], stdout=subprocess.DEVNULL)
    out = "build-release/x86_64"
    script = (f"[ -f {out}/build.ninja ] || cmake -S . -B {out} -G Ninja -DCMAKE_BUILD_TYPE=Release -DALIA_BUILD_TESTS=OFF "
              f"-DALIA_BUILD_APPS=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > /dev/null && "
              f"cmake --build {out} --target alia_engine > /dev/null && rm -rf {out}/alia-stage && "
              f"cmake --install {out} --prefix {out}/alia-stage > /dev/null")  # paths are the container's
    run(["docker", "run", "--rm", "-u", f"{os.getuid()}:{os.getgid()}", "-v", f"{SOURCE}:/src", "-w", "/src", image,
         "sh", "-c", script])
    return SOURCE / out / "alia-stage" / "python"


def engine_files(target):
    """(python/ with alia_engine, alia_kit and alia_pack, the first-party plugins/) for "pc" (direct runs), "pc-cart"
    (a PC cart: the simulator) or "board"."""
    if not SOURCE:
        cache = fetch("pc" if target == "pc-cart" else target)  # release builds are Debian-built already
        return cache / "python", cache / "plugins"
    if target == "pc-cart":
        return build_engine_debian(), SOURCE / "plugins"
    out = build_engine(target)
    stage = out / "alia-stage"
    if stage.exists():
        shutil.rmtree(stage)
    run(["cmake", "--install", out, "--prefix", stage], stdout=subprocess.DEVNULL)
    return stage / "python", SOURCE / "plugins"


def python_for(project):
    """The interpreter that runs the game here: PYTHON, the project's .venv, or the one running alia. Below 3.12 the
    engine can't import (an error); other than the board's 3.13, a warning: the game runs on 3.13 on the board."""
    venv = project.root / ".venv" / "bin" / "python"
    py = os.environ.get("PYTHON") or (str(venv) if venv.exists() else sys.executable)
    if py not in _CHECKED:
        _CHECKED.add(py)
        r = subprocess.run([py, "-c", "import sys; print(*sys.version_info[:2])"], capture_output=True, text=True)
        have = tuple(int(v) for v in r.stdout.split()) if r.returncode == 0 else None
        if have is None:
            raise Fail(f"{py}: not a Python interpreter")
        if have < MIN_PYTHON:
            raise Fail(f"{py} is Python {have[0]}.{have[1]}: alia-engine needs 3.12 or newer, the board runs 3.13 "
                       "(docs/getting-started.md)")
        if have != BOARD_PYTHON:
            print(f"alia: warning: {py} is Python {have[0]}.{have[1]}, the board runs {BOARD_PYTHON[0]}.{BOARD_PYTHON[1]}: "
                  "syntax or standard library newer than that won't be there (develop on 3.13: docs/getting-started.md)",
                  file=sys.stderr)
    return py


_CHECKED = set()


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
    engine, first_party = engine_files(target)
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
    shutil.copytree(engine, py, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))  # the engine is copied
    alia_plugin.add(project.plugin_dirs(first_party), out, VERSION, quiet=True)
    if link and (sdk := sdk_package()):  # development only: in a cart the system provides `alia`
        (py / "alia").symlink_to(sdk, target_is_directory=True)
    return content


# ---- commands --------------------------------------------------------------------------------------

def sdk_package():
    """The Alia Pad SDK's `alia` package as installed beside this tool (alia setup), or None. Development layouts link
    it into content/python/ so the engine reads the platform's rules (alia.runtime) as it does in a cart, where the
    system provides it. Run as tools/alia.py, this file's own directory would make `import alia` this tool."""
    import importlib.util
    saved = list(sys.path)
    try:
        sys.path[:] = [p for p in sys.path if Path(p or ".").resolve() != HERE]
        spec = importlib.util.find_spec("alia") if "alia" not in sys.modules or sys.modules["alia"].__name__ == "alia" else None
    except (ImportError, ValueError):
        spec = None
    finally:
        sys.path[:] = saved
    if spec is None or not spec.origin or Path(spec.origin).resolve() == Path(__file__).resolve():
        return None
    return Path(spec.origin).parent


def cmd_run(project, args):
    if "--sim" in args:  # the cart, PC build, in the devkit's simulator (alia-sdk 0.2+: acart run)
        args = [a for a in args if a != "--sim"]
        try:
            has_run = subprocess.run([ACART, "run", "--help"], capture_output=True).returncode == 0
        except FileNotFoundError:
            raise Fail("acart: not found (the Alia Pad devkit's: alia setup installs it)") from None
        if not has_run:
            raise Fail("--sim needs the Alia Pad devkit's simulator (acart run, alia-sdk 0.2 or newer)")
        out = cmd_cart(project, pc=True)
        run([ACART, "run", out, *args])
        return
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
    lay_out(project, out, "pc-cart" if pc else "board")
    if project.art:  # rendered with the PC engine, headless, from the real game
        run([python_for(project), project.art, out], cwd=project.root, env=dev_env(project))
    if not pc:
        run([ACART, "check", out])
        run([ACART, "pack", out, "-o", project.build / f"{project.name}.acart"])
    elif "--pc" in subprocess.run([ACART, "check", "--help"], capture_output=True, text=True).stdout:
        run([ACART, "check", "--pc", out])  # x86-64 files, the platform's symbol versions (alia-sdk 0.2+)
    print(f"cart: {out}")
    return out


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
version = ">={VERSION.rsplit(".", 1)[0]}, <1"
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


def cmd_setup(board=False):
    """Get ready: the engine for the PC (and the board's with --board) and acart; from a checkout, an `alia` command."""
    if SOURCE:  # a checkout: a launcher for this tool, the engine is built from source when needed
        bin_dir = Path.home() / ".local" / "bin"
        bin_dir.mkdir(parents=True, exist_ok=True)
        launcher = bin_dir / "alia"
        launcher.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{Path(__file__).resolve()}" "$@"\n')
        launcher.chmod(0o755)
        on_path = str(bin_dir) in os.environ.get("PATH", "").split(os.pathsep)
        print(f"made {launcher}" + ("" if on_path else f"; add {bin_dir} to PATH"))
    else:
        for target in ("pc", "board") if board else ("pc",):
            print(f"engine {VERSION} for the {target}: {fetch(target)}")
    acart = shutil.which(ACART) or (ACART if Path(ACART).is_file() else None)
    if not acart and not SOURCE:  # installed with pip: put the devkit's SDK next to it
        print(f"acart: installing the Alia Pad devkit's SDK ({ALIA_SDK})")
        if subprocess.run([sys.executable, "-m", "pip", "install", "-q", f"alia-sdk @ {ALIA_SDK}"]).returncode == 0:
            acart = shutil.which("acart") or str(Path(sys.executable).parent / "acart")
    if acart and (shutil.which(acart) or Path(acart).is_file()):
        print(f"acart: {acart}")
    else:
        print("acart: not found. The cart commands (alia cart, install, launch) need the Alia Pad devkit's acart: "
              f"pip install '{ALIA_SDK}' (docs/getting-started.md), or set ACART")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="alia", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"alia {VERSION} ({'source: ' + str(SOURCE) if SOURCE else 'release'})")
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
            return cmd_setup(board="--board" in a.rest)
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
