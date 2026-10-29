#!/bin/sh
# Run the read-only verifier, archive it locally, and preserve a nonzero
# test result as a normal outcome rather than an unexplained SSH failure.
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
# shellcheck source=tools/n48-auto-test/config.sh
. "$ROOT/tools/n48-auto-test/config.sh"

label=${1:-boot}
stamp=$(date +%Y%m%d-%H%M%S)
name="$label-$stamp"
archive="$ROOT/$N48_DIAGNOSTIC_DIR/$name"
mkdir -p "$archive"

if [ "$label" = boot ]; then
    boot_epoch=$(n48_ssh 'sysctl -n kern.boottime' |
        sed -n 's/.*{ sec = \([0-9][0-9]*\), usec.*/\1/p')
    case $boot_epoch in
        ''|*[!0-9]*) echo 'cannot determine target boot epoch' >&2; exit 3 ;;
    esac
    exec "$ROOT/tools/n48-auto-test/n48-boot-report.sh" fetch "$archive" "$boot_epoch"
fi

n48_scp "$ROOT/tools/n48-auto-test/n48-verify-remote.sh" "$N48_TARGET:/tmp/n48-verify.sh"
n48_scp "$ROOT/tools/n48-auto-test/n48-boot-evidence.py" "$N48_TARGET:/tmp/n48-boot-evidence.py"
n48_scp "$ROOT/tools/n48-auto-test/n48-metal-smoke.sh" "$N48_TARGET:/tmp/n48-metal-smoke.sh"
n48_scp "$ROOT/navi48/Cobalt64-main/tools/native/mtlprobe/mtlprobe" "$N48_TARGET:/tmp/mtlprobe"
n48_scp "$ROOT/diagnostics/n48-auto-test/metal-after-sw-repair/expected.raw" "$N48_TARGET:/tmp/expected.raw"
n48_ssh 'sudo codesign --force --sign - /tmp/mtlprobe &&
    sudo mkdir -p /var/db/navi48-auto-test/tools &&
    sudo install -m 755 /tmp/n48-verify.sh /var/db/navi48-auto-test/tools/verify.sh &&
    sudo install -m 755 /tmp/n48-boot-evidence.py /var/db/navi48-auto-test/tools/boot-evidence.py &&
    sudo install -m 755 /tmp/n48-metal-smoke.sh /var/db/navi48-auto-test/tools/metal-smoke.sh &&
    sudo install -m 755 /tmp/mtlprobe /var/db/navi48-auto-test/tools/mtlprobe &&
    sudo install -m 644 /tmp/expected.raw /var/db/navi48-auto-test/tools/expected.raw &&
    sudo chmod 755 /var/db/navi48-auto-test/tools &&
    sudo chmod 700 /var/db/navi48-auto-test'
set +e
n48_ssh 'sudo /var/db/navi48-auto-test/tools/navi48test accel sessstat 4 >/dev/null 2>&1 || true; sudo -u louxuanchen env N48_SETTLE_SECONDS="${N48_SETTLE_SECONDS:-30}" N48_METAL_SMOKE_SCRIPT=/var/db/navi48-auto-test/tools/metal-smoke.sh N48_MTLPROBE=/var/db/navi48-auto-test/tools/mtlprobe N48_EXPECTED=/var/db/navi48-auto-test/tools/expected.raw sh /var/db/navi48-auto-test/tools/verify.sh /var/db/navi48-auto-test/manual-latest' > "$archive/result.txt" 2>&1
result=$?
set -e
n48_scp -r "$N48_TARGET:/var/db/navi48-auto-test/manual-latest/." "$archive/" || true
printf '%s\n' "$result" > "$archive/exit-code.txt"
printf '%s\n' "$archive"
cat "$archive/result.txt"
exit "$result"
