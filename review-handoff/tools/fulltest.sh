#!/bin/bash
# Usage: fulltest.sh <source-dir> <name> [Debug|Release] [make-target...]
# Configures (once) and builds the real project in $S/b-<name>-<cfg> with the stub SQLite + FTXUI
# overrides, then runs inventatory_tests and inventatory_input_tests inside a private D-Bus
# session with an unlocked keyring and the SQLite version shim. Prints a short summary.
S=/tmp/claude-0/-home-user-Inventatory-Software/12d62aab-f424-56dd-b06c-ff8666ba88ef/scratchpad
SRC=$(readlink -f "$1"); NAME=$2; CFG=${3:-Debug}
B=$S/b-$NAME-$CFG
[ -f $B/CMakeCache.txt ] || cmake -S "$SRC" -B $B -DCMAKE_BUILD_TYPE=$CFG \
  -DFETCHCONTENT_SOURCE_DIR_FTXUI=$S/ftxui -DFETCHCONTENT_SOURCE_DIR_SQLITE3_AMALGAMATION=$S/fakesqlite \
  -DCMAKE_CXX_STANDARD_LIBRARIES=-lsqlite3 > $B.cfg.log 2>&1 || { echo "CONFIG FAILED, see $B.cfg.log"; exit 2; }
cmake --build $B --parallel 3 --target inventatory inventatory_tests inventatory_input_tests > $B.build.log 2>&1 \
  || { echo "BUILD FAILED, errors:"; grep -E "error|Error" $B.build.log | head -20; exit 3; }
echo "BUILD OK ($CFG)"
export XDG_DATA_HOME=$S/xdgdata-$NAME; mkdir -p $XDG_DATA_HOME
for try in 1 2; do
  out=$(printf '%s' 'ci-keyring-password' | INVENTATORY_REQUIRE_LOCALE_TESTS=1 dbus-run-session -- bash -c "
    eval \"\$(gnome-keyring-daemon --unlock --components=secrets)\"
    cd $B && LD_PRELOAD=$S/shim.so ./inventatory_tests 2>&1 | tail -5; echo core_rc=\${PIPESTATUS[0]}
    LD_PRELOAD=$S/shim.so ./inventatory_input_tests; echo input_rc=\$?" 2>&1 | grep -v "fd limit")
  echo "$out" | grep -q "core_rc=0" && break
  [ $try = 1 ] && echo "(first run failed; retrying once, the keyring step can flake right after a rebuild)"
done
echo "$out"
echo "$out" | grep -q "core_rc=0" && echo "$out" | grep -q "input_rc=0" && echo "TESTS PASSED ($CFG)" || { echo "TESTS FAILED ($CFG)"; exit 4; }
