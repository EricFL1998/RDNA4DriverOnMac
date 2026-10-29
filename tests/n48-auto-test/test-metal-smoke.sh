#!/bin/sh
# Verify the real smoke runner invokes the intended probe and propagates
# every result, rather than dropping the command name during privilege changes.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/n48-smoke-fixture.XXXXXX")
trap 'rm -rf "$W"' EXIT HUP INT TERM
mkdir -p "$W/bin" "$W/out"

cat > "$W/bin/sudo" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$N48_SMOKE_CALLS"
[ "$1" = '-u' ] && [ "$2" = root ] || exit 64
shift 2
exec "$@"
EOF
cat > "$W/bin/probe" <<'EOF'
#!/bin/sh
if [ "${N48M_ALLOW:-}" != 1 ]; then
    echo 'N48M_ALLOW missing' >&2
    exit 65
fi
printf '%s\n' "$*" >> "$N48_SMOKE_ARGS"
[ "$1" != fail ] || exit 1
printf 'mtlprobe: PASS %s\n' "$1"
EOF
chmod +x "$W/bin/sudo" "$W/bin/probe"
printf 'golden' > "$W/expected.raw"

PATH="$W/bin:$PATH" N48_SMOKE_CALLS="$W/calls" N48_SMOKE_ARGS="$W/args" \
N48_MTLPROBE="$W/bin/probe" N48_EXPECTED="$W/expected.raw" \
    sh "$ROOT/tools/n48-auto-test/n48-metal-smoke.sh" "$W/out" > "$W/result.log"

grep -q '^METAL_SMOKE PASS$' "$W/out/result.txt"
for command in list clear quad triangle frames compute iosurface depth msaa; do
    if [ "$command" = list ] || [ "$command" = depth ]; then
        grep -q "^$command\$" "$W/args" || {
            printf 'missing probe command: %s\n' "$command" >&2
            exit 1
        }
        continue
    fi
    grep -q "^$command " "$W/args" || {
        printf 'missing probe command: %s\n' "$command" >&2
        exit 1
    }
done
grep -q 'triangle .*/triangle.png' "$W/args"
grep -q 'frames 24 --inflight 3' "$W/args"
grep -q 'compute .*/compute.png' "$W/args"
grep -q 'iosurface .*/iosurface.png' "$W/args"
grep -q 'msaa .*/msaa.png' "$W/args"
echo 'PASS: real smoke runner forwards commands, golden paths, and status'

cat > "$W/bin/probe" <<'EOF'
#!/bin/sh
[ "$1" = clear ] && exit 1
printf 'mtlprobe: PASS %s\n' "$1"
EOF
chmod +x "$W/bin/probe"
if PATH="$W/bin:$PATH" N48_SMOKE_CALLS="$W/calls2" N48_SMOKE_ARGS="$W/args2" \
N48_MTLPROBE="$W/bin/probe" N48_EXPECTED="$W/expected.raw" \
    sh "$ROOT/tools/n48-auto-test/n48-metal-smoke.sh" "$W/out2"; then
    echo 'FAIL smoke hides a failed clear' >&2
    exit 1
fi
grep -q '^METAL_SMOKE FAIL$' "$W/out2/result.txt"
echo 'PASS: a real probe failure fails the suite'
