#!/bin/bash
# Build parent.com / child.com and run parent.com under kvikdos.
#
# Expected outcomes:
#
#   POST-FIX (in-place spawn implemented):
#     parent: in-place spawn OK       <- exit 0
#     child: parent sentinel found    <- printed before that
#
#   PRE-FIX (current kvikdos behaviour, snapshot+reset_emu):
#     child: parent sentinel MISSING (memory wiped?)
#     parent: child exit code != 0
#     <- exit 3
#
# Run from the kvikdos repo root.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TESTDIR="$REPO_ROOT/tests/inplace_spawn"

cd "$REPO_ROOT"

# Build kvikdos if needed.
if [[ ! -x kvikdos && ! -x kvikdos-soft ]]; then
    echo "==> building kvikdos"
    make kvikdos kvikdos-soft 2>&1 | tail -3
fi

# Pick whichever binary is available.
if [[ -x kvikdos ]]; then
    KVIKDOS_BIN=./kvikdos
else
    KVIKDOS_BIN=./kvikdos-soft
fi

echo "==> assembling fixtures"
nasm -O0 -f bin -o "$TESTDIR/PARENT.COM"   "$TESTDIR/parent.nasm"
nasm -O0 -f bin -o "$TESTDIR/CHILD.COM"    "$TESTDIR/child.nasm"
nasm -O0 -f bin -o "$TESTDIR/PARENT_E.COM" "$TESTDIR/parent_exe.nasm"
nasm -O0 -f bin -o "$TESTDIR/CHILD_EX.EXE" "$TESTDIR/child_exe.nasm"

# kvikdos resolves DOS-relative filenames against the CWD of the host
# process, so cd into TESTDIR before launching. Use absolute path for
# the kvikdos binary.
KVIKDOS_ABS="$REPO_ROOT/${KVIKDOS_BIN#./}"
fail=0

run_one() {
    local label="$1" prog="$2" want="$3"
    echo "==> [$label] running $prog"
    local out rc=0
    out=$(cd "$TESTDIR" && "$KVIKDOS_ABS" "$prog" 2>&1) || rc=$?
    echo "$out"
    echo "exit=$rc"
    if grep -q "$want" <<<"$out"; then
        echo "  PASS"
    else
        echo "  FAIL"
        fail=1
    fi
    echo
}

run_one "COM child"  PARENT.COM   'parent: in-place spawn OK'
run_one "EXE child"  PARENT_E.COM 'parent_exe: in-place EXE spawn OK'

if [[ $fail -eq 0 ]]; then
    echo "RESULT: PASS — both spawn variants working"
    exit 0
else
    echo "RESULT: FAIL"
    exit 1
fi
