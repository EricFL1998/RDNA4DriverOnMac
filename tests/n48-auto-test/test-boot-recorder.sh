#!/bin/sh
# Run the real recorder against an isolated filesystem and verifier.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RECORDER="$ROOT/tools/n48-auto-test/n48-boot-recorder.sh"
[ -f "$RECORDER" ] || { echo "FAIL: boot recorder is missing" >&2; exit 1; }
W=$(mktemp -d "${TMPDIR:-/tmp}/n48-boot-recorder.XXXXXX")
trap 'rm -rf "$W"' EXIT HUP INT TERM
mkdir -p "$W/bin" "$W/reports"
cat > "$W/bin/verify.sh" <<'EOF'
#!/bin/sh
set -u
printf '%s\n' "$N48_RECORDER_TEST_MARKER" > "$1/fixture-marker.txt"
printf 'boot_epoch=%s\n' "$N48_RECORDER_TEST_BOOT" > "$1/environment.txt"
printf 'RESULT %s\n' "$N48_RECORDER_TEST_MARKER" > "$1/result.txt"
printf 'pid=%s metal_smoke=%s\n' "$$" "${N48_METAL_SMOKE:-unset}" >> "$N48_RECORDER_TEST_INVOCATIONS"
exit "${N48_RECORDER_TEST_EXIT:-0}"
EOF
chmod +x "$W/bin/verify.sh"
printf 'boot-evidence fixture\n' > "$W/bin/boot-evidence.py"

run_case() {
    epoch=$1
    marker=$2
    code=$3
    set +e
    N48_RECORDER_ROOT="$W/reports" \
    N48_RECORDER_BIN="$W/bin" \
    N48_BOOT_EPOCH="$epoch" \
    N48_RECORDER_TEST_MARKER="$marker" \
    N48_RECORDER_TEST_BOOT="$epoch" \
    N48_RECORDER_TEST_EXIT="$code" \
    N48_RECORDER_TEST_INVOCATIONS="$W/invocations" \
        sh "$RECORDER" > "$W/recorder-$epoch.log" 2>&1
    actual=$?
    set -e
    [ "$actual" -eq "$code" ] || {
        cat "$W/recorder-$epoch.log" >&2
        echo "FAIL: recorder changed verifier exit $code to $actual" >&2
        exit 1
    }
    [ "$(cat "$W/reports/$epoch/fixture-marker.txt")" = "$marker" ]
    [ "$(cat "$W/reports/$epoch/capture-exit-code.txt")" = "$code" ]
    [ "$(cat "$W/reports/$epoch/environment.txt")" = "boot_epoch=$epoch" ]
    grep -q '"complete": true' "$W/reports/$epoch/capture.json"
    grep -q '"boot_epoch": '"$epoch" "$W/reports/$epoch/capture.json"
}

run_case 1793056000 PASS 0
run_case 1793057000 FAIL 1
[ "$(wc -l < "$W/invocations" | tr -d ' ')" = 2 ]
grep -q 'metal_smoke=0' "$W/invocations"
[ "$(find "$W/reports" -name capture.json | wc -l | tr -d ' ')" = 2 ]
if pgrep -f "$W/bin/verify.sh" > /dev/null; then
    echo 'FAIL: recorder left a verifier running' >&2
    exit 1
fi

# Pruning is only about old captures. It must never remove the active boot.
for epoch in 1 2 3 4; do
    mkdir -p "$W/reports/$epoch"
    printf '{"complete": true}\n' > "$W/reports/$epoch/capture.json"
done
N48_RECORDER_ROOT="$W/reports" N48_RECORDER_BIN="$W/bin" \
N48_BOOT_EPOCH=1793058000 N48_RECORDER_TEST_MARKER=last \
N48_RECORDER_TEST_BOOT=1793058000 N48_RECORDER_TEST_EXIT=0 \
N48_RECORDER_TEST_INVOCATIONS="$W/invocations" N48_REPORT_LIMIT=3 \
    sh "$RECORDER" >> "$W/recorder-last.log" 2>&1
[ -f "$W/reports/1793058000/capture.json" ]
[ "$(find "$W/reports" -name capture.json | wc -l | tr -d ' ')" = 3 ]

# The recorder's production default resolves tools beside the persistent
# report root, so it remains runnable after /tmp is cleared at reboot.
mkdir -p "$W/persistent/tools"
cp "$W/bin/verify.sh" "$W/persistent/tools/verify.sh"
N48_RECORDER_ROOT="$W/persistent" N48_BOOT_EPOCH=1793059000 \
N48_RECORDER_TEST_MARKER=persistent N48_RECORDER_TEST_BOOT=1793059000 \
N48_RECORDER_TEST_EXIT=0 N48_RECORDER_TEST_INVOCATIONS="$W/invocations" \
    sh "$RECORDER" > "$W/recorder-persistent.log" 2>&1
[ "$(cat "$W/persistent/1793059000/fixture-marker.txt")" = persistent ]

python3 - "$ROOT/tools/n48-auto-test/com.navi48.autotest.plist" "$RECORDER" <<'PY'
import pathlib
import plistlib
import sys
plist = plistlib.loads(pathlib.Path(sys.argv[1]).read_bytes())
assert plist['Label'] == 'com.navi48.autotest'
assert plist['RunAtLoad'] is True
assert plist.get('KeepAlive') is not True, 'a crash must not repeatedly rerun the recorder'
assert 'StartInterval' not in plist
assert plist['ProgramArguments'][0] == '/bin/sh'
assert plist['ProgramArguments'][1] == '/var/db/navi48-auto-test/tools/boot-recorder.sh'
source = pathlib.Path(sys.argv[2]).read_text()
for forbidden in ('killall', '/sbin/reboot', 'shutdown -r'):
    assert forbidden not in source, 'boot evidence must not restart the GUI or computer'
print('PASS: one-shot LaunchDaemon and no GUI restart')
PY
echo 'PASS: persistent per-boot capture, failure preservation, and retention'
