#!/bin/sh
# Execute the same pixel-correct Metal smoke suite on the target, as root.
# These tools use N48M_ALLOW and the bringup route, not the app allow-list.
set -u

OUT=${1:-/tmp/n48-auto-test/metal}
PROBE=${N48_MTLPROBE:-/tmp/mtlprobe}
EXPECTED=${N48_EXPECTED:-/tmp/expected.raw}
mkdir -p "$OUT"
chmod 700 "$OUT"

if [ ! -x "$PROBE" ]; then
    printf 'SKIP metal probe unavailable: %s\n' "$PROBE"
    exit 2
fi

metal_run() {
    label=$1
    shift
    printf 'RUN %s: %s\n' "$label" "$*" | tee "$OUT/$label.cmd"
    # root's login zsh setup affects `sudo ... arch ... env` on this target;
    # use the proven direct form instead of nesting those wrappers.
    sudo -u root /usr/bin/env N48M_ALLOW=1 "$PROBE" "$label" "$@" \
        > "$OUT/$label.log" 2>&1
    rc=$?
    printf '%s\n' "$rc" > "$OUT/$label.exit"
    printf '%s exit=%s\n' "$label" "$rc"
    /usr/bin/grep -E 'PASS|FAIL|NSError|differ|mismatch|status=' "$OUT/$label.log" | /usr/bin/tail -12 || true
    return "$rc"
}

result=0
metal_run list || result=1
metal_run clear "$OUT/clear.png" || result=1
metal_run quad "$OUT/quad.png" || result=1
if [ -f "$EXPECTED" ]; then
    metal_run triangle "$OUT/triangle.png" || result=1
    metal_run frames 24 --inflight 3 || result=1
else
    printf 'SKIP triangle and frames golden missing: %s\n' "$EXPECTED"
    result=1
fi
metal_run compute "$OUT/compute.png" || result=1
metal_run iosurface "$OUT/iosurface.png" || result=1
metal_run depth || result=1
metal_run msaa "$OUT/msaa.png" || result=1
printf 'METAL_SMOKE %s\n' "$([ "$result" -eq 0 ] && echo PASS || echo FAIL)" | tee "$OUT/result.txt"
exit "$result"
