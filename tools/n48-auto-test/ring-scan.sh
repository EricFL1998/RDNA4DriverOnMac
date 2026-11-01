#!/bin/bash
# Run StageProbe and scan the attribute ring at the base *that run* programmed,
# parsed straight out of its PM4 stream. Never assume the ring address.
set -u
cd "$(dirname "$0")"
LIB=${1:-}
SCAN_OFFSETS=${2:-0x0}
LOG=${3:-run.log}
PM4=/tmp/n48probe/cmd.pm4
rm -f "$PM4"
if [ -n "$LIB" ]; then
    sudo cp "$LIB" /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib
fi
sudo perl -e "alarm 90; exec @ARGV" -- /usr/bin/env \
    N48_TRIANGLE_PM4="$PM4" ${EXTRA_ENV:-} ./StageProbe > "$LOG" 2>&1
rc=$?
base=$(python3 - <<'EOF'
import struct
w = struct.unpack("<%dI" % (len(open('/tmp/n48probe/cmd.pm4','rb').read())//4),
                  open('/tmp/n48probe/cmd.pm4','rb').read())
lo = hi = 0
i = 0
while i < len(w):
    h = w[i]; n = ((h >> 16) & 0x3fff) + 1; op = (h >> 8) & 0xff
    if op == 0x76:
        for k in range(n - 1):
            a = 0xb000 + (w[i+1] + k) * 4
            if a == 0xb230: lo = w[i+2+k]
            if a == 0xb234: hi = w[i+2+k] & 0xffff
    i += n + 1
print((hi << 32) | lo)
EOF
)
printf 'exit=%s ringBase=0x%x\n' "$rc" "$base"
grep -E "Rendered|mismatches|center=" "$LOG" | sed 's/.*StageProbe\[[0-9:]*\] //'
for off in $SCAN_OFFSETS; do
    addr=$((base + off))
    printf 'scan +%s: ' "$off"
    sudo ./ReadHex "$(printf 0x%x $addr)" 2 | tail -1
done
