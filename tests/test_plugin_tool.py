"""tools/alia_plugin.py (docs/plugins.md): manifests, the engine version check, adding to a staged cart."""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import alia_plugin as tool  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


check(tool.version_ok("0.2.0", ">=0.1, <1") and not tool.version_ok("1.0.0", ">=0.1, <1")
      and not tool.version_ok("0.1.9", ">=0.2") and tool.version_ok("0.2", "==0.2.0"), "version clauses")
try:
    tool.version_ok("0.2.0", "about 0.2")
    check(False, "a bad clause is refused")
except ValueError:
    check(True, "a bad clause is refused")
check(tool.engine_version().count(".") == 2, f"the repository's engine version ({tool.engine_version()})")

m = tool.read_manifest(REPO / "plugins" / "tween")
check(m["package"] == "alia_tween" and m["name"] == "tween", "plugins/tween's manifest")

with tempfile.TemporaryDirectory() as tmp:
    stage = Path(tmp) / "cart"
    (stage / "content" / "python").mkdir(parents=True)
    (stage / "cart.toml").write_text("[cart]\n")
    tool.add([REPO / "plugins" / "tween"], stage, "0.1.0")
    dest = stage / "content" / "python" / "alia_tween"
    check((dest / "__init__.py").is_file() and (dest / "plugin.toml").is_file() and not (dest / "__pycache__").exists(),
          "add: the package and its plugin.toml in content/python")
    check([p["name"] for p in tool.installed(stage)] == ["tween"], "list: what a cart carries")
    try:
        tool.add([REPO / "plugins" / "tween"], stage, "1.2.0")
        check(False, "a plugin for another engine is refused")
    except ValueError as err:
        check("needs alia-engine" in str(err), "a plugin for another engine is refused")
    bad = Path(tmp) / "bad"
    (bad / "alia_kit").mkdir(parents=True)
    (bad / "alia_kit" / "__init__.py").write_text("")
    (bad / "plugin.toml").write_text('[plugin]\nname = "bad"\npackage = "alia_kit"\nversion = "1"\nengine = ">=0.1"\n')
    try:
        tool.read_manifest(bad)
        check(False, "a plugin may not take the engine's names")
    except ValueError:
        check(True, "a plugin may not take the engine's names")

sys.exit(1 if failures else 0)
