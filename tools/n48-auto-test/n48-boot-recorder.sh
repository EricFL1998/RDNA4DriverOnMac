#!/bin/sh
# Capture once, early at boot. The caller decides when to retrieve the result;
# this script must remain independent of SSH, WindowServer, and the login UI.
set -u

ROOT=${N48_RECORDER_ROOT:-/var/db/navi48-auto-test}
BIN=${N48_RECORDER_BIN:-$ROOT/tools}
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
umask 077

if [ -n "${N48_BOOT_EPOCH:-}" ]; then
    boot_epoch=$N48_BOOT_EPOCH
else
    boot_epoch=$(sysctl -n kern.boottime 2>/dev/null |
        sed -n 's/.*{ sec = \([0-9][0-9]*\), usec.*/\1/p')
fi
case $boot_epoch in
    ''|*[!0-9]*) echo 'cannot determine the current boot epoch' >&2; exit 2 ;;
esac
mkdir -p "$ROOT/$boot_epoch"
OUT="$ROOT/$boot_epoch"
chmod 700 "$ROOT" "$OUT"

# Use the deployed tools, not the mutable user home directory. Keep the
# report even when Metal smoke or the graphical session fails immediately.
N48_METAL_SMOKE=0 sh "$BIN/verify.sh" "$OUT" > "$OUT/result.txt" 2>&1
result=$?
printf '%s\n' "$result" > "$OUT/capture-exit-code.txt"
python3 - "$OUT/capture.json" "$boot_epoch" "$result" <<'PY'
import datetime
import json
import pathlib
import sys
pathlib.Path(sys.argv[1]).write_text(json.dumps({
    'complete': True,
    'boot_epoch': int(sys.argv[2]),
    'capture_exit_code': int(sys.argv[3]),
    'finished': datetime.datetime.now(datetime.timezone.utc).isoformat(),
}, indent=2) + '\n')
PY
chmod -R a+rX "$ROOT" "$OUT"

python3 - "$ROOT" "$boot_epoch" "${N48_REPORT_LIMIT:-8}" <<'PY'
import json
import pathlib
import shutil
import sys
root = pathlib.Path(sys.argv[1])
current = sys.argv[2]
limit = max(2, int(sys.argv[3]))
reports = []
for path in root.iterdir():
    if path.name == current or not path.is_dir() or path.is_symlink():
        continue
    try:
        complete = json.loads((path / 'capture.json').read_text()).get('complete') is True
        int(path.name)
    except (OSError, ValueError, KeyError):
        continue
    if complete:
        reports.append(path)
for path in sorted(reports, key=lambda p: int(p.name), reverse=True)[limit - 1:]:
    shutil.rmtree(path)
PY

exit "$result"
