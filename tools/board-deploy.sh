#!/bin/bash
# Run the engine's tests on an Alia Pad board: push the cross-built test build (build-board/, docs/architecture.md
# §5.1) and the test data over adb, run every test there, print a summary. Exit 1 if any failed.
#   tools/board-deploy.sh [TARGET_DIR] [ADB_SERIAL]
# TARGET_DIR on the board defaults to ~/alia-engine (the adb user's home); ADB_SERIAL picks the device when more
# than one is connected. Everything runs headless (EGL surfaceless): nothing takes the screen.
set -euo pipefail
cd "$(dirname "$0")/.."
D=${1:-'$HOME/alia-engine'}
ADB=(adb)
[ -n "${2:-}" ] && ADB=(adb -s "$2")
[ -x build-board/test_render ] || { echo "no board build: cmake --build build-board (docs/architecture.md §5.1)" >&2; exit 1; }

D=$("${ADB[@]}" shell "echo $D" | tr -d '\r')
"${ADB[@]}" shell "mkdir -p $D/build-board $D/tests"
push() { "${ADB[@]}" push --sync "$@" > /dev/null 2>&1; }  # exits non-zero (set -e) if a push fails
push build-board/alia-bench build-board/test_* build-board/alia_engine.abi3.so build-board/alia_kit.py \
    build-board/alia_pack.py "$D/build-board/"
for pkg in plugins/*/alia_*/; do push "$pkg" "$D/build-board/"; done
push tests/golden tests/test_python.py tests/test_kit.py plugins/*/tests/test_*.py "$D/tests/"

# The C++ tests (the image ones compare with tests/golden), then the Python ones against the board's module.
"${ADB[@]}" shell "cd $D && fails=0
for t in build-board/test_*; do
  if ALIA_GOLDEN_DIR=\$PWD/tests/golden \$t > /tmp/alia-\${t##*/}.log 2>&1; then r=ok; else r=FAIL; fails=\$((fails + 1)); fi
  echo \"\$r \${t##*/} (\$(grep -c '^ok' /tmp/alia-\${t##*/}.log) checks)\"
done
for t in tests/test_python.py tests/test_kit.py tests/test_tween.py; do
  if (cd / && PYTHONPATH=$D/build-board python3 $D/\$t > /tmp/alia-\${t##*/}.log 2>&1); then r=ok; else r=FAIL; fails=\$((fails + 1)); fi
  echo \"\$r \${t##*/} (\$(grep -c '^ok' /tmp/alia-\${t##*/}.log) checks)\"
done
grep -h '^FAIL' /tmp/alia-test_*.log | head -20
echo \"board: \$fails failed (logs in /tmp/alia-*.log on the board)\"
[ \$fails -eq 0 ]"
