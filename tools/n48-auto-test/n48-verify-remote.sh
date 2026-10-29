#!/bin/sh
# Read-only checks for the macOS under test. A failure is a test result,
# never a reason to kill WindowServer, log out, or change driver settings.
set -u

TOOLS=${N48_TOOLS_DIR:-/var/db/navi48-auto-test/tools}
OUT=${1:-/var/db/navi48-auto-test/manual-latest}
settle=${N48_SETTLE_SECONDS:-30}
boot_epoch=$(sysctl -n kern.boottime | sed -n 's/.*{ sec = \([0-9][0-9]*\), usec.*/\1/p')
boot_epoch=${boot_epoch:-0}
boot_local=$(date -r "$boot_epoch" '+%Y-%m-%d %H:%M:%S')
mkdir -p "$OUT"
chmod 700 "$OUT"

{
    printf 'started=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'boot_epoch=%s\n' "$boot_epoch"
    printf 'boot_local=%s\n' "$boot_local"
    printf 'boot_time=%s\n' "$(sysctl -n kern.boottime 2>/dev/null)"
    printf 'boot_args=%s\n' "$(nvram boot-args 2>/dev/null)"
    printf 'auto_login=%s\n' "$(sysadminctl -autologin status 2>&1)"
    printf 'sw_vers=%s\n' "$(sw_vers | tr '\n' ' ')"
    printf 'bundle_sha256=%s\n' "$(shasum -a 256 /Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS/Navi48Metal 2>/dev/null)"
    printf 'settle_seconds=%s\n' "$settle"
} > "$OUT/environment.txt"

cp /Library/GPUBundles/Navi48Metal.bundle/Contents/Info.plist "$OUT/Navi48Metal-Info.plist" 2>/dev/null || true

# Keep a remote recorder even if SSH disconnects during a login attempt or a
# WindowServer restart. It is killed by the verifier, not by rebooting services.
nohup log stream --style syslog --level debug \
    --predicate 'process == "WindowServer" OR process == "loginwindow" OR process == "Dock" OR process == "Finder" OR process == "sessionlogoutd" OR process == "launchd" OR process == "ReportCrash" OR process == "kernel"' \
    > "$OUT/stream.log" 2> "$OUT/stream-error.log" < /dev/null &
recorder=$!
printf '%s\n' "$recorder" > "$OUT/stream.pid"

sleep "$settle"
kill "$recorder" 2>/dev/null || true
wait "$recorder" 2>/dev/null || true

# Use the boot timestamp, not a moving duration or the filename of a report.
log show --start "$boot_local" --style syslog --info --debug \
    --predicate 'process == "WindowServer" OR process == "loginwindow" OR process == "Dock" OR process == "Finder" OR process == "sessionlogoutd" OR process == "launchd" OR process == "ReportCrash" OR process == "kernel"' \
    > "$OUT/unified.log" 2> "$OUT/unified-error.log" || true

# A process-only log query on this machine can return records from other
# boots with future timestamps. Preserve raw boot-scoped metadata instead.
# Include the WindowServer start marker as well as the compositor and probes.
log show --last boot --style ndjson --info --debug \
    --predicate 'composedMessage == "Server is starting up" OR composedMessage CONTAINS "Navi48Metal: CA" OR composedMessage CONTAINS "Metal compositor activated" OR composedMessage CONTAINS "BootSet="' \
    > "$OUT/ca-boot.ndjson" 2> "$OUT/ca-boot-error.log" || true
selector=${N48_BOOT_EVIDENCE:-$TOOLS/boot-evidence.py}
if [ ! -x "$selector" ]; then
    selector=$(dirname "$0")/boot-evidence.py
fi
python3 "$selector" "$OUT" "$OUT/ca-boot.ndjson" > "$OUT/ca-boot.txt" 2> "$OUT/ca-boot-selector-error.log" || true

python3 - "$OUT" <<'PY'
import pathlib
import re
import sys
import time
from datetime import datetime

out = pathlib.Path(sys.argv[1])
environment = (out / 'environment.txt').read_text()
boot = int(re.search(r'^boot_epoch=(\d+)$', environment, re.MULTILINE).group(1))
finished = int(__import__('time').time())
lines = []
for name in ['unified.log', 'stream.log']:
    try:
        source = (out / name).read_text(errors='replace')
    except OSError:
        continue
    for line in source.splitlines():
        match = re.match(r'^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)?)([+-]\d{4}) ', line)
        if not match:
            continue
        when = datetime.strptime(match.group(1) + match.group(2), '%Y-%m-%d %H:%M:%S.%f%z').timestamp()
        # This machine's unified log also has next-day 06:xx records from an
        # earlier boot. Keep only the real boot-to-capture interval.
        if boot <= when <= finished:
            lines.append((when, line))
lines.sort(key=lambda item: item[0])
(out / 'timeline.log').write_text('\n'.join(line for _, line in lines) + '\n')
PY

ps aux > "$OUT/processes.txt"
who > "$OUT/users.txt" 2>&1 || true
sysadminctl -autologin status > "$OUT/autologin.txt" 2>&1 || true
fdesetup status > "$OUT/filevault.txt" 2>&1 || true
ioreg -n Navi48Accelerator -r -d 1 > "$OUT/ioaccelerator.txt" 2>&1 || true
ioreg -n Navi48Bringup -r -d 1 > "$OUT/bringup.txt" 2>&1 || true
system_profiler SPDisplaysDataType > "$OUT/displays.txt" 2>&1 || true

TOOL=$TOOLS/navi48test
if [ -x "$TOOL" ]; then
    sudo "$TOOL" accel sessstat 4 > "$OUT/gpu-global.txt" 2>&1 || true
    sudo "$TOOL" accel pipestat > "$OUT/gpu-pipestat.txt" 2>&1 || true
    sudo "$TOOL" log > "$OUT/kext-ring.log" 2>&1 || true
fi

metal_smoke=${N48_METAL_SMOKE_SCRIPT:-$TOOLS/metal-smoke.sh}
if [ "${N48_METAL_SMOKE:-1}" != 0 ] && [ -x "$metal_smoke" ]; then
    "$metal_smoke" "$OUT/metal" > "$OUT/metal-smoke.log" 2>&1 || true
    if [ -f "$OUT/metal/result.txt" ]; then
        cp "$OUT/metal/result.txt" "$OUT/metal-result.txt"
    else
        printf 'METAL_SMOKE missing result\n' > "$OUT/metal-result.txt"
    fi
fi

# The probe is root-owned by WindowServer. Read only its bounded regular
# file; never truncate or remove evidence that a later restart may append to.
probe_dir=${N48_CA_REMOTE_DIR:-/private/tmp}
# Keep the source name as well as the path so missing evidence is still
# reported, rather than silently interpreted as zero events.
copy_probe() {
    label=$1
    source_file="$probe_dir/n48-ca-${label}-88.log"
    src=$source_file
    dest="$OUT/ca-${label}.log"
    if sudo test ! -e "$src"; then
        # push is created only on Apple's assertion. Absence is a legitimate
        # zero-event result; origin is required to prove installation worked.
        if [ "$label" = push ]; then
            : > "$dest"
            printf 'collected-empty-no-event\n' > "$OUT/ca-${label}-collected.txt"
        else
            printf 'absent\n' > "$OUT/ca-${label}-error.txt"
        fi
        return 0
    fi
    if sudo test -L "$src" || sudo test ! -f "$src"; then
        printf 'absent-or-not-regular\n' > "$OUT/ca-${label}-error.txt"
        return 0
    fi
    if sudo test "$(stat -f %z "$src")" -gt 65536; then
        printf 'oversize\n' > "$OUT/ca-${label}-error.txt"
        return 0
    fi
    if [ "$label" != push ] && sudo test "$(stat -f %Lp "$src")" != 600; then
        printf 'bad-mode\n' > "$OUT/ca-${label}-error.txt"
        return 0
    fi
    sudo cat "$src" > "$dest"
    printf 'collected\n' > "$OUT/ca-${label}-collected.txt"
}
if [ "$(id -u)" -ne 0 ]; then
    copy_probe origin
    copy_probe context
    copy_probe push
else
    for name in origin context push; do
        source_file="$probe_dir/n48-ca-${name}-88.log"
        if [ ! -f "$source_file" ] || [ -L "$source_file" ]; then
            printf 'absent-or-not-regular\n' > "$OUT/ca-${name}-error.txt"
        elif [ "$(stat -f %z "$source_file")" -gt 65536 ]; then
            printf 'oversize\n' > "$OUT/ca-${name}-error.txt"
        else
            cat "$source_file" > "$OUT/ca-${name}.log"
            printf 'collected\n' > "$OUT/ca-${name}-collected.txt"
        fi
    done
fi

mkdir -p "$OUT/reports"
find /Library/Logs/DiagnosticReports "$HOME/Library/Logs/DiagnosticReports" \
    -maxdepth 2 -type f \( -name 'WindowServer-*.ips' -o -name 'Dock-*.ips' -o -name 'Finder-*.ips' \) \
    -exec cp {} "$OUT/reports/" \; 2>/dev/null || true
if [ "$(id -u)" -ne 0 ]; then
    sudo find /Library/Logs/DiagnosticReports -maxdepth 2 -type f \
        \( -name 'WindowServer-*.ips' -o -name 'Dock-*.ips' -o -name 'Finder-*.ips' \) \
        -exec cp {} "$OUT/reports/" \; 2>/dev/null || true
    sudo find /Library/Logs/DiagnosticReports/Retired -type f \
        \( -name 'WindowServer-*.ips' -o -name 'Dock-*.ips' -o -name 'Finder-*.ips' \) \
        -exec cp {} "$OUT/reports/" \; 2>/dev/null || true
    sudo chmod -R a+rX "$OUT"
else
    find /Library/Logs/DiagnosticReports/Retired -type f \
        \( -name 'WindowServer-*.ips' -o -name 'Dock-*.ips' -o -name 'Finder-*.ips' \) \
        -exec cp {} "$OUT/reports/" \; 2>/dev/null || true
fi

python3 - "$OUT" <<'PY'
import json
import pathlib
import re
import subprocess
import sys
import time
from datetime import datetime

out = pathlib.Path(sys.argv[1])

def text(name):
    try:
        return (out / name).read_text(errors='replace')
    except OSError:
        return ''

checks = []
def check(name, ok, evidence):
    checks.append({'name': name, 'pass': bool(ok), 'evidence': evidence})

proc = text('processes.txt')
for name, pattern in [('windowserver', 'WindowServer -daemon'),
                      ('dock', '/Dock.app/Contents/MacOS/Dock'),
                      ('finder', '/Finder.app/Contents/MacOS/Finder')]:
    # process_dock / process_finder are actual desktop-session assertions,
    # not merely proof that a GPU bundle was discovered.
    alive = [line.strip() for line in proc.splitlines() if pattern in line and 'grep ' not in line]
    check('process_' + name, alive, alive or 'process not running')

boot_args = text('environment.txt')
check('native_gpu_boot_args', all(arg in boot_args for arg in
      ['navi48-native=1', 'navi48-metal=1', 'navi48-metal-ws=1']), boot_args)
accel = text('ioaccelerator.txt')
check('ioaccelerator_active', 'Navi48Accelerator' in accel and 'active' in accel, accel[:500])
logs = text('timeline.log')
boot_evidence = {}
try:
    boot_evidence = json.loads(text('boot-evidence.json'))
except (ValueError, json.JSONDecodeError):
    pass
boot_logs = text('ca-boot.txt')
check('metal_compositor', 'Metal compositor activated.' in boot_logs and 'BootSet=M' in boot_logs,
      [line for line in boot_logs.splitlines()
       if 'Metal compositor' in line or 'BootSet=' in line][-6:]
      or text('ca-boot-selector-error.log'))
gpu = text('gpu-global.txt')
check('gpu_not_hung', bool(re.search(r'open / DEAD slots / HUNG:\s*\d+ / 0 / 0', gpu)), gpu)
check('gpu_no_new_fault', 'CHANGED since the previous sessstat 4' not in gpu, gpu)
check('ca_sw_observer_installed',
      'CA SW origin observe-only installed=1' in boot_logs,
      [line for line in boot_logs.splitlines() if 'CA SW origin' in line][-12:])
check('ca_metal_observer_installed',
      'CA origin observe-only wrappers installed' in boot_logs,
      [line for line in boot_logs.splitlines()
       if 'CA origin observe-only wrappers installed' in line
       or 'CA origin probe NOT installed' in line][-12:])
push_evidence = text('ca-push.log').splitlines()
check('ca_push_evidence_collected', (out / 'ca-push-collected.txt').exists(),
      text('ca-push-error.txt') or 'no ca-push collection status')
check('ca_no_missing_surface_this_boot',
      (out / 'ca-push-collected.txt').exists() and not push_evidence, push_evidence[-16:])
check('ca_origin_evidence_collected', (out / 'ca-origin-collected.txt').exists(),
      text('ca-origin-error.txt') or 'no ca-origin collection status')
check('metal_smoke', 'METAL_SMOKE PASS' in text('metal-result.txt'),
      text('metal-smoke.log')[-8000:])
if (out / 'ca-origin-collected.txt').exists():
    origins = text('ca-origin.log').splitlines()
    creation = [line for line in origins if re.search(
        r'\bkind=(?:image|iosurface|reinterpreted|sw_iosurface|sw_iosurface_tracked)\b', line)]
    check('ca_creation_events', bool(creation), {
        'count': len(origins), 'creation_count': len(creation),
        'tail': origins[-12:],
    })
    tracked = [line for line in origins if 'kind=sw_iosurface_tracked' in line]
    check('ca_tracked_surfaces', bool(tracked), origins[-12:])
if (out / 'ca-context-collected.txt').exists():
    contexts = text('ca-context.log').splitlines()
    factory_events = [line for line in contexts if line.startswith('CA context ')]
    check('ca_context_events', bool(factory_events), contexts[-12:])
else:
    check('ca_context_evidence_collected', False,
          text('ca-context-error.txt') or 'no CA context collection status')
check('session_not_logged_out', 'startDirectLogout:reason:' not in logs and
      'Window Server exited, closing down the session immediately' not in logs,
      [line for line in logs.splitlines() if 'Logout' in line or 'Window Server exited' in line][-10:])

start = subprocess.run(['sysctl', '-n', 'kern.boottime'], capture_output=True, text=True).stdout
match = re.search(r'sec\s*=\s*(\d+)', start)
boot_epoch = int(match.group(1)) if match else 0
environment = text('environment.txt')
saved_boot = re.search(r'^boot_epoch=(\d+)$', environment, re.MULTILINE)
if saved_boot:
    boot_epoch = int(saved_boot.group(1))
recent = []
for report in sorted((out / 'reports').glob('*.ips')):
    try:
        with report.open() as f:
            meta = json.loads(f.readline())
            details = json.load(f)
        fault = details['threads'][details['faultingThread']]
        frames = []
        for frame in fault.get('frames', [])[:16]:
            image = details['usedImages'][frame['imageIndex']]
            frames.append({'image': image.get('name'), 'offset': frame['imageOffset'],
                           'symbol': frame.get('symbol'), 'symbolLocation': frame.get('symbolLocation')})
        event = {'file': report.name, 'timestamp': meta.get('timestamp'),
                 'exception': details.get('exception'), 'frames': frames}
        stamp = subprocess.run(['stat', '-f', '%m', str(report)], capture_output=True, text=True).stdout.strip()
        # ReportCrash can finish and move the report after WindowServer died.
        report_time = datetime.strptime(meta['timestamp'], '%Y-%m-%d %H:%M:%S.%f %z').timestamp()
        if stamp.isdigit() and report_time >= boot_epoch:
            recent.append(event)
    except (OSError, ValueError, KeyError, IndexError) as error:
        recent.append({'file': report.name, 'error': str(error)})
check('no_windowserver_crash_this_boot', not recent, recent)

result = {'pass': all(item['pass'] for item in checks), 'checks': checks}
(out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
for item in checks:
    print('%s %s' % ('PASS' if item['pass'] else 'FAIL', item['name']))
    if not item['pass']:
        print(json.dumps(item['evidence'], ensure_ascii=False)[:8000])
print('RESULT %s' % ('PASS' if result['pass'] else 'FAIL'))
sys.exit(0 if result['pass'] else 1)
PY

