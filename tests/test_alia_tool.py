"""tools/alia.py (docs/project.md): a new project runs and tests; a project's layout carries its plugins."""

import os
import shutil
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
    engine = Path(tmp) / "engine"
    shutil.copytree(content / "python", engine)
    alia.engine_files = lambda target: (engine, REPO / "plugins")
    try:
        alia.cmd_run(project, ["--frames", "5"])
        alia.cmd_test(project, ["-k", "runs"])
        check(f"exec --env PYTHONPATH=/work/build/dev/content:/work/build/dev/content/python {root} -- pytest -q "
              "/work/tests -k runs" in calls.read_text().splitlines()
              and os.readlink(project.build / "dev" / "content" / "main.py") == "../../../src/main.py",
              "test: pytest in the image (acart exec), the project at /work, the layout linked relatively")

        # cart art ([art]): a run renders it once (the stand-in acart renders nothing), then reuses build/art/
        (root / "cart" / "cart.toml").write_text((root / "cart" / "cart.toml").read_text() + '\n[art]\nicon = "icon.png"\n')
        project.art = root / "tools" / "art.py"
        before = len(calls.read_text().splitlines())
        alia.cmd_run(project, [])
        rendered = any("art.py" in c for c in calls.read_text().splitlines()[before:])
        (project.build / "art").mkdir(exist_ok=True)
        (project.build / "art" / "icon.png").write_bytes(b"png")
        before = len(calls.read_text().splitlines())
        alia.cmd_run(project, [])
        check(rendered and not any("art.py" in c for c in calls.read_text().splitlines()[before:])
              and (project.build / "cart-pc" / "icon.png").read_bytes() == b"png",
              "run: the cart art rendered once, then copied from build/art/")
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
import re  # noqa: E402
calls_text = [m.group(0) for f in ("alia.py", "alia_plugin.py")
              for m in re.finditer(r"\.(write_text|read_text)\((?:[^()]|\([^()]*\))*\)", (REPO / "tools" / f).read_text())]
check(calls_text and all("encoding=" in c for c in calls_text),
      "the tool reads and writes text as UTF-8, never the system's encoding (cp1252, GBK on Windows)")
try:
    alia.Project.find(Path("/"))
    check(False, "outside a project: an error")
except alia.Fail:
    check(True, "outside a project: an error")
sys.exit(1 if failures else 0)
