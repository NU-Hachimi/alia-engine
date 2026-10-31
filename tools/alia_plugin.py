#!/usr/bin/env python3
"""Put alia-engine plugins (docs/plugins.md) into a cart: a plugin is a Python package plus a plugin.toml; adding it
copies the package into the cart's content/python/ (next to alia_engine and alia_kit), with its plugin.toml, after
checking that it works with the engine the cart carries.

    tools/alia_plugin.py check PLUGIN_DIR                 is the plugin.toml right (also: alia plugin check)
    tools/alia_plugin.py add PLUGIN_DIR... STAGE_DIR      copy into STAGE_DIR/content/python/
    tools/alia_plugin.py list STAGE_DIR                   the plugins in a staged cart
    --engine VERSION    the cart's engine version (default: this repository's, CMakeLists.txt)

A staged cart is the directory acart packs (cart.toml, content/). Nothing here touches the board or cart.toml:
plugins are part of the game's files, like its own modules.
"""

import argparse
import re
import shutil
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
_CLAUSE = re.compile(r"^\s*(>=|<=|==|!=|>|<)\s*(\d+(?:\.\d+){0,2})\s*$")


def engine_version():
    m = re.search(r"project\(alia_engine VERSION (\d+\.\d+\.\d+)", (REPO / "CMakeLists.txt").read_text())
    return m.group(1)


def _parts(v):
    p = [int(x) for x in v.split(".")]
    return tuple(p + [0] * (3 - len(p)))


def version_ok(version, spec):
    """Whether version ("0.2.0") meets spec (">=0.1, <1": clauses with >= <= > < == !=, all of them)."""
    have = _parts(version)
    ops = {">=": lambda a, b: a >= b, "<=": lambda a, b: a <= b, ">": lambda a, b: a > b, "<": lambda a, b: a < b,
           "==": lambda a, b: a == b, "!=": lambda a, b: a != b}
    for clause in spec.split(","):
        m = _CLAUSE.match(clause)
        if not m:
            raise ValueError(f"engine = {spec!r}: {clause.strip()!r} is not a version clause like '>=0.2'")
        if not ops[m.group(1)](have, _parts(m.group(2))):
            return False
    return True


def read_manifest(plugin_dir):
    """plugin.toml's [plugin] table, checked; raises ValueError with what is wrong."""
    plugin_dir = Path(plugin_dir)
    path = plugin_dir / "plugin.toml"
    if not path.is_file():
        raise ValueError(f"{plugin_dir}: no plugin.toml")
    with open(path, "rb") as f:
        doc = tomllib.load(f)
    p = doc.get("plugin")
    if not isinstance(p, dict):
        raise ValueError(f"{path}: no [plugin] table")
    for key in ("name", "package", "version", "engine"):
        if not isinstance(p.get(key), str) or not p[key]:
            raise ValueError(f"{path}: [plugin] {key} is missing")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", p["package"]):
        raise ValueError(f"{path}: package {p['package']!r} is not a Python package name")
    if p["package"] in ("alia_engine", "alia_kit"):
        raise ValueError(f"{path}: package {p['package']!r} is the engine's own")
    if not (plugin_dir / p["package"] / "__init__.py").is_file():
        raise ValueError(f"{path}: no package {p['package']}/__init__.py next to it")
    version_ok("0.0.0", p["engine"])  # the spec parses
    return p


def add(plugin_dirs, stage, engine, quiet=False):
    """Copy each plugin's package into stage/content/python/ (a cart's layout), after checking its engine range."""
    dest_root = Path(stage) / "content" / "python"
    dest_root.mkdir(parents=True, exist_ok=True)
    for d in plugin_dirs:
        p = read_manifest(d)
        if not version_ok(engine, p["engine"]):
            raise ValueError(f"plugin {p['name']} {p['version']} needs alia-engine {p['engine']}; the cart has {engine}")
        dest = dest_root / p["package"]
        if dest.exists():
            shutil.rmtree(dest)
        shutil.copytree(Path(d) / p["package"], dest, ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
        shutil.copy2(Path(d) / "plugin.toml", dest / "plugin.toml")
        if not quiet:
            print(f"added {p['name']} {p['version']} -> {dest}")


def installed(stage):
    out = []
    for toml in sorted((Path(stage) / "content" / "python").glob("*/plugin.toml")):
        with open(toml, "rb") as f:
            out.append(tomllib.load(f)["plugin"])
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("check", "add", "list"))
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--engine", default=None, help="the cart's engine version (default: this repository's)")
    a = ap.parse_args(argv)
    try:
        if a.command == "check":
            for d in a.paths:
                p = read_manifest(d)
                print(f"ok {p['name']} {p['version']} (package {p['package']}, engine {p['engine']})")
        elif a.command == "add":
            if len(a.paths) < 2:
                ap.error("add PLUGIN_DIR... STAGE_DIR")
            if not (Path(a.paths[-1]) / "cart.toml").is_file():
                raise ValueError(f"{a.paths[-1]}: not a staged cart (no cart.toml)")
            add(a.paths[:-1], a.paths[-1], a.engine or engine_version())
        else:
            for p in installed(a.paths[0]):
                print(f"{p['name']:16} {p['version']:8} {p['package']:20} engine {p['engine']}")
    except ValueError as err:
        print(f"alia_plugin: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
