#!/bin/sh
# The process-based syslog query can contain historical records with wrong
# timestamps. Select the current boot from NDJSON metadata instead.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/n48-boot-evidence.XXXXXX")
trap 'rm -rf "$W"' EXIT HUP INT TERM
cat > "$W/events.ndjson" <<'EOF'
{"bootUUID":"previous","processID":203,"processImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer","senderImagePath":"/Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS/Navi48Metal","timestamp":"2026-10-27 01:56:45+0800","eventMessage":"Server is starting up"}
{"bootUUID":"previous","processID":203,"processImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer","senderImagePath":"/Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS/Navi48Metal","timestamp":"2026-10-27 01:56:46+0800","eventMessage":"CA origin observe-only wrappers installed"}
{"bootUUID":"current","processID":203,"processImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer","senderImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight","timestamp":"2026-10-27 03:29:34+0800","eventMessage":"Server is starting up"}
{"bootUUID":"current","processID":203,"processImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer","senderImagePath":"/Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS/Navi48Metal","timestamp":"2026-10-27 03:30:43+0800","eventMessage":"Navi48Metal: CA origin observe-only wrappers installed"}
{"bootUUID":"current","processID":203,"processImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer","senderImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight","timestamp":"2026-10-27 03:30:45+0800","eventMessage":"Metal compositor activated. BootSet=M"}
{"bootUUID":"other","processID":833,"processImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer","senderImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight","timestamp":"2026-10-27 03:30:46+0800","eventMessage":"Server is starting up"}
{"bootUUID":"current","processID":833,"processImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer","senderImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight","timestamp":"2026-10-27 03:30:47+0800","eventMessage":"Metal compositor activated. BootSet=M"}
{"bootUUID":"current","processID":203,"processImagePath":"/System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer","senderImagePath":"/Library/GPUBundles/Navi48Metal.bundle/Contents/MacOS/Navi48Metal","timestamp":"2026-10-27 03:30:48+0800","eventMessage":"Navi48Metal: CA SW origin observe-only installed=1"}
{"count":8,"finished":1}
EOF
python3 - "$W/events.ndjson" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
lines = p.read_text().splitlines()
record = json.loads(lines[2])
record['formatString'] = record.pop('eventMessage')
lines[2] = json.dumps(record)
p.write_text('\n'.join(lines) + '\n')
PY
python3 "$ROOT/tools/n48-auto-test/n48-boot-evidence.py" "$W/out" "$W/events.ndjson" > "$W/result.txt"
python3 - "$W" <<'PY'
import json, pathlib, sys
w = pathlib.Path(sys.argv[1])
result = json.loads((w/'out/boot-evidence.json').read_text())
assert result['boot_uuid'] == 'current'
assert len(result['events']) == 5, result['events']
assert len([e for e in result['events'] if e['processID'] == 203]) == 4
assert len([e for e in result['events'] if e['processID'] == 833]) == 1
text = (w/'result.txt').read_text()
assert 'Metal compositor activated.' in text
assert 'CA origin observe-only wrappers installed' in text
assert 'CA SW origin observe-only installed=1' in text
print('PASS: boot UUID filters historical timestamps and later WS instances')
PY

/usr/bin/grep -v 'Server is starting up' "$W/events.ndjson" > "$W/no-start.ndjson"
python3 "$ROOT/tools/n48-auto-test/n48-boot-evidence.py" "$W/no-start" "$W/no-start.ndjson" > "$W/no-start.txt"
/usr/bin/grep 'boot_uuid=current' "$W/no-start.txt" >/dev/null
echo 'PASS: compositor boot UUID suffices without a startup marker'
