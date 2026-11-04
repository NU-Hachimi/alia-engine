"""`alia assets`: build a project's assets/ from its pipeline/ sources (docs/import.md). Runs in the runtime image
(alia.py copies it and alia_import.py into build/tools/ and starts it there); pure Python, so a test can call
main() directly.

    alia assets            build what changed (assets/.alia-assets.json remembers what each output was made from)
    alia assets --force    build everything again
    alia assets --check    build in memory and compare: fails when a committed output is stale (CI)

alia.toml:

    [assets]
    build = "pipeline/build.py:build"         # optional: the game's own builder, build(ctx), after the packs

    [[assets.pack]]
    out = "assets/props.apak"
    glb = ["pipeline/models/*.glb"]           # each: alia_import.import_glb, prefix = the file's name
    png = ["pipeline/textures/*.png"]         # each: alia_import.add_png, name = the file's name
    options = { colors = "palette", mipmaps = true }     # for every file of this pack
    [assets.pack.files."pipeline/textures/floor.png"]   # for one file
    wrap = "repeat"
"""

import argparse
import glob
import hashlib
import importlib.util
import json
import os
import sys
import tomllib

import alia_import
import alia_pack

VERSION = 1
STAMP = ".alia-assets.json"
GLB_OPTIONS = {"prefix", "merge", "colors", "shading", "scale", "filter", "mipmaps", "palette"}
PNG_OPTIONS = {"name", "filter", "wrap", "mipmaps"}
PACK_KEYS = {"out", "glb", "png", "options", "files"}


class Fail(Exception):
    pass


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _read(path):
    with open(path, "rb") as f:
        return f.read()


class Context:
    """What a builder gets (build(ctx)): where things are, the importers, and incremental writes.

    ctx.root      the project            ctx.sources   its pipeline/         ctx.out   its assets/
    ctx.options   alia.toml [assets.options]
    ctx.writer()  a new alia_pack.PackWriter; ctx.write_pack("x.apak", writer) writes it under ctx.out
    ctx.write("parts.json", data)          any other file under ctx.out (bytes or str)
    ctx.stale(inputs, outputs)             True when an output must be made again: missing, or its inputs (paths,
                                           relative to the project) changed since it was written; always True with
                                           --force and --check
    ctx.import_glb, ctx.add_png, ctx.read_png, ctx.read_glb      alia_import's functions
    ctx.warn(text)
    ctx.check     True under --check: writes are compared, not made (write only through ctx.write / write_pack)
    """

    import_glb = staticmethod(alia_import.import_glb)
    add_png = staticmethod(alia_import.add_png)
    read_png = staticmethod(alia_import.read_png)
    read_glb = staticmethod(alia_import.read_glb)

    def __init__(self, builder):
        self._b = builder
        self.root, self.out = builder.root, builder.out
        self.sources = os.path.join(builder.root, "pipeline")
        self.options = builder.config.get("options", {})
        self.check = builder.check
        self._pending = {}  # output (relative to the project) -> the key stale() computed for it
        self._extra = builder.builder_hash

    @staticmethod
    def writer():
        return alia_pack.PackWriter()

    def warn(self, text):
        self._b.warnings.append(text)

    def stale(self, inputs, outputs):
        key = self._b.key(list(inputs), extra=self._extra)
        any_stale = self._b.force or self._b.check
        for o in outputs:
            rel = self._b.rel(os.path.join(self.out, o))
            self._pending[rel] = key
            if not self._b.fresh(rel, key):
                any_stale = True
        return any_stale

    def write(self, relpath, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        rel = self._b.rel(os.path.join(self.out, relpath))
        self._b.output(rel, bytes(data), self._pending.get(rel))

    def write_pack(self, relpath, writer):
        self.write(relpath, writer.bytes())


class Builder:
    def __init__(self, root, check=False, force=False, out=None):
        self.root = os.path.abspath(root)
        with open(os.path.join(self.root, "alia.toml"), "rb") as f:
            doc = tomllib.load(f)
        self.config = doc.get("assets", {})
        game = doc.get("game", {})
        self.out = os.path.join(self.root, out or game.get("assets", "assets"))
        self.check, self.force = check, force
        self.warnings, self.stale_outputs, self.report = [], [], []
        stamp = os.path.join(self.out, STAMP)
        try:
            self.stamp = json.loads(_read(stamp)).get("outputs", {}) if os.path.exists(stamp) else {}
        except (ValueError, OSError):
            self.stamp = {}
        self.builder_hash = ""

    # -- paths and stamps ---------------------------------------------------------------------------------------
    def rel(self, path):
        rel = os.path.relpath(os.path.abspath(path), self.root).replace(os.sep, "/")
        if rel.startswith("../"):
            raise Fail(f"{path}: outside the project")
        return rel

    def key(self, inputs, spec=None, extra=""):
        h = hashlib.sha256(json.dumps([VERSION, alia_import.VERSION, alia_pack.VERSION, spec, extra],
                                      sort_keys=True).encode())
        for path in sorted(set(inputs)):
            full = os.path.join(self.root, path)
            h.update(path.encode() + b"\0" + (_sha(_read(full)).encode() if os.path.isfile(full) else b"missing"))
        return h.hexdigest()

    def fresh(self, rel, key):
        s = self.stamp.get(rel)
        full = os.path.join(self.root, rel)
        return bool(s and key and s.get("key") == key and os.path.isfile(full) and _sha(_read(full)) == s.get("sha256"))

    def output(self, rel, data, key):
        """An output made: compared (--check) or written (only when its bytes changed), and stamped."""
        full = os.path.join(self.root, rel)
        old = _read(full) if os.path.isfile(full) else None
        if self.check:
            if old != data:
                self.stale_outputs.append(rel)
            return
        if old != data:
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "wb") as f:
                f.write(data)
            self.report.append(f"built {rel} ({len(data) // 1024} KB)")
        else:
            self.report.append(f"same  {rel}")
        self.stamp[rel] = {"key": key, "sha256": _sha(data)}

    # -- packs -------------------------------------------------------------------------------------------------
    def files(self, patterns, what, out):
        found = []
        for p in ([patterns] if isinstance(patterns, str) else patterns):
            hits = sorted(glob.glob(os.path.join(self.root, p), recursive=True))
            if not hits:
                self.warnings.append(f"{out}: {what} {p!r} matches nothing")
            found += [self.rel(h) for h in hits if os.path.isfile(h)]
        return found

    @staticmethod
    def gltf_inputs(path):
        """A .gltf's own files (its .bin and images), so editing them rebuilds."""
        if not path.endswith(".gltf"):
            return []
        try:
            doc = json.loads(_read(path))
        except (OSError, ValueError):
            return []
        base = os.path.dirname(path)
        return [os.path.join(base, x["uri"].replace("%20", " ")) for x in doc.get("buffers", []) + doc.get("images", [])
                if "uri" in x and not x["uri"].startswith("data:")]

    def pack(self, spec):
        unknown = set(spec) - PACK_KEYS
        if unknown or "out" not in spec:
            raise Fail(f"[[assets.pack]]: needs out = \"assets/....apak\"; unknown keys {sorted(unknown)}")
        out = self.rel(os.path.join(self.root, spec["out"]))
        options, per_file = spec.get("options", {}), spec.get("files", {})
        bad = set(options) - GLB_OPTIONS - PNG_OPTIONS
        for f, o in per_file.items():
            bad |= set(o) - GLB_OPTIONS - PNG_OPTIONS
        if bad:
            raise Fail(f"{out}: unknown options {sorted(bad)} (glb: {sorted(GLB_OPTIONS)}; png: {sorted(PNG_OPTIONS)})")
        glbs, pngs = self.files(spec.get("glb", []), "glb", out), self.files(spec.get("png", []), "png", out)
        inputs = glbs + pngs + [self.rel(x) for g in glbs for x in self.gltf_inputs(os.path.join(self.root, g))]
        key = self.key(inputs, spec)
        if not self.force and not self.check and self.fresh(out, key):
            self.report.append(f"ok    {out}")
            return
        w = alia_pack.PackWriter()
        try:
            for g in glbs:
                o = {k: v for k, v in {**options, **per_file.get(g, {})}.items() if k in GLB_OPTIONS}
                alia_import.import_glb(w, os.path.join(self.root, g), warnings=self.warnings, **o)
            for p in pngs:
                o = {k: v for k, v in {**options, **per_file.get(p, {})}.items() if k in PNG_OPTIONS}
                alia_import.add_png(w, os.path.join(self.root, p), **o)
            data = w.bytes()
        except (alia_import.AssetError, alia_pack.PackError, OSError) as err:
            raise Fail(f"{out}: {err}") from None
        self.output(out, data, key)

    # -- the game's builder -------------------------------------------------------------------------------------
    def run_builder(self, ref):
        path, _, func = ref.partition(":")
        full = os.path.join(self.root, path)
        if not func or not os.path.isfile(full):
            raise Fail(f"[assets] build = {ref!r}: \"path/to/file.py:function\" in the project")
        self.builder_hash = _sha(_read(full))
        sys.path.insert(0, os.path.dirname(full))
        spec = importlib.util.spec_from_file_location("alia_assets_builder", full)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        if not callable(getattr(module, func, None)):
            raise Fail(f"{path}: no function {func}")
        getattr(module, func)(Context(self))

    def run(self):
        for spec in self.config.get("pack", []):
            self.pack(spec)
        if self.config.get("build"):
            self.run_builder(self.config["build"])
        if not self.check:
            stamp = os.path.join(self.out, STAMP)
            data = json.dumps({"version": VERSION, "outputs": dict(sorted(self.stamp.items()))}, indent=1, sort_keys=True) + "\n"
            if not os.path.isfile(stamp) or _read(stamp) != data.encode():
                os.makedirs(self.out, exist_ok=True)
                with open(stamp, "w") as f:
                    f.write(data)
        return self


def main(argv=None):
    ap = argparse.ArgumentParser(prog="alia assets", description="Build assets/ from pipeline/ (docs/import.md).")
    ap.add_argument("--root", default=".")
    ap.add_argument("--check", action="store_true", help="build in memory; fail when a committed output is stale")
    ap.add_argument("--force", action="store_true", help="build everything again")
    a = ap.parse_args(argv)
    try:
        b = Builder(a.root, check=a.check, force=a.force)
        if not b.config:
            print("alia assets: nothing to build (no [assets] in alia.toml; docs/import.md)")
            return 0
        b.run()
    except Fail as err:
        print(f"alia assets: {err}", file=sys.stderr)
        return 1
    for line in b.report:
        print(line)
    for w in b.warnings:
        print(f"warning: {w}", file=sys.stderr)
    if b.check:
        if b.stale_outputs:
            print("stale (run alia assets): " + ", ".join(b.stale_outputs), file=sys.stderr)
            return 1
        print("assets up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
