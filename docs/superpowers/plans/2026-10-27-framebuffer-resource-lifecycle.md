# Framebuffer Resource Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the native Navi48 display pipe create a real framebuffer `IOAccelResource2`/VidMemory/MemoryMap association so `IOAccelDisplayPipe::init_framebuffer_resource()` can prepare the display without bypassing its checks.

**Architecture:** Use the aux accelerator's real `Navi48VidMemory` and `Navi48MemoryMap` classes, not a raw stand-in object. Bind the VidMemory to the framebuffer resource, create a fixed-address kernel mapping, and preserve the exact resource/physical-segment lifecycle. The display-specific mapping is a fixed GPU-visible address; other Metal textures and mappings keep their existing behavior.

**Tech Stack:** C++17 IOKit, existing Navi48Accel aux kext, native_disp display hook, host-side pure semantic tests, extracted 25G83/25G241 kernel evidence, automatic-login deployment.

**Spec:** `docs/superpowers/specs/2026-10-27-framebuffer-resource-lifecycle-design.md`; target failure `diagnostics/n48-auto-test/check-20261027-080213/timeline.log` (`init_framebuffer_resource ... failed to prepare framebuffer resource`).

## Global Constraints

- Do not bypass `IOAccelDisplayPipe::init_framebuffer_resource`, return an untyped stand-in, force its active bit to fake success, or suppress its later present assertions.
- Preserve the proven CA callback-return observer and RADV/translator libraries; no rendering behavior changes in `Navi48Device.m`.
- Use real resource objects and balanced retain/release behavior. No dummy IOSurface, no fake `IOAccelResource2`, no global `mm_hook` success for unrelated mappings.
- Do not call AMD's `reserveFrameBuffer` or enable the unverified old DisplayPipeGuard Route A experiment.
- Do not change the automatic-login configuration or deploy a boot recorder. Deploy only after tests pass and with explicit user approval. A passing boot recorder, process survival, or Metal smoke is not full desktop/Metal validation.
- No Git repository exists: preserve existing experimental source files and do not initialize/reset/commit a repository.

## Review Focus

- `VidMemory::createMappingInTask()` must create and initialize a real map, not return NULL or an arbitrary pointer.
- The mapping must retain the VidMemory and represent the actual GPU-visible framebuffer bytes, with correct length and fixed GPU address semantics.
- `resource+0x88` and `resource+0x70` must refer to the correct memory, not `IOAccelKernelMappedMemory` substituted for a VidMemory.
- Destroy/unmap must release the map and resource association once, without freeing the shared scanout allocation or changing unrelated texture behavior.
- The 25G241 slot semantics must be verified from the actual system collection, not guessed from Route A comments.

---

### Task 1: Fixed-Address Mapping Semantics

**Files:**
- Create: `navi48/Cobalt64-main/tools/native/navi48accel/src/n48_framebuffer_memory.h`
- Modify: `navi48/Cobalt64-main/tools/native/navi48accel/src/Navi48Accel.cpp`
- Test: `navi48/Cobalt64-main/tools/native/navi48accel/tests/framebuffer_memory_test.cpp`

**Interfaces:**
- Produces: a pure decision describing the required init/map/reserve sequence and failure cleanup for the framebuffer VidMemory.
- Consumes: existing `Navi48Accelerator::newVidMemory()` and `Navi48MemoryMap` initialization.

- [ ] **Step 1: Write the failing lifecycle test**

Assert init precedes `createMappingInTask`, map initialization precedes fixed-address reservation, and the reserved GPU address matches the fixed physical segment. Assert init/allocation failure releases created objects once and does not call reserve.

- [ ] **Step 2: Verify RED**

Run `clang++ ... framebuffer_memory_test.cpp`. Expected: missing helper.

- [ ] **Step 3: Implement the minimal helper and aux wiring**

Add only the framebuffer-scoped real-object wiring needed to make the test pass. Do not add a global map-success hook or alter `Navi48MemoryMap::commitIntoGPUPageTable` for other mappings.

- [ ] **Step 4: Verify GREEN and aux host suite**

Run the new test, `make -C tests all`, and `make` with the preserved generated layout inputs. Expected: lifecycle and layout gates pass; no new unsupported override is silently accepted.

### Task 2: Display-Pipe Resource Binding and Evidence

**Files:**
- Modify: `navi48/Cobalt64-main/src/navi48-bringup/src/amd/native_disp.cpp`
- Modify: `navi48/Cobalt64-main/src/navi48-bringup/src/amd/native_disp_flow.h`
- Modify: `navi48/Cobalt64-main/src/navi48-bringup/src/amd/native_disp_pure.h`
- Modify: `navi48/Cobalt64-main/src/navi48-bringup/tests/native_disp_test.cpp`
- Modify: `tools/n48-auto-test/n48-boot-recorder.sh`
- Modify: `tools/n48-auto-test/com.navi48.autotest.plist`
- Modify: `tools/n48-auto-test/n48-boot-report.sh`
- Test: `tests/n48-auto-test/test-boot-recorder.sh`

**Interfaces:**
- Consumes: the real aux framebuffer mapping created in Task 1.
- Produces: `resource+0x88` bound to the real VidMemory; boot evidence distinguishing native prepare failure, DisplayPipe transactions, and production presents.

- [ ] **Step 1: Write failing binding and recorder tests**

Require the display hook to use the real VidMemory and preserve its owner on failure. Require boot tools and LaunchDaemon to use `/var/db/navi48-auto-test/tools`, never the cleaned `/tmp/n48-auto-test` scripts.

- [ ] **Step 2: Verify RED**

Run `native_disp_test.cpp` and `test-boot-recorder.sh`. Expected: binding absent and plist/tmp path mismatch.

- [ ] **Step 3: Implement binding and durable recorder paths**

Replace the stand-in return with the real object only after init/map succeed; retain the original failure return otherwise. Move recorder scripts and `navi48test` into a persistent tools directory. Record `IOAccelDisplayPipe` prepare errors and present counters in boot evidence.

- [ ] **Step 4: Verify host and recorder suites**

Run `native_disp_test.cpp`, `tests/n48-auto-test/test-*.sh`, and the aux tests. Expected: all pass, no fake resource or global success behavior.

### Task 3: Target and Production Validation

**Files:**
- Evidence: `diagnostics/n48-auto-test/framebuffer-resource-20261027/evidence.md`

- [ ] **Step 1: Check exact 25G241 binary semantics**

Extract the target's actual SystemKernelExtensions and `IOAcceleratorFamily2`; compare the slot signatures and init/map/prepare disassembly to the 25G83 evidence.

- [ ] **Step 2: Build and install the aux candidate**

Preserve the currently working `Navi48Metal.bundle` and RADV libraries. Build only the aux/kernel candidate, verify hashes, and follow the existing approved aux installation procedure rather than modifying SIP/security policy.

- [ ] **Step 3: Reboot and collect the exact new boot**

Confirm `kind=metal` CA context remains correct, no new `init_framebuffer_resource` failure occurs, `GetMTLTexture` is reached, and display transaction/present counters advance.

- [ ] **Step 4: Validate the complete goal**

Require visible correct desktop, sustained production CoreDisplay GPUPass/scanout, no GPU fault, no new WindowServer/Dock/Finder crash, and application Metal coverage. If any requirement is unverified, keep the full RDNA4/Metal objective active.
