# RDNA4 Fix Documentation and Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 记录 RDNA4Driver 与 RDNA4FBMinimal 的真实修复过程，并将相关工作目录整理为只保留当前有效实现与构建产物。

**Architecture:** 用根目录最终状态报告作为唯一完整修复记录，更新活动 README/构建脚本中的 AuxKC 指引；过时诊断和实验产物移动到现有 archive 体系，不删除参考项目、备份或可用构建。

**Tech Stack:** Markdown、shell、Make、macOS `plutil`、x86_64 kext 构建。

**Spec:** `RDNA4_STATUS_REPORT.md` 的原始故障描述，以及本次目标机最终启动日志和 AuxKC 验证结果。

## Global Constraints

- 不修改 `navi48/Cobalt64-main`、`/tmp/RDNA4FB` 或 `/tmp/nvidia-macos-driver`。
- 不删除当前源码、`build/RDNA4Driver.kext`、`build/RDNA4FBMinimal.kext` 或目标机备份。
- 历史文件优先归档，只有确认为空的目录才用 `rmdir` 清理。
- 最终文档必须区分“加载/匹配/BAR/framebuffer 已验证”与“Metal/GPU 加速尚未完成”。

## Review Focus

- 不得再把 macOS 26.7.1 写成 Sequoia 15.7；Task 1 检查最终报告的系统版本。
- 不得再把 OpenCore 注入描述为完全失败；Task 1 记录注入成功但匹配失败的证据。
- 不得让活动文档继续推荐 `kextcache` 或 OpenCore 注入；Task 2 检查 README、Makefile 和 check_build.sh。
- 不得把 `0x1A18 = 0xffffffff` 隐去或解释为加速成功；Task 1 将其列为后续硬件问题。
- 归档不得覆盖已有文件；Task 2 在移动前检查目标路径。

---

### Task 1: 最终修复报告

**Files:**
- Modify: `RDNA4_STATUS_REPORT.md`

**Interfaces:**
- Consumes: 最终加载 UUID `E5CC5957-17AB-39EE-9C76-71DABEB6EF30`、`1715CA73-3469-3339-A12B-354760D27045`。
- Produces: 一份包含根因、修复步骤、最终配置、验证证据、备份和剩余问题的中文报告。

- [ ] **Step 1: 重写状态报告**
  记录 OpenCore 注入成功但 probe 未调用、`IOPropertyMatch` 类型错误、framebuffer 无效显示契约、AuxKC 切换、正确设备 ID，以及两个驱动最终同时启动的证据。

- [ ] **Step 2: 检查报告结论**
  确认报告明确写出 macOS 26.7.1、AuxKC 最终架构、WindowServer 验证，以及 `0x1A18` 读到 `0xffffffff` 和 Metal 尚未完成。

### Task 2: 活动指引与历史归档

**Files:**
- Modify: `RDNA4Driver/README.md`
- Modify: `RDNA4Driver/Makefile`
- Modify: `RDNA4Driver/check_build.sh`
- Move: 9 个过时 Markdown 文件到 `archive/old-docs/RDNA4Driver/`
- Move: `Info.plist.test`、`test_minimal.cpp`、`try_host_make.sh`、`verify_dual_personality.sh` 到 `archive/old-scripts/RDNA4Driver/`
- Move: `RDNA4Driver/dist`、`RDNA4Driver/RDNA4Driver` 到 `archive/old-artifacts/RDNA4Driver/`

**Interfaces:**
- Consumes: 当前双 AuxKC 部署方式和 `kmutil showloaded` 验证方式。
- Produces: 不再包含旧 `kextcache`/OpenCore 注入指引的活动工作目录。

- [ ] **Step 1: 更新活动指引**
  README 改为简洁的当前功能、构建、AuxKC 安装和验证说明；Makefile 用 `kmutil load/unload/showloaded` 替换旧命令并移除 `kextcache` 目标；check_build.sh 同步后续步骤。

- [ ] **Step 2: 验证脚本与 plist**
  运行 `make -n install`、`make -n unload`、`make -n status`，并用 `plutil -lint` 检查两个 Info.plist。

- [ ] **Step 3: 归档历史文件**
  先确认归档目标不存在，再移动列出的文档、实验文件和旧目录；仅对空目录执行 `rmdir`。

### Task 3: 最终验证

**Files:**
- Verify: 两个项目及最终报告。

**Interfaces:**
- Consumes: Task 1 和 Task 2 的全部变更。
- Produces: 可复现的干净构建和清晰的工作区结果。

- [ ] **Step 1: 干净构建**
  分别在两个项目运行 `make clean && make`，要求退出码 0；保留 RDNA4Driver 已有的两个编译警告记录。

- [ ] **Step 2: 检查交付物**
  用 `dwarfdump --uuid` 确认驱动 UUID 仍为 `E5CC5957-…`、framebuffer UUID 仍为 `1715CA73-…`，并确认最终报告存在、归档文件就位、活动目录不含列出的旧实验文件。
