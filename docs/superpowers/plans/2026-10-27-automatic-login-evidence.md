# Automatic Login Evidence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve the automatic-login failure evidence at boot and automatically retrieve it after each driver deployment, without using a visible verbose screen as the crash record.

**Architecture:** Reuse the existing remote verifier for crash reports, unified logs, and GPU evidence. Add a root LaunchDaemon that invokes that verifier after boot, writes a boot-specific persistent report, and never restarts WindowServer or loginwindow. Extend the existing deployment/check scripts to install and retrieve this evidence over the existing SSH channel.

**Tech Stack:** POSIX shell, macOS launchd plist, Python test fixtures, OpenSSH, existing Navi48Metal deployment scripts.

**Spec:** The user's explicit request to keep automatic login enabled, reboot after each GPU-driver update, and collect the resulting logs; the existing non-Git workspace and its automatic-login evidence requirements.

## Global Constraints

- Keep automatic login enabled; never store or request the user's login password in scripts or logs.
- Do not restart or kill WindowServer, Dock, Finder, or loginwindow to make the test run.
- Every driver installation requires a normal reboot followed by collection; never claim a live process or Metal smoke test is a correct desktop.
- Preserve all existing experimental sources and dependency libraries; this task changes evidence collection, not rendering behavior.
- This workspace has no Git repository: do not initialize one, reset, commit, or replace the user's experimental snapshots.
- Do not change VPN, routing, or other host network settings to recover SSH.

## Review Focus

- An SSH failure must be reported as unavailable, not an empty successful report.
- A second verifier run must not mix boots or misattribute an old crash to the current one.
- A root recorder must still run when no user has logged in and must exit rather than leave a recurring Metal-test process.
- An oversized probe or report directory must not grow without bounds or overwrite previous boots.
- A collection failure must not make a deployment reboot again or roll back an unrelated driver.

---

### Task 1: Persistent Boot Recorder

**Files:**
- Create: `tools/n48-auto-test/n48-boot-recorder.sh`
- Create: `tools/n48-auto-test/com.navi48.autotest.plist`
- Test: `tests/n48-auto-test/test-boot-recorder.sh`

**Interfaces:**
- Produces: `/var/db/navi48-auto-test/<boot_epoch>/`, including `environment.txt`, `result.txt`, `result.json`, `unified.log`, CA probe logs, and current-boot `.ips` reports.
- Consumes: the existing `/tmp/n48-auto-test/verify.sh` and `/tmp/n48-auto-test/boot-evidence.py`.

- [ ] **Step 1: Write a failing end-to-end recorder fixture**

Run the real recorder with temporary, explicitly named `N48_*_ROOT` paths and a fixture verifier. Assert that a boot-specific report is created, completion and exit code are recorded, the verifier runs once, and its nonzero test result does not become a successful report. Assert no `killall` or reboot action occurs.

- [ ] **Step 2: Run the fixture before implementation**

Run `sh tests/n48-auto-test/test-boot-recorder.sh`. Expected: FAIL because the recorder is absent.

- [ ] **Step 3: Implement the bounded recorder and LaunchDaemon**

Create a per-boot root-owned directory, copy the previous verifier result rather than parsing it as success, and cap old reports by count without deleting the current one. Run once at boot with `RunAtLoad`, not `KeepAlive` or `StartInterval`; use the system Python 3 path selected on the target, not a Homebrew user-session dependency.

- [ ] **Step 4: Verify recorder behavior**

Run the fixture for successful and failing verifier results and two distinct boot epochs. Expected: separate reports, correct recorded exit codes, and no retained test process.

### Task 2: Deployment and Retrieval

**Files:**
- Modify: `deploy-optimized-driver.sh`
- Modify: `tools/n48-auto-test/n48-collect-reports.sh`
- Modify: `tools/n48-auto-test/config.sh`
- Test: `tests/n48-auto-test/test-auto-deploy.sh`

**Interfaces:**
- Consumes: Task 1's persistent per-boot reports and existing SSH functions.
- Produces: local `diagnostics/n48-auto-test/boot-YYYYMMDD-HHMMSS/` reports and the existing interactive check/reboot reports.

- [ ] **Step 1: Write failing deployment fixtures**

Use isolated temporary SSH/SCP command fixtures to exercise normal deployment and check paths. Assert that recorder installation precedes reboot, automatic-login status is collected, the exact new boot epoch is selected, and a connection failure neither creates a PASS nor triggers a retry reboot.

- [ ] **Step 2: Run the fixtures before implementation**

Run `sh tests/n48-auto-test/test-auto-deploy.sh`. Expected: FAIL because no boot recorder is installed or retrieved.

- [ ] **Step 3: Extend the existing scripts minimally**

Install the recorder with the verifier, keep the current executable checksum verification, and retrieve the persistent report alongside the interactive report. Preserve the current `--check` and `--reboot` interfaces. Do not suppress SSH errors or treat a missing report as successful evidence.

- [ ] **Step 4: Run all automated-test fixtures**

Run `tests/n48-auto-test/test-*.sh` and `sh -n` on every modified shell script. Expected: all tests pass, including the old CA evidence and boot-UUID tests.

### Task 3: Target Validation

**Files:**
- Evidence: `diagnostics/n48-auto-test/autologin-recorder-20261027/evidence.md`

- [ ] **Step 1: Restore the existing SSH path**

Read-only connection checks must establish the target's actual reachability before deployment. If SSH is unavailable, record the blocker; do not change VPN settings or call the test complete.

- [ ] **Step 2: Verify automatic login and recorder installation**

Confirm `sysadminctl -autologin status`, FileVault state, launchd exit status, and the installed driver hash. Record any failure explicitly.

- [ ] **Step 3: Reboot the current callback candidate once**

Run `deploy-optimized-driver.sh --reboot` after local tests pass and SSH is available. Retrieve both persistent and interactive evidence for the exact new boot. Inspect `ca-context.log`, production `GetMTLTexture`/present evidence, and current-boot crash stacks.

- [ ] **Step 4: Report the observed boundary**

Distinguish a working recorder from a working GPU driver. Only a real visible desktop with sustained production compositing/scanout can pass the broader Metal goal; preserve failures as actionable evidence rather than masking them.
