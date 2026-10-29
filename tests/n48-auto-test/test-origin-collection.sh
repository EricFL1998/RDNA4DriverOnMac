#!/bin/sh
# Exercise the verifier's real origin-copy block with synthetic evidence.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/n48-origin-collect.XXXXXX")
trap 'rm -rf "$W"' EXIT HUP INT TERM
mkdir -p "$W/private/tmp" "$W/out" "$W/bin"
printf 'N48_CA_TEST_DIR=%s\n' "$W" > "$W/settings"
. "$W/settings"
printf 'CA origin synthetic-iosurface surf=0x1234\n' > "$W/private/tmp/n48-ca-origin-88.log"
printf 'CA push-missing synthetic\n' > "$W/private/tmp/n48-ca-push-88.log"
printf 'CA context seq=0 kind=failed context=0x0\n' > "$W/private/tmp/n48-ca-context-88.log"
chmod 600 "$W/private/tmp/n48-ca-origin-88.log" "$W/private/tmp/n48-ca-push-88.log" "$W/private/tmp/n48-ca-context-88.log"
origin_before=$(shasum -a 256 "$W/private/tmp/n48-ca-origin-88.log")
push_before=$(shasum -a 256 "$W/private/tmp/n48-ca-push-88.log")
context_before=$(shasum -a 256 "$W/private/tmp/n48-ca-context-88.log")

python3 - "$ROOT/tools/n48-auto-test/n48-verify-remote.sh" "$W/copy.sh" <<'PY'
import pathlib
import sys
text = pathlib.Path(sys.argv[1]).read_text()
start = text.index('# The probe is root-owned by WindowServer.')
end = text.index('mkdir -p "$OUT/reports"', start)
pathlib.Path(sys.argv[2]).write_text('set -eu\n' + text[start:end])
PY

# Root execution does not need sudo; retain that exact verifier branch.
# The target allows passwordless sudo; this fixture uses the same read-only
# commands against local synthetic evidence, without changing host privileges.
cat > "$W/bin/sudo" <<'EOF'
#!/bin/sh
exec "$@"
EOF
chmod +x "$W/bin/sudo"
PATH="$W/bin:$PATH" N48_CA_REMOTE_DIR="$N48_CA_TEST_DIR/private/tmp" OUT="$W/out" sh "$W/copy.sh"
cmp "$W/private/tmp/n48-ca-origin-88.log" "$W/out/ca-origin.log"
cmp "$W/private/tmp/n48-ca-push-88.log" "$W/out/ca-push.log"
cmp "$W/private/tmp/n48-ca-context-88.log" "$W/out/ca-context.log"
[ "$(shasum -a 256 "$W/private/tmp/n48-ca-origin-88.log")" = "$origin_before" ]
[ "$(shasum -a 256 "$W/private/tmp/n48-ca-push-88.log")" = "$push_before" ]
[ "$(shasum -a 256 "$W/private/tmp/n48-ca-context-88.log")" = "$context_before" ]
echo 'PASS: CA origin, context, and push logs collected without modifying sources'

rm "$W/out/ca-origin.log"
ln -s "$N48_CA_TEST_DIR/private/tmp/n48-ca-origin-88.log" "$W/private/tmp/n48-ca-origin-link.log"
PATH="$W/bin:$PATH" N48_CA_REMOTE_DIR="$N48_CA_TEST_DIR/private/tmp" OUT="$W/out" sh "$W/copy.sh"
[ -f "$W/out/ca-origin.log" ]
echo 'PASS: original origin file still collected in presence of unrelated link'

for mode in missing oversized empty; do
    rm -rf "$W/out"
    mkdir -p "$W/out"
    rm -f "$W/private/tmp/n48-ca-origin-88.log"
    case $mode in
        missing) ;;
        oversized) python3 - "$W/private/tmp/n48-ca-origin-88.log" <<'PY'
import pathlib,sys
pathlib.Path(sys.argv[1]).write_bytes(b'x' * 65537)
PY
            chmod 600 "$W/private/tmp/n48-ca-origin-88.log" ;;
        empty) : > "$W/private/tmp/n48-ca-origin-88.log"; chmod 600 "$W/private/tmp/n48-ca-origin-88.log" ;;
    esac
    rm -f "$W/private/tmp/n48-ca-push-88.log"
    rm -f "$W/private/tmp/n48-ca-context-88.log"
    if [ "$mode" = empty ]; then
        : > "$W/private/tmp/n48-ca-push-88.log"
        chmod 600 "$W/private/tmp/n48-ca-push-88.log"
    fi
    PATH="$W/bin:$PATH" N48_CA_REMOTE_DIR="$N48_CA_TEST_DIR/private/tmp" OUT="$W/out" sh "$W/copy.sh"
    if [ -f "$W/out/ca-origin-collected.txt" ]; then
        [ -f "$W/out/ca-origin.log" ] || { echo 'collected marker without origin file'; exit 1; }
    fi
    python3 - "$W/out" "$mode" <<'PY'
import pathlib,sys
out=pathlib.Path(sys.argv[1]); mode=sys.argv[2]
origin=(out/'ca-origin.log').read_bytes() if (out/'ca-origin.log').exists() else b''
push=(out/'ca-push.log').read_bytes() if (out/'ca-push.log').exists() else b''
if mode=='missing':
    assert not origin, 'missing origin must not be presented as collected evidence'
    assert not push, 'no push event is an empty log, not evidence of a crash'
    assert (out/'ca-push-collected.txt').exists(), 'zero push events is an explicit collected result'
    assert not (out/'ca-origin-collected.txt').exists(), 'missing origin remains a collection error'
elif mode=='oversized':
    assert (out/'ca-origin-error.txt').exists(), 'over-limit evidence must have an explicit error'
    assert not (out/'ca-origin-collected.txt').exists(), 'over-limit must not be accepted'
elif mode=='empty':
    assert not origin, 'empty fixture must remain empty'
    assert (out/'ca-origin-collected.txt').exists(), 'empty file is collected but has no registrations'
    assert not origin, 'collected empty origin contains no tracked surface evidence'
PY
done
echo 'PASS: missing, oversized, and empty CA evidence have explicit fixtures'
