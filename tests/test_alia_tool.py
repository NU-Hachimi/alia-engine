"""tools/alia.py (docs/project.md): a new project runs and tests; a project's layout carries its plugins."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import alia  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp) / "hello"
    alia.cmd_new(root)
    check(all((root / p).exists() for p in ("alia.toml", "cart/cart.toml", "src/main.py", "tests/test_game.py",
                                            "assets", "plugins", "tools", "pipeline", "pyrightconfig.json")), "new: the recommended layout, an editor config")
    project = alia.Project.find(root / "src")
    check(project.root == root and project.name == "hello", "the project is found from inside it")
    content = alia.lay_out(project, project.build / "dev", link=True)
    check((content / "main.py").is_symlink() and (content / "python" / "alia_kit.py").is_file()
          and (content / "python" / "alia_pack.py").is_file(), "dev layout: the game linked, the engine copied")
    env = dict(os.environ, PYTHONPATH=str(content / "python"))
    shot = Path(tmp) / "shot.png"
    r = subprocess.run([sys.executable, "main.py", "--headless", "--frames", "30", "--screenshot", str(shot)],
                       cwd=content, env=env, capture_output=True, text=True)
    check(r.returncode == 0 and shot.stat().st_size > 1000, "the new game runs headless")

    # alia run: the PC cart, copied, handed to the simulator (a stand-in acart; this build's engine, not Debian's)
    fake, calls = Path(tmp) / "acart", Path(tmp) / "calls"
    fake.write_text(f'#!/bin/sh\necho "$@" >> {calls}\n')
    fake.chmod(0o755)
    saved = alia.ACART, alia.engine_files
    alia.ACART = str(fake)
    alia.engine_files = lambda target: (content / "python", REPO / "plugins")
    try:
        alia.cmd_run(project, ["--frames", "5"])
    finally:
        alia.ACART, alia.engine_files = saved
    cart = project.build / "cart-pc"
    check(f"run {cart} --frames 5" in calls.read_text().splitlines()
          and (cart / "content" / "main.py").is_file() and not (cart / "content" / "main.py").is_symlink()
          and (cart / "cart.toml").is_file(), "run: the cart copied into build/cart-pc, run with acart run")

sample = alia.Project.find(REPO / "apps" / "sample")
with tempfile.TemporaryDirectory() as tmp:
    content = alia.lay_out(sample, Path(tmp), link=False)
    check((content / "python" / "alia_tween" / "plugin.toml").is_file() and not (content / "main.py").is_symlink(),
          "the sample's layout carries the tween plugin (alia.toml) and copies its files")
import alia_engine  # noqa: E402  (PYTHONPATH: the build)
pyproject = (REPO / "pyproject.toml").read_text()
check(alia.VERSION == alia_engine.__version__ and f'version = "{alia.VERSION}"' in pyproject,
      f"one version everywhere: CMakeLists.txt, the module, pyproject.toml ({alia.VERSION})")
try:
    alia.Project.find(Path("/"))
    check(False, "outside a project: an error")
except alia.Fail:
    check(True, "outside a project: an error")
sys.exit(1 if failures else 0)
