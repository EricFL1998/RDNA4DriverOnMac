# 黑苹果启动故障恢复指南

**记录时间**: 2026-10-28  
**故障现象**: 引导后内核日志停在 `X86PlatformPlugin` 之后，出现
`Kext com.apple.driver.IOVersatile - can't resolve dependencies; OSBundleLibraries missing/invalid type`，
随后卡死（X86P_ 处）。

> 说明：`IOVersatile` 依赖错误在本工程历史记录中已多次出现（见 `archive/旧文档`），
> 它本身通常**不是致命错误**；真正导致卡死的是内核扩展依赖链/AuxKC 状态。
> 本次卡死发生在 `rdna4=1 rdna4-gfx=1` 全量 GPU 初始化配置下。

---

## 一、已完成的清理（U 盘 EFI，`/Volumes/1401/EFI/OC`）

| 项目 | 操作 |
|---|---|
| `Kexts/Navi48Bringup.kext` | **已删除**（备份：`/tmp/Navi48Bringup.kext.bak`） |
| `Kexts/RDNA4Driver.kext` | 移至 `_disabled-rdna4-kexts/` |
| `Kexts/RDNA4FBMinimal.kext` | 移至 `_disabled-rdna4-kexts/` |
| `Kexts/TestMinimal.kext` | 移至 `_disabled-rdna4-kexts/` |
| `config.plist` 的 `Kernel/Add` | 移除全部 RDNA4/Navi48/TestMinimal 条目 |
| `config.plist` 的 boot-args | 移除 `rdna4=*`、`navi48-*` 全部参数 |

当前 boot-args：
```
-v debug=0x100 keepsyms=1 -amfipassbeta ctrsmt=full revpatch=sbvmm amfi_get_out_of_my_way=1
```
`plutil -lint` 校验通过。

保留 `Kernel/Block`（阻断 Apple 原生 AMD 驱动误匹配 RDNA4 显卡）：
- `com.apple.kext.AMDRadeonX6000`
- `com.apple.kext.AMDSupport`
- `com.apple.kext.AMDRadeonX6000HWServices`

---

## 二、仍存在于 macOS 系统盘的风险点

U 盘 EFI 无法控制系统盘中的以下内容，需在系统内处理：

1. `/Library/Extensions/RDNA4Driver.kext`
   - 该驱动 `probe()` 要求 boot-arg `rdna4=1`；**当前 boot-args 已无该参数，因此不会 attach**，属于惰性状态，无需立即删除。
2. `/Library/Extensions/RDNA4FBMinimal.kext`
   - 无 boot-arg 门控，会无条件 attach 并提供控制台 framebuffer。
   - **它是当前显示器点亮的唯一来源**（Apple 原生 AMD 驱动被 Block），暂不要删。
   - 如需临时禁用，可加 boot-arg `rdna4fb-off=1`（但会导致无显示，仅用于 SSH 排障）。
3. AuxKC（Auxiliary Kernel Collection）可能残留旧注入项
   - 若启动仍失败，需要在 Recovery 中重建。

---

## 三、恢复步骤

### 步骤 A：先用干净 U 盘 EFI 启动

1. 插入 U 盘，进入 OpenCore 菜单，选择 macOS 启动项。
2. 观察是否有改善：
   - 能进登录界面 → 进入步骤 B 清理系统盘。
   - 仍卡住 → 进入步骤 C（Recovery）。

### 步骤 B：进入系统后清理（推荐）

```bash
# 1. 查看当前残留
ls -la /Library/Extensions/ | grep -iE "rdna4|navi48|testminimal"

# 2. 移除除 RDNA4FBMinimal 之外的所有实验 kext（RDNA4FBMinimal 保留用于显示）
sudo rm -rf /Library/Extensions/RDNA4Driver.kext
sudo rm -rf /Library/Extensions/TestMinimal.kext
sudo rm -rf /Library/Extensions/Navi48Accel.kext
sudo rm -rf /Library/Extensions/Navi48Bringup.kext
sudo rm -rf /Library/Extensions/Navi48DisplayNub.kext

# 3. 重建 AuxKC / 内核缓存
sudo kmutil install --update-all
sudo kcditto

# 4. 确认 RDNA4Driver 未加载
kmutil showloaded | grep -iE "rdna4|navi48"
# 期望：只剩下 com.rdna4.framebuffer.minimal（若保留）

# 5. 重启
sudo reboot
```

### 步骤 C：无法进入系统时（Recovery）

1. OpenCore 菜单选择 **Recovery**。
2. 打开终端，定位系统卷：
```bash
diskutil list
# 找到 macOS 数据卷，通常是 diskXs1，挂载为 /Volumes/你的卷名
```
3. 删除实验 kext：
```bash
ls -la "/Volumes/<卷名>/Library/Extensions/" | grep -iE "rdna4|navi48|testminimal"
rm -rf "/Volumes/<卷名>/Library/Extensions/RDNA4Driver.kext"
rm -rf "/Volumes/<卷名>/Library/Extensions/Navi48Accel.kext"
# RDNA4FBMinimal 视情况保留（无显示时可临时保留）
```
4. 重建 AuxKC：
```bash
kmutil install --volume-root "/Volumes/<卷名>" --update-all
```
5. 重启回正常系统。

---

## 四、后续排查顺序（慢慢修）

1. **先确认纯净启动可用**（无任何本项目 kext 的注入，系统能进桌面）。
2. 再逐个恢复：
   - 第一步只放回 `RDNA4FBMinimal.kext`（验证显示）。
   - 第二步只放回 `RDNA4Driver.kext`，boot-args 仅加 `rdna4=1`（不做 PSP/SMU/GFX 初始化）验证识别。
   - 第三步再逐步加 `rdna4-psp=1`、`rdna4-smu=1`，每加一步独立验证。
   - **最后**才加 `rdna4-gfx=1`（这是本次故障前的新增项，风险最高）。
3. 每次改动前备份 `config.plist` 与 `/Library/Extensions`。
4. 保持 SSH 可用，避免完全依赖屏幕排障。

---

## 五、已备份文件位置

- U 盘 config 备份：`/Volumes/1401/EFI/OC/config.plist.backup-*`（多份）
- Navi48Bringup.kext 备份：`/tmp/Navi48Bringup.kext.bak`
- 被禁用的 kext：`/Volumes/1401/EFI/OC/_disabled-rdna4-kexts/`
