# Framebuffer Resource Lifecycle Design

## Intent

RX 9070 XT / RDNA4 currently reaches real Metal device creation, Core Animation Metal context creation, and Metal smoke rendering. The remaining desktop path stops earlier: the native `Navi48DisplayPipe` returns a raw stand-in for `initFramebufferResource`, and the subsequent `IOAccelResource2::prepare()` fails. This work must establish the real framebuffer resource lifecycle, not merely silence that error.

## Evidence

- Target boot `1793059064` records three `slot 267` returns followed by `IOAccelDisplayPipe::init_framebuffer_resource ... failed to prepare framebuffer resource`.
- The same boot has `caller_DisplaySurface_GetMTLTexture=0` and production `frames=0`; the texture reached by the driver is `CaptureSurfaceMetal`, not the display pipeline.
- The production callback-return observer now records a non-NULL `kind=metal` context. That part must not be changed.
- `IOAccelDisplayPipe::init_framebuffer_resource` writes `resource+0x88` to the returned memory and calls resource vtable slot 46 (`prepare`). It marks the pipe active only on true.
- `IOAccelResource2::map()` calls VidMemory `createMappingInTask` (slot 39). A NULL result cannot work.
- `IOAccelMemoryMap` supports a fixed-address mapping. Its physical flag bypasses the generic page-table commit path, and its GPU virtual address accessor returns the fixed physical segment.
- `IOAccelVidMemory::withPhysicalAddress` initializes a physical VidMemory and records its wired state. The current raw stand-in does not implement those semantics.

## Chosen Direction

Use real `Navi48VidMemory` and `Navi48MemoryMap` objects for the framebuffer resource. Initialize them in the family order, create a kernel-task map, reserve the fixed physical address, and bind the resource map association only after the entire association is usable. DisplayPipe's caller stores the returned memory at `resource+0x88`. Use a fixed GPU-visible framebuffer address, not a fabricated GPU page table or an unrelated successful `mm_hook`.

This is an experiment only until its exact behavior is verified on 25G241. Do not enable the older DisplayPipeGuard Route A, which substitutes a different stand-in and suppresses `IOAccelResource2::prepare`.

## Non-goals

- No change to Core Animation callback behavior, RADV, shader translation, or application argument-buffer claims.
- No blanket acceptance of arbitrary memory mappings or fake VidMemory types.
- No claim that boot-time process survival is a working desktop.
- No change to the actual framebuffer allocation or ownership of shared scanout memory.

## Invariants

1. Every returned framebuffer memory is a real VidMemory initialized by Apple's physical-address factory against the accelerator. That factory intentionally passes null shared/resource arguments; the display resource stores the returned object afterward.
2. A created mapping is a real `IOAccelMemoryMap` initialized against the same VidMemory, with fixed-address reservation and correct byte length.
3. Any failure before publication releases the objects in reverse order and returns the original failure, not a partially initialized resource.
4. Unmap/destruction releases exactly what the association owns and never frees shared scanout bytes.
5. The actual 25G241 binaries are authoritative for slot indices and return semantics; older Route A comments are not.

## Validation

- Pure host tests cover initialization order, fixed address/length, failure cleanup, and no unrelated `mm_hook` success.
- Aux layout gates and native display tests cover class vtables and binding behavior.
- Exact target kernel evidence verifies the current system ABI.
- Login-session evidence proves whether the real DisplayPipe prepares, reaches `GetMTLTexture`, and advances production presents. The broader goal still needs visible desktop correctness and application Metal coverage. Boot-time capture is optional; it must not be installed or started without an explicit user request.
