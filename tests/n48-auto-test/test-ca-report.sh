#!/bin/sh
# Run the verifier's real report-decision block against synthetic evidence.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/n48-ca-report.XXXXXX")
trap 'rm -rf "$W"' EXIT HUP INT TERM
python3 - "$ROOT/tools/n48-auto-test/n48-verify-remote.sh" "$W/report.py" <<'PY'
import pathlib,sys
text=pathlib.Path(sys.argv[1]).read_text()
start=text.index("python3 - \"$OUT\" <<'PY'", text.index('find /Library/Logs/DiagnosticReports'))
start=text.index('\n',start)+1
end=text.index('\nPY\n',start)
pathlib.Path(sys.argv[2]).write_text(text[start:end]+'\n')
PY

mkdir -p "$W/out/reports"
cat > "$W/out/processes.txt" <<'EOF'
_windowserver 203 /System/Library/PrivateFrameworks/SkyLight.framework/Resources/WindowServer -daemon
louxuanchen 951 /System/Library/CoreServices/Dock.app/Contents/MacOS/Dock
louxuanchen 954 /System/Library/CoreServices/Finder.app/Contents/MacOS/Finder
EOF
cat > "$W/out/environment.txt" <<'EOF'
boot_epoch=1
navi48-native=1 navi48-metal=1 navi48-metal-ws=1
EOF
cat > "$W/out/ioaccelerator.txt" <<'EOF'
Navi48Accelerator active
EOF
cat > "$W/out/timeline.log" <<'EOF'
Metal compositor activated. BootSet=M
Navi48Metal: CA SW origin observe-only installed=1 (ios=1 finalize=2)
Navi48Metal: CA origin observe-only wrappers installed (image=1 ios=2 reinterpret=3)
EOF
cat > "$W/out/gpu-global.txt" <<'EOF'
open / DEAD slots / HUNG: 2 / 0 / 0
EOF
printf 'METAL_SMOKE PASS\n' > "$W/out/metal-result.txt"
printf 'collected\n' > "$W/out/ca-origin-collected.txt"
printf 'collected\n' > "$W/out/ca-context-collected.txt"
printf 'CA context seq=0 kind=metal flags=0x22410 priority=0 context=0x1234 id=1\n' > "$W/out/ca-context.log"
printf 'Navi48Metal: CA context import observers installed\n' >> "$W/out/ca-boot.txt"
printf 'collected-empty-no-event\n' > "$W/out/ca-push-collected.txt"
: > "$W/out/ca-push.log"
cp "$ROOT/diagnostics/n48-auto-test/reboot-20261027-033000/ca-boot.ndjson" "$W/out/ca-boot.ndjson"
python3 "$ROOT/tools/n48-auto-test/n48-boot-evidence.py" "$W/out" "$W/out/ca-boot.ndjson" > "$W/out/ca-boot.txt"

for mode in healthy missing missing_metal empty; do
    case $mode in
        healthy) printf 'CA origin kind=iosurface\nCA origin kind=sw_iosurface_tracked\n' > "$W/out/ca-origin.log" ;;
        missing|missing_metal) printf 'CA origin kind=finalize\n' > "$W/out/ca-origin.log" ;;
        empty) : > "$W/out/ca-origin.log" ;;
    esac
    if [ "$mode" = missing_metal ]; then
        mv "$W/out/timeline.log" "$W/out/timeline.with-metal"
        /usr/bin/grep -v 'wrappers installed' "$W/out/timeline.with-metal" > "$W/out/timeline.log"
        /usr/bin/grep -v 'wrappers installed' "$W/out/ca-boot.txt" > "$W/out/ca-boot.filtered"
        mv "$W/out/ca-boot.filtered" "$W/out/ca-boot.txt"
    fi
    set +e
    python3 "$W/report.py" "$W/out" > "$W/out/result.txt"
    rc=$?
    set -e
    if [ "$mode" = healthy ]; then
        [ "$rc" -eq 0 ] || { cat "$W/out/result.txt"; exit 1; }
    else
        [ "$rc" -ne 0 ] || { echo "$mode: incomplete evidence passed"; exit 1; }
        /usr/bin/grep -E 'FAIL (ca_creation_events|ca_context_events|ca_context_evidence_collected)' "$W/out/result.txt" >/dev/null || {
            cat "$W/out/result.txt"; exit 1;
        }
        if [ "$mode" = missing_metal ]; then
            /usr/bin/grep 'FAIL ca_metal_observer_installed' "$W/out/result.txt" >/dev/null || {
                cat "$W/out/result.txt"; exit 1;
            }
            mv "$W/out/timeline.with-metal" "$W/out/timeline.log"
            python3 "$ROOT/tools/n48-auto-test/n48-boot-evidence.py" "$W/out" "$W/out/ca-boot.ndjson" > "$W/out/ca-boot.txt"
        fi
    fi
done
echo 'PASS: CA report distinguishes successful and missing creation evidence'
