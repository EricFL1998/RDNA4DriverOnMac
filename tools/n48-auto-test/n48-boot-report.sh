#!/bin/sh
# Install or retrieve persistent evidence. A transport failure is not a GPU
# result and must never trigger a second reboot in the deployment script.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT/tools/n48-auto-test/config.sh"
command=${1:-}

case $command in
    install)
        n48_scp "$ROOT/tools/n48-auto-test/n48-verify-remote.sh" "$N48_TARGET:/tmp/n48-verify.sh"
        n48_scp "$ROOT/tools/n48-auto-test/n48-boot-evidence.py" "$N48_TARGET:/tmp/n48-boot-evidence.py"
        n48_scp "$ROOT/tools/n48-auto-test/n48-boot-recorder.sh" "$N48_TARGET:/tmp/n48-boot-recorder.sh"
        n48_scp "$ROOT/tools/n48-auto-test/com.navi48.autotest.plist" "$N48_TARGET:/tmp/com.navi48.autotest.plist"
        n48_ssh 'sudo mkdir -p /var/db/navi48-auto-test/tools &&
            sudo chmod 700 /var/db/navi48-auto-test &&
            sudo install -m 755 /tmp/n48-verify.sh /var/db/navi48-auto-test/tools/verify.sh &&
            sudo install -m 755 /tmp/n48-boot-evidence.py /var/db/navi48-auto-test/tools/boot-evidence.py &&
            sudo install -m 755 /tmp/n48-boot-recorder.sh /var/db/navi48-auto-test/tools/boot-recorder.sh &&
            if sudo test -f /tmp/navi48test; then sudo install -m 755 /tmp/navi48test /var/db/navi48-auto-test/tools/navi48test; fi &&
            sudo cp /tmp/com.navi48.autotest.plist /Library/LaunchDaemons/com.navi48.autotest.plist &&
            sudo chown root:wheel /Library/LaunchDaemons/com.navi48.autotest.plist &&
            sudo chmod 644 /Library/LaunchDaemons/com.navi48.autotest.plist &&
            sudo chmod 755 /var/db/navi48-auto-test/tools &&
            sudo plutil -lint /Library/LaunchDaemons/com.navi48.autotest.plist'
        if ! n48_ssh 'sudo launchctl print system/com.navi48.autotest' >/dev/null 2>&1; then
            n48_ssh 'sudo launchctl bootstrap system /Library/LaunchDaemons/com.navi48.autotest.plist'
        fi
        n48_ssh 'sudo launchctl kickstart system/com.navi48.autotest'
        echo 'boot-recorder.sh installed; one-shot capture requested'
        ;;
    fetch)
        OUT=${2:?usage: n48-boot-report.sh fetch OUT BOOT_EPOCH}
        expected=${3:?usage: n48-boot-report.sh fetch OUT BOOT_EPOCH}
        case $expected in ''|*[!0-9]*) echo 'invalid boot epoch' >&2; exit 2 ;; esac
        mkdir -p "$OUT"
        chmod 700 "$OUT"
        remote="/var/db/navi48-auto-test/$expected"
        if ! remote_environment=$(n48_ssh "cat $remote/environment.txt"); then
            echo 'persistent boot report unavailable' >&2
            exit 3
        fi
        if ! printf '%s\n' "$remote_environment" | grep -q "^boot_epoch=$expected$"; then
            echo 'persistent boot report boot mismatch' >&2
            exit 4
        fi
        printf '%s\n' "$remote_environment" > "$OUT/environment.txt"
        if ! n48_ssh "test -f $remote/capture.json"; then
            echo 'persistent boot report is incomplete' >&2
            exit 5
        fi
        n48_ssh "cat $remote/capture.json" > "$OUT/capture.json"
        n48_scp -r "$N48_TARGET:$remote/." "$OUT/"
        n48_ssh 'sudo launchctl print system/com.navi48.autotest' > "$OUT/launchd-status.txt" 2>&1 || true
        python3 - "$OUT" "$expected" <<'PY'
import json
import pathlib
import sys
out = pathlib.Path(sys.argv[1])
capture = json.loads((out / 'capture.json').read_text())
if capture.get('complete') is not True:
    raise SystemExit('persistent capture is not complete')
if capture.get('boot_epoch') != int(sys.argv[2]):
    raise SystemExit('persistent capture boot mismatch')
result = {}
result_path = out / 'result.json'
if result_path.exists():
    result = json.loads(result_path.read_text())
(out / 'report-status.json').write_text(json.dumps({
    'boot_epoch': capture['boot_epoch'],
    'complete': True,
    'pass': result.get('pass') is True,
    'capture_exit_code': capture.get('capture_exit_code'),
    'interactive_metal_smoke_included': (out / 'metal-result.txt').exists(),
}, indent=2) + '\n')
PY
        cat "$OUT/result.txt"
        cat "$OUT/report-status.json"
        ;;
    *) echo 'usage: n48-boot-report.sh install | fetch OUT BOOT_EPOCH' >&2; exit 2 ;;
esac
