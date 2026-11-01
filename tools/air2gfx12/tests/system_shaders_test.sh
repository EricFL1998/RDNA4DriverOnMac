#!/bin/sh
# Compile every shader the macOS compositor uses and require every symbol to
# resolve. This is the real shader surface of the endgame: CoreDisplay's final
# display pass and SkyLight's WindowServer compositor. A shader that still
# relocates air.sample_texture_* or __il_* would jump through an unbound GOT at
# run time, because the AIRNT loader copies only .text.
#
# Expected function counts are from macOS 26/27. A different count is reported
# as a NOTE (the OS build changed the set), not a failure; unresolved symbols
# are the failure.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
AIR2GFX12=${AIR2GFX12:-$ROOT/bin/air2gfx12}
COREDISPLAY=${COREDISPLAY_METALLIB:-/System/Library/Frameworks/CoreDisplay.framework/Versions/A/Resources/default.metallib}
SKYLIGHT=${SKYLIGHT_METALLIB:-/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/Resources/SkyLightShaders.air64.metallib}

TMP=${TMPDIR:-/tmp}/n48-system-shaders.$$
mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

if [ ! -x "$AIR2GFX12" ]; then
    printf 'FAIL compiler missing: %s\n' "$AIR2GFX12"
    exit 1
fi

total=0
pass=0
fail=0

run_library() {
    library=$1
    label=$2
    expected=$3

    if [ ! -f "$library" ]; then
        printf 'SKIP %s: %s not present\n' "$label" "$library"
        return 0
    fi

    names=$("$AIR2GFX12" "$library" --list 2>/dev/null |
        sed -n 's/^  \[[0-9]*\] \(.*\) (.*/\1/p')
    count=$(printf '%s\n' "$names" | grep -c .)
    printf '%s: %s shader(s)\n' "$label" "$count"
    if [ "$count" -ne "$expected" ]; then
        printf '  NOTE expected %s; the OS build changed the shader set\n' "$expected"
    fi

    for name in $names; do
        total=$((total + 1))
        log="$TMP/$label-$name.log"
        if "$AIR2GFX12" "$library" --kernel "$name" \
            --emit-obj "$TMP/$label-$name.o" --require-resolved >"$log" 2>&1; then
            pass=$((pass + 1))
            printf '  PASS %s\n' "$name"
        else
            fail=$((fail + 1))
            printf '  FAIL %s\n' "$name"
            sed -n 's/^  /    /p' "$log" | tail -20
        fi
    done
}

run_library "$COREDISPLAY" CoreDisplay 4
run_library "$SKYLIGHT" SkyLight 33

printf '\n%d/%d shader(s) fully resolved, %d failed\n' "$pass" "$total" "$fail"
[ "$fail" -eq 0 ]
