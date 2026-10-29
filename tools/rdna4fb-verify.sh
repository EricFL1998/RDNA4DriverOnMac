#!/bin/bash
# Read-only RDNA4FB post-boot verification. Pass --arm to publish the Navi48
# display pipe and restart WindowServer after the read-only checks.

set -u
set -o pipefail

DEPLOY=/Volumes/1401/N48-Metal-Deploy
ARCHIVE=/Volumes/1401/logs-archive
TS=$(date +%Y%m%d-%H%M%S)
LOG="$ARCHIVE/rdna4fb-verify-$TS.log"
ARM=0
[ "${1:-}" = "--arm" ] && ARM=1

mkdir -p "$ARCHIVE"
exec > >(tee -a "$LOG") 2>&1

section() {
	printf '\n===== %s =====\n' "$*"
}

run() {
	printf '\n$ %s\n' "$*"
	"$@" || printf '[exit %s]\n' "$?"
}

section "RDNA4FB verify $TS"
printf 'log: %s\n' "$LOG"

section "system"
run uname -a
run sw_vers
run sysctl kern.boottime
run sysctl kern.bootargs
printf 'console user: %s\n' "$(stat -f %Su /dev/console 2>/dev/null || true)"
printf 'logged-in users:\n'
who || true
printf 'WindowServer pid: %s\n' "$(pgrep -x WindowServer 2>/dev/null || echo missing)"

section "kernel collections"
run sudo -n sh -c \
	'kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc | grep -E "RDNA4FB|accelprobe|highpoint"'
run sudo -n sh -c \
	'kmutil showloaded --list-only | grep -Ei "RDNA4FB|navi48|accelprobe"'
run shasum -a 256 /Library/Extensions/RDNA4FB.kext/Contents/MacOS/RDNA4FB
run shasum -a 256 /Library/KernelCollections/AuxiliaryKernelExtensions.kc

section "RDNA4FB registry"
if sudo -n ioreg -r -c RDNA4FB -l -w0 >/tmp/rdna4fb-ioreg.txt 2>&1; then
	grep -E 'IOClass|IOMatchCategory|IOProviderClass|model|Console,|VRAM,TotalMB|MMIO,Verified|AtomBIOS,|Discovery,' \
		/tmp/rdna4fb-ioreg.txt || true
else
	cat /tmp/rdna4fb-ioreg.txt
fi

section "framebuffer tree"
run sudo -n ioreg -l -w0 -r -c IOFramebuffer -d 2

section "displays"
run system_profiler SPDisplaysDataType

section "driver log"
sudo -n dmesg 2>/dev/null |
	grep -E 'RDNA4FB:|Navi48Bringup:|Unable to find display pipe|Software compositor' |
	tail -160 || true

section "WindowServer GPU log"
log show --last boot --style compact --predicate \
	'process == "WindowServer" OR eventMessage CONTAINS "Unable to find display pipe" OR eventMessage CONTAINS "Navi48Metal"' \
	2>/dev/null |
	grep -E 'Unable to find display pipe|Software compositor|Navi48Metal|RDNA4FB|Accelerator' |
	tail -200 || true

section "Navi48 status"
if [ -x "$DEPLOY/n48nub" ]; then
	run sudo -n "$DEPLOY/n48nub" status
else
	printf 'missing %s\n' "$DEPLOY/n48nub"
fi
if [ -x "$DEPLOY/navi48test" ]; then
	run sudo -n "$DEPLOY/navi48test" accel m6stat 0
else
	printf 'missing %s\n' "$DEPLOY/navi48test"
fi

if [ "$ARM" -eq 1 ]; then
	section "arm display pipe"
	for bin in n48nub navi48test; do
		if [ ! -x "$DEPLOY/$bin" ]; then
			printf 'missing executable: %s\n' "$DEPLOY/$bin"
			exit 2
		fi
	done

	run sudo -n "$DEPLOY/n48nub" publish
	run sudo -n "$DEPLOY/navi48test" accel pipeadopt
	run sudo -n "$DEPLOY/navi48test" accel pipearm 1
	run sudo -n "$DEPLOY/navi48test" accel pipereload 0
	run sudo -n launchctl kickstart -k system/com.apple.WindowServer
	sleep 8

	section "post-arm state"
	run sudo -n "$DEPLOY/navi48test" accel m6stat 0
	run sudo -n ioreg -r -c Navi48MetalNub -l -w0
	run sudo -n ioreg -r -c Navi48DisplayPipe -l -w0
	run system_profiler SPDisplaysDataType
	sudo -n dmesg 2>/dev/null |
		grep -E 'Navi48Metal|display pipe|compositor|command_buffers|frames' |
		tail -200 || true
fi

section "summary gates"
FAIL=0
sudo -n kmutil showloaded --list-only 2>/dev/null | grep -q 'com.hackintosh.RDNA4FB' ||
	{ echo 'FAIL: RDNA4FB not loaded'; FAIL=1; }
sudo -n ioreg -r -c RDNA4FB -l -w0 2>/dev/null | grep -q 'IOClass.*RDNA4FB' ||
	{ echo 'FAIL: no RDNA4FB service'; FAIL=1; }
sudo -n ioreg -l -w0 -r -c IOFramebuffer -d 2 2>/dev/null | grep -q 'RDNA4FB' ||
	{ echo 'FAIL: RDNA4FB not in framebuffer tree'; FAIL=1; }
if sudo -n dmesg 2>/dev/null | grep -q 'Unable to find display pipe'; then
	echo 'FAIL: WindowServer still reports no display pipe'
	FAIL=1
fi
[ "$FAIL" -eq 0 ] && echo 'PASS: RDNA4FB is loaded and attached'
exit "$FAIL"
