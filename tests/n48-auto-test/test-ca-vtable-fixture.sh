#!/bin/sh
# Check the actual 25G241 vtable used by the production address constants.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
QC="$ROOT/diagnostics/20261026-scanout-recovery/dyld-cache/QuartzCore"
python3 - "$ROOT" "$QC" <<'PY'
import pathlib
import struct
import subprocess
import sys

root, qc = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
nm = subprocess.check_output(['nm', '-arch', 'x86_64', '-nm', str(qc)],
                             text=True, stderr=subprocess.DEVNULL)
symbols = {}
for line in nm.splitlines():
    fields = line.split()
    if len(fields) >= 3:
        try:
            symbols[fields[-1]] = int(fields[0], 16)
        except ValueError:
            pass

def symbol(suffix):
    matches = [address for name, address in symbols.items() if name.endswith(suffix)]
    assert len(matches) == 1, (suffix, matches)
    return matches[0]

anchor = symbols['_CARenderOGLNew_']
image = symbol('12MetalContext13image_surfaceEPNS0_5ImageEPNS_6Render7TextureE')
ios = symbol('12MetalContext29create_surface_from_iosurfaceEiiiP11__IOSurface')
reinterpret = symbol('12MetalContext28create_reinterpreted_surfaceEPNS0_7SurfaceEb')
copy_clear = symbol('7Context24copy_surface_clear_flagsEPNS0_7SurfaceES3_')
source = (root / 'navi48/Cobalt64-main/tools/native/navi48metal/Navi48Device.m').read_text()
installer = source[source.index('static void n48_install_metal_image_surface_probe'):
                   source.index('static void n48_install_sw_surface_probe')]
assert 'reinterpret_slot = (void **)(vptr + 0x2e0)' in installer, \
    'production must observe the actual reinterpreted-surface virtual slot'
for label, expected in [('expected', image), ('ios_expected', ios),
                        ('reinterpret_expected', reinterpret)]:
    delta = expected - anchor
    assert f'{label} = (void *)((uintptr_t)anchor + {delta:#x})' in installer, \
        f'production {label} target does not match the real vtable: {delta:#x}'

def decode_chained(value):
    # This checked-in 25G241 fixture has the same dyld rebase as its live
    # addresses. A raw pointer cannot be tested by a generic bit-mask guess.
    return value + 0x7df800000000

assert decode_chained(0x02000d2c8d4e) == 0x7ff80d2c8d4e, hex(decode_chained(0x02000d2c8d4e))

const_addr = 0x7ff840d2bd70
const_offset = 4105104
vptr = anchor + 0x33ac6dd2
with qc.open('rb') as f:
    for offset, expected in [(0x298, image), (0x2d8, ios),
                             (0x2e0, reinterpret), (0x2e8, copy_clear)]:
        f.seek(const_offset + vptr - const_addr + offset)
        chained = struct.unpack('<Q', f.read(8))[0]
        actual = decode_chained(chained)
        assert actual == expected, (hex(offset), hex(actual), hex(expected))
    # Decode the adjacent SW table too; it is the repair boundary, not Metal's.
    sw_ptr = anchor + 0x33ad2d22
    sw_ios = symbol('9SWContext29create_surface_from_iosurfaceEiiiP11__IOSurface')
    sw_finalize = symbol('9SWContext16finalize_surfaceEPNS0_7SurfaceE')
    for offset, expected in [(0x2d8, sw_ios), (0x320, sw_finalize)]:
        f.seek(const_offset + sw_ptr - const_addr + offset)
        chained = struct.unpack('<Q', f.read(8))[0]
        actual = decode_chained(chained)
        assert actual == expected, (hex(offset), hex(actual), hex(expected))
print(f'PASS: Metal reinterpret is +0x2e0 (anchor {reinterpret - anchor:#x}); +0x2e8 stays copy-clear')
assert '.context_reinterpreted_surface = *(uintptr_t *)(*(uintptr_t *)f.context + 0x2e0)' in source, \
    'push diagnostics must name the real reinterpreted method, not copy-clear'
PY
