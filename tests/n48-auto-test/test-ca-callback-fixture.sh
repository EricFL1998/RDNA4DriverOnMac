#!/bin/sh
# Prove the actual 25G241 callback table layout used by the import observer.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
QC="$ROOT/diagnostics/20261027-live-skylight-cache/extracted/QuartzCore"
SL="$ROOT/diagnostics/20261027-live-skylight-cache/extracted/SkyLight"

python3 - "$ROOT" "$QC" "$SL" <<'PY'
import pathlib
import re
import struct
import subprocess
import sys

root, qc, sl = map(pathlib.Path, sys.argv[1:])

def symbols(binary):
    result = {}
    output = subprocess.check_output(['nm', '-arch', 'x86_64', '-nm', str(binary)],
                                     text=True, stderr=subprocess.DEVNULL)
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            try:
                result[fields[-1]] = int(fields[0], 16)
            except ValueError:
                pass
    return result

q = symbols(qc)
s = symbols(sl)
anchor = q['_CARenderOGLNew_']
factory = q['_kCARenderMetalCallbacks']
sls = s['_SLSMainDisplayID']
stub = int(next(line.split()[0] for line in subprocess.check_output(
    ['otool', '-Iv', str(sl)], text=True).splitlines()
    if line.endswith('_CARenderOGLNew_')), 16)
disassembly = subprocess.check_output(['objdump', '-d', '--arch=x86_64',
    f'--start-address={stub:#x}', f'--stop-address={stub + 6:#x}', str(sl)], text=True)
slot = int(re.search(r'## (0x[0-9a-f]+)', disassembly).group(1), 16)
raw = bytes.fromhex(re.search(r': ((?:[0-9a-f]{2} )+)', disassembly).group(1))
assert raw[:2] == b'\xff\x25' and len(raw) == 6
assert stub + 6 + int.from_bytes(raw[2:6], 'little', signed=True) == slot
assert slot - sls == 0x36c69405
assert factory - anchor == 0x33ac54e2
assert factory - 0x7ff840d2bd70 >= 0

# The shared-cache pointer uses the same rebasing convention as the existing
# vtable fixture; don't mistake the encoded pointer for a function address.
data = qc.read_bytes()
entry_offset = 4105104 + factory - 0x7ff840d2bd70
encoded = struct.unpack_from('<Q', data, entry_offset)[0]
assert encoded + 0x7df800000000 == q['__ZL17metal_new_contextPvS_j']

source = (root / 'navi48/Cobalt64-main/tools/native/navi48metal/Navi48Device.m').read_text()
header = (root / 'navi48/Cobalt64-main/tools/native/navi48metal/n48_ca_import_hook.h').read_text()
probe = (root / 'navi48/Cobalt64-main/tools/native/navi48metal/n48_ca_context_probe.h').read_text()
assert '(uintptr_t)sl_anchor + 0x2b215b' in source
assert '((uintptr_t)sl_anchor + 0x36c69405)' in source
assert '- 0x36c69405' not in source
assert 'n48_ca_callbacks, void *, void *, unsigned' in header
assert 'original(callbacks, device, options, priority)' in probe
assert 'original(thunks[slot]' not in probe
wrapper = source[source.index('static void *n48_ca_render_new_context_probe'):
                   source.index('static void n48_ca_origin_record')]
assert wrapper.count('n48_orig_ca_context_render_new(callbacks') == 0, \
    'production must use the single-call helper, not call the original twice'
assert wrapper.count('n48_ca_render_new_observe(n48_orig_ca_context_render_new,') == 1
print('PASS: callback table, three-argument factory, GOT delta, single original call')
PY
