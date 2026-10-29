#!/bin/sh
# Test the real installation/retrieval helper with only the network replaced.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
HELPER="$ROOT/tools/n48-auto-test/n48-boot-report.sh"
[ -f "$HELPER" ] || { echo 'FAIL: boot report helper is missing' >&2; exit 1; }
W=$(mktemp -d "${TMPDIR:-/tmp}/n48-auto-deploy.XXXXXX")
trap 'rm -rf "$W"' EXIT HUP INT TERM
mkdir -p "$W/remote/1793060000/reports" "$W/local" "$W/bin"
cat > "$W/remote/1793060000/capture.json" <<'EOF'
{"complete": true, "boot_epoch": 1793060000, "capture_exit_code": 1}
EOF
printf 'boot_epoch=1793060000\n' > "$W/remote/1793060000/environment.txt"
printf 'RESULT FAIL\n' > "$W/remote/1793060000/result.txt"
printf 'WindowServer synthetic crash\n' > "$W/remote/1793060000/reports/WindowServer.ips"

# SSH and SCP are the external boundary. Everything after the helper is
# called is the real helper logic, not a replacement for its policy.
cat > "$W/bin/ssh" <<'EOF'
#!/bin/sh
printf 'ssh %s\n' "$*" >> "$N48_TEST_TRACE"
remote=${N48_TEST_REMOTE:?}
last=
for arg in "$@"; do last=$arg; done
case $last in
    *1793060001/environment.txt)
        printf 'boot_epoch=1793060000\n' ;;
    'cat /var/db/navi48-auto-test/1793060000/environment.txt')
        cat "$remote/1793060000/environment.txt" ;;
    'test -f /var/db/navi48-auto-test/1793060000/capture.json')
        test -f "$remote/1793060000/capture.json" ;;
    'cat /var/db/navi48-auto-test/1793060000/capture.json')
        cat "$remote/1793060000/capture.json" ;;
    'sudo launchctl print system/com.navi48.autotest')
        printf 'state = not running\nlast exit code = 1\n' ;;
    'sudo /var/db/navi48-auto-test/tools/boot-recorder.sh')
        exit "${N48_TEST_RECORDER_EXIT:-0}" ;;
    'sudo plutil -lint /Library/LaunchDaemons/com.navi48.autotest.plist')
        printf 'plist OK\n' ;;
    'sudo '*)
        printf 'install %s\n' "$last" >> "$N48_TEST_TRACE"
        exit 0 ;;
    *) exit 1 ;;
esac
EOF
cat > "$W/bin/scp" <<'EOF'
#!/bin/sh
printf 'scp %s\n' "$*" >> "$N48_TEST_TRACE"
remote=${N48_TEST_REMOTE:?}
for arg in "$@"; do
    case $arg in
        *:/var/db/navi48-auto-test/*)
            src=$remote/${arg#*:/var/db/navi48-auto-test/}
            shift
            while [ "$#" -gt 1 ]; do shift; done
            cp -R "$src/." "$1/"
            exit 0 ;;
        *:/tmp/*) exit 0 ;;
    esac
done
exit 1
EOF
chmod +x "$W/bin/ssh" "$W/bin/scp"
export N48_TEST_TRACE="$W/trace" N48_TEST_REMOTE="$W/remote"
export PATH="$W/bin:$PATH"

sh "$HELPER" install > "$W/install.log"
grep -q 'boot-recorder.sh' "$W/trace"
grep -q 'com.navi48.autotest.plist' "$W/trace"
grep -q 'boot-recorder.sh' "$W/install.log"
if grep -E 'reboot|killall' "$W/trace"; then
    echo 'FAIL: recorder installation restarted the test machine' >&2
    exit 1
fi

sh "$HELPER" fetch "$W/local" 1793060000
cmp "$W/local/environment.txt" "$W/remote/1793060000/environment.txt"
cmp "$W/local/result.txt" "$W/remote/1793060000/result.txt"
cmp "$W/local/reports/WindowServer.ips" "$W/remote/1793060000/reports/WindowServer.ips"
grep -q '"pass": false' "$W/local/report-status.json"
grep -q '"complete": true' "$W/local/report-status.json"
grep -q 'last exit code = 1' "$W/local/launchd-status.txt"

mkdir "$W/wrong-boot"
if sh "$HELPER" fetch "$W/wrong-boot" 1793060001 > "$W/wrong.log" 2>&1; then
    echo 'FAIL: a different boot was accepted as current evidence' >&2
    exit 1
fi
[ ! -f "$W/wrong-boot/environment.txt" ]
[ ! -f "$W/wrong-boot/report-status.json" ]
grep -q 'boot mismatch' "$W/wrong.log"

mkdir "$W/missing"
cat > "$W/bin/ssh" <<'EOF'
#!/bin/sh
exit 255
EOF
if sh "$HELPER" fetch "$W/missing" 1793060000 > "$W/missing.log" 2>&1; then
    echo 'FAIL: SSH failure became a successful capture' >&2
    exit 1
fi
[ ! -f "$W/missing/report-status.json" ]
grep -q 'unavailable' "$W/missing.log"

# Replace only ssh/scp and the wait between boot attempts. The deploy and
# collection scripts themselves remain the production scripts under test.
cat > "$W/bin/ssh" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$N48_TEST_TRACE"
printf '%s\n' "$*" >> "$N48_TEST_TRACE.all"
while [ "$#" -gt 0 ] && [ "$1" != test@target ]; do shift; done
[ "$#" -gt 0 ] && shift
last=$*
case $last in
    'sysctl -n kern.boottime')
        if [ -f "$N48_TEST_TRACE.rebooted" ]; then
            printf '{ sec = 1793060000, usec = 0 }\n'
        else
            printf '{ sec = 1793050000, usec = 0 }\n'
        fi ;;
    'sudo /sbin/reboot') : > "$N48_TEST_TRACE.rebooted" ;;
    *autologin*) printf 'Automatic login user: louxuanchen\n' ;;
    'shasum -a 256 /Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS/Navi48Metal')
        tar -xOf "$N48_TEST_ROOT/navi48/Cobalt64-main/tools/native/navi48metal/build/Navi48Metal.bundle.tar" Navi48Metal.bundle/Contents/MacOS/Navi48Metal | shasum -a 256 ;;
    'cat /var/db/navi48-auto-test/1793060000/environment.txt')
        cat "$N48_TEST_REMOTE/1793060000/environment.txt" ;;
    'test -f /var/db/navi48-auto-test/1793060000/capture.json')
        test -f "$N48_TEST_REMOTE/1793060000/capture.json" ;;
    'cat /var/db/navi48-auto-test/1793060000/capture.json')
        cat "$N48_TEST_REMOTE/1793060000/capture.json" ;;
    'sudo launchctl print system/com.navi48.autotest') printf 'last exit code = 1\n' ;;
    *) exit 0 ;;
esac
EOF
cat > "$W/bin/scp" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$N48_TEST_TRACE"
printf '%s\n' "$*" >> "$N48_TEST_TRACE.all"
for arg in "$@"; do
    case $arg in *:/var/db/navi48-auto-test/*)
        src=$N48_TEST_REMOTE/${arg#*:/var/db/navi48-auto-test/}
        for dest in "$@"; do :; done
        cp -R "$src/." "$dest/"
        exit 0 ;;
    esac
done
exit 0
EOF
cat > "$W/bin/sleep" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$W/bin/sleep"
N48_TEST_ROOT="$ROOT" N48_SSH_KEY="$W/test-key" N48_DIAGNOSTIC_DIR="$W/diagnostics" \
N48_TARGET=test@target N48_TEST_REMOTE="$W/remote" N48_TEST_TRACE="$W/deploy-trace" \
    PATH="$W/bin:$PATH" bash "$ROOT/deploy-optimized-driver.sh" --reboot > "$W/deploy.log" 2>&1
python3 - "$W/deploy-trace" <<'PY'
import pathlib
import sys
trace = pathlib.Path(sys.argv[1]).read_text()
for tool in ('/var/db/navi48-auto-test/tools/verify.sh',
             '/var/db/navi48-auto-test/tools/boot-evidence.py',
             '/var/db/navi48-auto-test/tools/boot-recorder.sh',
             '/var/db/navi48-auto-test/tools/navi48test'):
    assert tool in trace, f'missing persistent installation: {tool}'
assert 'com.navi48.autotest.plist' in trace
assert trace.count('/sbin/reboot') == 1, 'one test must cause one reboot'
assert trace.index('/sbin/reboot') < trace.rindex('cat /var/db/navi48-auto-test/1793060000/environment.txt')
assert trace.index('/var/db/navi48-auto-test/tools/boot-recorder.sh') < trace.index('/sbin/reboot')
assert 'sysadminctl -autologin status' in trace
print('PASS: deployment installs recorder before one reboot and exact-boot fetch')
PY
echo 'PASS: exact-boot evidence, SSH failure handling, and crash preservation'
