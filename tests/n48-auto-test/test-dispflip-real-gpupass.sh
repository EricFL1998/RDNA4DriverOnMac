#!/bin/sh
# dispflip must exercise the production GPUPass acceptance rule, not a
# private present-all override or a different shader family.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
python3 - "$ROOT/navi48/Cobalt64-main/tools/native/mtlprobe/mtlprobe.m" <<'PY'
import pathlib, sys
text = pathlib.Path(sys.argv[1]).read_text()
start = text.index('static int cmd_dispflip(')
end = text.index('\nstatic int cmd_iosurface(', start)
body = text[start:end]
assert 'CoreDisplay.framework/Versions/A/Resources/default.metallib' in body, \
    'dispflip must use CoreDisplay GPUPass, not blend_fs'
assert 'newFunctionWithName:@"GPUPass" constantValues:cv' in body
assert 'newFunctionWithName:@"ViewportToNDC"' in body
assert 'df_draw(dev, q, pso, pattern, lut, tex, frame)' in text[start:end], \
    'every displayed frame must use the real pure-copy GPUPass draw'
assert 'blend_fs' not in body
assert 'presentall' not in body
assert 'n48m-presentall' not in body
print('Expected RED: dispflip still uses blend_fs rather than real GPUPass')
PY
