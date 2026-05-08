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
nasm -O0 -f bin -o "$TESTDIR/PARENT.COM" "$TESTDIR/parent.nasm"
nasm -O0 -f bin -o "$TESTDIR/CHILD.COM"  "$TESTDIR/child.nasm"

echo "==> running parent.com under $KVIKDOS_BIN"
echo
# kvikdos resolves DOS-relative filenames against the CWD of the host
# process, so cd into TESTDIR before launching. Use absolute path for
# the kvikdos binary.
out=$(cd "$TESTDIR" && "$REPO_ROOT/${KVIKDOS_BIN#./}" PARENT.COM 2>&1) || rc=$?
rc=${rc:-0}
echo "$out"
echo
echo "exit=$rc"

if grep -q 'parent: in-place spawn OK' <<<"$out"; then
    echo "RESULT: PASS — in-place spawn working"
    exit 0
else
    echo "RESULT: FAIL (expected, until in-place spawn lands)"
    exit 1
fi
