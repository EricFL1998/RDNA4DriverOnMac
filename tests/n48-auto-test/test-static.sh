#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

sh -n "$ROOT/deploy-optimized-driver.sh"
sh -n "$ROOT/tools/n48-auto-test/config.sh"
sh -n "$ROOT/tools/n48-auto-test/n48-collect-reports.sh"
sh -n "$ROOT/tools/n48-auto-test/n48-verify-remote.sh"

python3 - "$ROOT" <<'PY'
from pathlib import Path
import re
import subprocess
import sys
root = Path(sys.argv[1])
deploy = (root / 'deploy-optimized-driver.sh').read_text()
verify = (root / 'tools/n48-auto-test/n48-verify-remote.sh').read_text()
plist = (root / 'tools/n48-auto-test/com.navi48.autotest.plist').read_text()
recorder = (root / 'tools/n48-auto-test/n48-boot-recorder.sh').read_text()
collect = (root / 'tools/n48-auto-test/n48-collect-reports.sh').read_text()
assert '/var/db/navi48-auto-test/tools/boot-recorder.sh' in plist, 'LaunchDaemon must call persistent recorder'
assert 'N48_RECORDER_BIN:-$ROOT/tools' in recorder, 'recorder tools must default under persistent root'
assert 'TOOL=$TOOLS/navi48test' in verify, 'verifier must use persistent deployed binary'
assert 'selector=${N48_BOOT_EVIDENCE:-$TOOLS/boot-evidence.py}' in verify, 'verifier must use persistent evidence selector'
assert 'N48_METAL_SMOKE_SCRIPT:-$TOOLS/metal-smoke.sh' in verify, 'manual smoke must use persistent script'
assert '/var/db/navi48-auto-test/tools/navi48test' in collect, 'manual collection must use the persistent deployed binary'
assert '/var/db/navi48-auto-test/tools/verify.sh' in collect, 'manual collection must use the persistent verifier'
assert '"$N48_TARGET:/var/db/navi48-auto-test/manual-latest/." "$archive/"' in collect, 'remote reports must copy into the separate local archive'
for feature in ('N48_TARGET', 'N48_TEST_ARTIFACT', 'n48-boot-report.sh',
                'n48-collect-reports.sh', '--reboot', '--check'):
    assert feature in deploy, f'missing deployment feature: {feature}'
assert 'killall' not in deploy, 'test driver must not kill WindowServer'
assert 'killall' not in verify, 'verifier must not kill WindowServer'
for feature in ('Retired', 'log stream', 'log show', 'kern.boottime',
                'process_dock', 'process_finder', 'BootSet=M',
                'gpu_not_hung', 'gpu_no_new_fault', 'n48-ca-${name}-88.log',
                'ca-origin.log', 'ca_no_missing_surface_this_boot',
                'ca_sw_observer_installed', 'ca_push_evidence_collected',
                'ca_origin_evidence_collected', 'ca_metal_observer_installed',
                'ca_creation_events', 'ca_context_events',
                'ca_context_evidence_collected', 'ca-context.log'):
    assert feature in verify, f'missing diagnostic feature: {feature}'
assert 'find /Library/Logs/DiagnosticReports ' in verify, 'active reports must be collected as well as Retired'
assert 'timeline.log' in verify, 'mixed boot logs must be filtered into a per-boot timeline'
assert 'boot <= when <= finished' in verify, 'old or future timestamps must not leak into test assertions'
assert 'Navi48Metal.bundle/Contents/MacOS/Navi48Metal | shasum' in deploy, 'verify the deployed executable, not only the tar checksum'
for script in root.joinpath('tools/n48-auto-test').glob('*.sh'):
    contents = script.read_text()
    assert 'lxc656' not in contents, 'credential leaked into test scripts'
subprocess.run(['sh', '-n', str(root / 'tools/n48-auto-test/n48-metal-smoke.sh')], check=True)
boot = subprocess.check_output(['sysctl', '-n', 'kern.boottime'], text=True)
pattern = r'.*{ sec = ([0-9][0-9]*), usec.*'
match = re.match(pattern, boot)
assert match, 'boot parser must extract sec, not usec'
assert int(match.group(1)) > 1600000000, 'parsed boot time is invalid'
print('PASS: deployment and diagnostic static checks')
PY
