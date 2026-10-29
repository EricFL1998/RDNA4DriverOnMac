#!/bin/bash
# Recover to the pre-RDNA4FB AuxKC. Intended for macOS Safe Mode or Recovery.

set -euo pipefail

VOL=/Volumes/1401
KEXT=/Library/Extensions/RDNA4FB.kext
AUX=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
WANT_AUX_SHA=1160227c8b9ccea7521f1fdb91af4e9c78381bad3b4a9cd1f52547b7137ee60d

if [ "$(id -u)" -ne 0 ]; then
	echo "run with sudo: sudo bash $0" >&2
	exit 1
fi

if [ ! -d "$VOL" ]; then
	diskutil mount /dev/disk3s1 >/dev/null 2>&1 || true
fi
if [ ! -d "$VOL/logs-archive" ]; then
	echo "USB archive volume is not mounted at $VOL" >&2
	exit 2
fi

BACKUP=$(find "$VOL/logs-archive" -type f \
	-path '*Library-KernelCollections-before-RDNA4FB-*/AuxiliaryKernelExtensions.kc' |
	sort -r | head -1)
if [ -z "$BACKUP" ]; then
	echo "pre-RDNA4FB AuxKC backup not found" >&2
	exit 3
fi

GOT=$(shasum -a 256 "$BACKUP" | awk '{print $1}')
if [ "$GOT" != "$WANT_AUX_SHA" ]; then
	echo "backup hash mismatch: $BACKUP" >&2
	echo "want $WANT_AUX_SHA" >&2
	echo "got  $GOT" >&2
	exit 4
fi

TS=$(date +%Y%m%d-%H%M%S)
if [ -d "$KEXT" ]; then
	ARCHIVE="$VOL/logs-archive/removed-RDNA4FB-$TS.kext"
	ditto --norsrc "$KEXT" "$ARCHIVE"
	if [ ! -x "$ARCHIVE/Contents/MacOS/RDNA4FB" ]; then
		echo "failed to archive RDNA4FB.kext" >&2
		exit 5
	fi
	rm -rf "$KEXT"
	echo "archived kext: $ARCHIVE"
fi

install -m 744 -o root -g wheel "$BACKUP" "$AUX"
sync

GOT=$(shasum -a 256 "$AUX" | awk '{print $1}')
if [ "$GOT" != "$WANT_AUX_SHA" ]; then
	echo "restored AuxKC hash mismatch: $GOT" >&2
	exit 6
fi
if kmutil inspect -A "$AUX" 2>/dev/null | grep -q 'com.hackintosh.RDNA4FB'; then
	echo "RDNA4FB is still present in the restored AuxKC" >&2
	exit 7
fi

echo "RDNA4FB removed and pre-install AuxKC restored."
echo "Reboot to return to the generic IONDRVFramebuffer."
