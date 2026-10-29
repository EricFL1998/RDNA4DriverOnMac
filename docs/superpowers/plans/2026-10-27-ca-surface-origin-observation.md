# CA Surface Origin Observation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Identify the Core Animation surface-creation path that returns the untracked surface before the automatic-login WindowServer assertion, without changing rendering behavior.

**Architecture:** Observe the real QuartzCore vtable methods after their original implementations return. Record creation kind, exact input arguments, return value, and the existing tracking state; include NULL returns. Preserve the existing observe-only assertion trampoline so WindowServer retains Apple's original failure behavior.

**Tech Stack:** Objective-C, C, x86_64 SysV, macOS QuartzCore 25G241, shell/Python tests, SSH deployment.

**Spec:** The user's continuing investigation and the reproduced `push_surface` assertion; target evidence is `diagnostics/n48-auto-test/reboot-20261027-003758`.

## Global Constraints

- Do not alter a returned surface, register/unregister surfaces, call private CA methods, or skip Apple's assertion.
- Do not reintroduce dummy IOSurface, CARenderOGLRender suppression, or automatic `track_surface`.
- Correct vptr offsets: `image_surface=+0x298`, `create_surface_from_iosurface=+0x2d8`, `create_reinterpreted_surface=+0x2e8`.
- The static MetalContext vtable address point is `CARenderOGLNew_ + 0x33ac6dd2`.
- Automatic login remains enabled; no password in files or logs.
- Every deployment is followed by a full reboot and automatic-login report.

## Review Focus

- A NULL return must be recorded rather than mistaken for an unobserved call; covered by the image, IOSurface, and reinterpreted-surface tests.
- The observer must not mutate tracking state or return a substitute object; covered by exact return and unchanged-memory assertions.
- The slot guard must verify all original targets before changing any vtable entries; covered by executable fixture tests.
- Reports must include evidence even if WindowServer dies immediately after automatic login; copy the durable UID-88 log during verification.

---

### Task 1: Read-Only Surface Observation

**Files:**
- Modify: `navi48/Cobalt64-main/tools/native/navi48metal/n48_ca_image_probe.h`
- Modify: `navi48/Cobalt64-main/tools/native/navi48metal/n48_ca_probe.h`
- Modify: `navi48/Cobalt64-main/tools/native/navi48metal/Navi48Device.m`
- Test: `navi48/Cobalt64-main/tools/native/navi48metal/test-ca-image-probe.c`
- Test: `navi48/Cobalt64-main/tools/native/navi48metal/test-ca-trampoline.sh`

**Interfaces:**
- Produces: read-only wrappers for the three real CA functions, using `void *(*)(void *context, ...)` original calls and reporting their unchanged return values.
- Produces: `n48-ca-origin-<uid>.log`, containing creation kind, arguments, returned surface, and bounded tracking-list observations.

- [ ] **Step 1: Write failing tests**

Add exact-argument tests for successful and NULL-returning `image_surface`, `create_surface_from_iosurface`, and `create_reinterpreted_surface`. Assert that the original is called once, its return is unchanged, the callback observes NULL, and no tracking-list bytes are changed. Add a durable-log round trip for all three kinds.

- [ ] **Step 2: Verify tests fail for the missing behavior**

Run `test-ca-image-probe.c` and the existing trampoline/probe tests. Expected: compilation or assertions fail because reinterpreted-surface observation and NULL-origin recording do not exist.

- [ ] **Step 3: Implement the minimal observation**

Invoke the original function before recording. Validate all three expected vtable targets before patching any slot. Install only in `N48_CA_OBSERVE_ONLY=1` x86_64 builds. Log bounded surface flags, width/height, links, and both tracking heads. Use only the known `CARenderOGLNew_` deltas; do not derive behavior from a guessed layout.

- [ ] **Step 4: Verify tests and build**

Run `test-ca-image-probe.c`, `test-ca-probe.c`, `test-ca-trampoline.sh`, and `tests/n48-auto-test/test-static.sh`; then build with `N48_CA_OBSERVE_ONLY=1` and the configured RADV/translator libraries. Expected: all tests and x86_64/arm64 compilation pass.

- [ ] **Step 5: Preserve local evidence**

Save test/build logs and the source-to-binary deployment checksum in `diagnostics/n48-auto-test/`. This is not a Git repository; do not commit, reset, or replace unrelated source snapshots.

### Task 2: Automatic-Login Origin Capture

**Files:**
- Modify: `tools/n48-auto-test/n48-verify-remote.sh`
- Test: `tests/n48-auto-test/test-static.sh`

**Interfaces:**
- Consumes: durable CA origin records from Task 1.
- Produces: `ca-origin.log` in each collected report alongside the existing WindowServer crash report.

- [ ] **Step 1: Write the failing collection test**

Exercise the verifier against a controlled report directory containing a synthetic UID-88 origin log. Assert that it archives the contents read-only and leaves the source untouched.

- [ ] **Step 2: Verify failure and implement**

Run the new collection test before implementation, then add bounded, regular-file-only copying of the origin log to the verifier. Keep collection independent of the pass/fail result.

- [ ] **Step 3: Deploy, reboot, and collect**

Run `deploy-optimized-driver.sh --reboot` from the project root. Expected: automatic login occurs and `ca-origin.log` plus a per-boot crash report are collected.

- [ ] **Step 4: Trace one failing surface**

Match the assertion's context, dimensions, flags, and return pointer to an origin record. If no origin matches, distinguish NULL/unobserved creation from tracking removal before proposing another experiment. Do not claim a driver fix until Dock and Finder survive and no new WindowServer crash appears.

### Task 3: Actual SWContext Surface Lifecycle

**Files:**
- Modify: `navi48/Cobalt64-main/tools/native/navi48metal/n48_ca_probe.h`
- Modify: `navi48/Cobalt64-main/tools/native/navi48metal/Navi48Device.m`
- Test: `navi48/Cobalt64-main/tools/native/navi48metal/test-ca-image-probe.c`

**Interfaces:**
- Consumes: Task 1's read-only observer wrappers and bounded durable record.
- Produces: installation result per real vtable and pre-/post-call state for SW `create_surface_from_iosurface` and `finalize_surface`.

- [ ] **Step 1: Test lifecycle reporting and pre-call snapshot**

Assert that a post-call record includes the captured pre-call `found` value and the subsequent value, and that release observation retains the original call and its arguments.

- [ ] **Step 2: Verify RED, then implement**

Add `pre_found` and the lifecycle wrapper only after the test fails for their absence. Install into the actual SW vtable at `CARenderOGLNew_ + 0x33ad2d22`, `create_surface_from_iosurface +0x2d8`, `finalize_surface +0x320`. Do not change the return value or list.

- [ ] **Step 3: Reboot and match the failing surface**

Archive previous probe logs before deployment. Verify successful slot installation in unified logs and correlate a single failing pointer with its creation and any release. This experiment tests the observed SW IOSurface creation/tracking hypothesis; it does not modify render policy.
