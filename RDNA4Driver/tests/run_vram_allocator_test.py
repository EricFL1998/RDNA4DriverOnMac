#!/usr/bin/env python3
"""Compile the real allocator methods with a host-only driver fixture.

ComputeSubmission.cpp also contains MMIO/queue bring-up. Extract only complete
allocator definitions rather than duplicating their algorithm in a fake driver.
The kext build separately checks these methods against the real driver layout.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "src/core/ComputeSubmission.cpp"
text = source.read_text()
names = (
    "allocateVRAMBuffer", "userHeapGpuAddr", "userHeapSize", "allocateUserVRAMBuffer",
    "freeVRAMBuffer", "vramFreeInsert", "vramFreeTake", "vramBlocksReset",
)
definitions = []
guard = re.search(r"^class VramLockGuard \{.*?^};", text, re.M | re.S)
if guard:
    definitions.append(guard.group())
for name in names:
    match = re.search(r"^(?:uint64_t|void|bool) RDNA4Driver::" + name + r"\(.*?^}",
                      text, re.M | re.S)
    if not match:
        raise RuntimeError(f"missing allocator definition: {name}")
    line = text.count("\n", 0, match.start()) + 1
    definitions.append(f'#line {line} "{source}"\n{match.group()}')
fenced_submit = re.search(r"^bool RDNA4Driver::submitUserComputeCommands\(.*?^}",
                          text, re.M | re.S)
if fenced_submit:
    definitions.append(fenced_submit.group())
with tempfile.TemporaryDirectory(prefix="rdna4-vram-test-") as directory:
    directory = pathlib.Path(directory)
    (directory / "vram-allocation-methods.inc").write_text("\n".join(definitions))
    executable = directory / "test_vram_allocator"
    subprocess.run([
        "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
        *(["-DTEST_FENCED_SUBMIT"] if fenced_submit else []),
        "-I" + str(root / "tests/host-shim"), "-I" + str(directory),
        str(root / "tests/test_vram_allocator.cpp"),
        str(root / "src/hardware/PM4BuilderKernel.cpp"), "-o", str(executable),
    ], check=True)
    sys.exit(subprocess.run([str(executable)]).returncode)
