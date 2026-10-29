#!/bin/sh
# The production family table is deliberately not a blanket Metal3 claim.
# This checks the source used in the bundle, not only n48g_supports_family().
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
D="$ROOT/navi48/Cobalt64-main/tools/native/navi48metal"
cc -O1 -Wall -Wextra -Werror -I"$D" \
    -o "${TMPDIR:-/tmp}/n48-family-test" "$D/test-gate.c"
"${TMPDIR:-/tmp}/n48-family-test" "$D"
