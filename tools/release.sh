#!/bin/bash
# Build a release of alia-engine into dist/v<version>/ (docs/getting-started.md): what `pip install` and `alia setup`
# download.
#   tools/release.sh            build, and check it installs and runs
#   tools/release.sh --publish  ... then make the GitHub release v<version> with it (gh)
# Both engine modules are built in the Debian 13 container of tools/release/Dockerfile (the board's baseline): the
# PC one natively, the board one cross-compiled against the board's sysroot (ALIA_SYSROOT, from the Alia Pad devkit).
#   alia-engine-<v>-linux-x86_64.tar.gz, -linux-aarch64.tar.gz    python/ (alia_engine + its .pyi, alia_kit, alia_pack), plugins/
#   alia_engine-<v>-py3-none-any.whl                              the alia command (pip install)
#   SHA256SUMS
set -euo pipefail
cd "$(dirname "$0")/.."
V=$(sed -n 's/^project(alia_engine VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
grep -q "^version = \"$V\"" pyproject.toml || { echo "pyproject.toml's version isn't $V (CMakeLists.txt)" >&2; exit 1; }
[ -n "${ALIA_SYSROOT:-}" ] && [ -d "$ALIA_SYSROOT/usr/include" ] || { echo "ALIA_SYSROOT: the board's sysroot" >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || echo "warning: uncommitted changes go into the release" >&2
DIST=dist/v$V
rm -rf "$DIST" && mkdir -p "$DIST"
IMAGE=alia-engine-build:trixie
docker build -q -t $IMAGE tools/release > /dev/null
OPTS="-G Ninja -DCMAKE_BUILD_TYPE=Release -DALIA_BUILD_TESTS=OFF -DALIA_BUILD_APPS=OFF"
in_box() { docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" -v "$ALIA_SYSROOT:/sysroot:ro" -w /src $IMAGE sh -c "$1"; }
in_box "cmake -S . -B build-release/x86_64 $OPTS -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > /dev/null &&
        cmake --build build-release/x86_64 --target alia_engine > /dev/null &&
        cmake -S . -B build-release/aarch64 $OPTS -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-board.cmake -DALIA_SYSROOT=/sysroot > /dev/null &&
        cmake --build build-release/aarch64 --target alia_engine > /dev/null"

for arch in x86_64 aarch64; do
    name=alia-engine-$V-linux-$arch
    stage=build-release/stage/$name
    rm -rf "$stage" && mkdir -p "$stage/plugins"
    in_box "cmake --install build-release/$arch --prefix $stage > /dev/null"   # python/ (paths are the container's)
    [ $arch = x86_64 ] || cp build-release/stage/alia-engine-$V-linux-x86_64/python/alia_engine.pyi "$stage/python/"  # the same API
    for manifest in plugins/*/plugin.toml; do                                     # the first-party plugins
        dir=$(dirname "$manifest")
        pkg=$(sed -n 's/^package *= *"\([A-Za-z0-9_]*\)".*/\1/p' "$manifest")
        mkdir -p "$stage/plugins/$(basename "$dir")"
        cp "$manifest" "$stage/plugins/$(basename "$dir")/"
        cp -r "$dir/$pkg" "$stage/plugins/$(basename "$dir")/"
    done
    find "$stage" -name __pycache__ -prune -exec rm -rf {} +
    file "$stage/python/alia_engine.abi3.so" | grep -q "$( [ $arch = x86_64 ] && echo x86-64 || echo aarch64)"
    tar -czf "$DIST/$name.tar.gz" -C build-release/stage "$name"
done
in_box "cp -r /src /tmp/w && cd /tmp/w && rm -rf build && HOME=/tmp python3 -m pip wheel --no-deps --no-build-isolation -q -w /src/$DIST ."
(cd "$DIST" && sha256sum -- *.tar.gz *.whl > SHA256SUMS)
ls -la "$DIST"

if [ "${1:-}" = "--publish" ]; then
    git tag -l "v$V" | grep -q . || { echo "tag v$V first (git tag v$V && git push origin v$V)" >&2; exit 1; }
    gh release create "v$V" "$DIST"/* --title "alia-engine $V" --notes "Install: docs/getting-started.md. The engine modules are for Linux x86_64 (glibc 2.38 and SDL3 3.2 or newer) and the Alia Pad board (aarch64)."
fi
