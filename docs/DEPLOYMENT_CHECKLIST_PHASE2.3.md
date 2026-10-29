# 黑苹果部署清单 - Phase 2.3

## 📋 文件清单（需要传输到黑苹果）

### 1. RDNA4Driver（已修改，包含 Phase 2.3）
```
RDNA4Driver/
├── src/
│   ├── core/
│   │   ├── RDNA4Driver.hpp     ✏️ 已修改（添加 ComputeRing 成员和公共接口）
│   │   └── RDNA4Driver.cpp     ✏️ 已修改（添加 Phase 2.3 方法）
│   └── hardware/
│       ├── ComputeRing.hpp     🆕 新文件
│       └── ComputeRing.cpp     🆕 新文件
├── Makefile                    ✏️ 已修改（添加 ComputeRing.cpp）
└── Info.plist                  ⚪ 未修改
```

### 2. RDNA4Accel（已修复 IOAccelerator 继承）
```
RDNA4Accel/
├── src/
│   ├── RDNA4Accel.hpp          ✏️ 已修改（继承 IOAccelerator）
│   └── RDNA4Accel.cpp          ✏️ 已修改（更新元类定义）
├── Makefile                    ⚪ 未修改
├── Info.plist                  ⚪ 未修改
└── REBUILD_NOTES.md            🆕 新文件（重编译说明）
```

---

## 🚀 部署步骤

### 步骤 1: 传输文件到黑苹果

**方式 A**: 使用 Git（推荐）
```bash
# 在开发 Mac 上提交
git add -A
git commit -m "Phase 2.3: Add ComputeRing and first compute kernel framework"
git push

# 在黑苹果上拉取
ssh user@hackintosh
cd ~/path/to/hackintosh
git pull
```

**方式 B**: 使用 SCP
```bash
# 打包文件
tar czf phase2.3.tar.gz RDNA4Driver/ RDNA4Accel/ docs/

# 传输到黑苹果
scp phase2.3.tar.gz user@hackintosh:/tmp/

# 在黑苹果上解压
ssh user@hackintosh
cd ~/hackintosh-project
tar xzf /tmp/phase2.3.tar.gz
```

### 步骤 2: 编译 RDNA4Driver

```bash
cd RDNA4Driver

# 清理旧文件
make clean

# 编译新版本
make

# 检查编译产物
ls -lh build/RDNA4Driver.kext/Contents/MacOS/RDNA4Driver
```

**期望输出**: 编译成功，kext 大小应该比之前大一些（因为新增了 ComputeRing）

### 步骤 3: 编译 RDNA4Accel（可选，如果要修复 IOAccelerator）

```bash
cd RDNA4Accel

# 清理
make clean

# 编译
make

# 检查
ls -lh build/RDNA4Accel.kext/Contents/MacOS/RDNA4Accel
```

### 步骤 4: 备份当前配置

```bash
# 备份当前稳定的驱动
sudo cp -R /Library/Extensions/RDNA4Driver.kext \
    /Library/Extensions/RDNA4Driver.kext.backup-$(date +%Y%m%d-%H%M%S)

# 备份 EFI（如果通过 OpenCore 注入）
cp -R /Volumes/EFI/OC/Kexts/RDNA4Driver.kext \
    /Volumes/EFI/OC/Kexts/RDNA4Driver.kext.backup-$(date +%Y%m%d-%H%M%S)
```

### 步骤 5: 部署新驱动

**方式 A**: 系统目录安装
```bash
# 卸载旧驱动
sudo kextunload /Library/Extensions/RDNA4Driver.kext

# 删除旧驱动
sudo rm -rf /Library/Extensions/RDNA4Driver.kext

# 安装新驱动
sudo cp -R build/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext

# 重建缓存
sudo kextcache -i /

# 手动加载测试（可选）
sudo kextload /Library/Extensions/RDNA4Driver.kext
```

**方式 B**: OpenCore 注入（推荐）
```bash
# 删除旧的
rm -rf /Volumes/EFI/OC/Kexts/RDNA4Driver.kext

# 拷贝新的
cp -R build/RDNA4Driver.kext /Volumes/EFI/OC/Kexts/

# 重启
sudo reboot
```

### 步骤 6: 重启验证

```bash
sudo reboot
```

---

## ✅ 验证清单

重启后，依次检查：

### 1. 驱动加载状态
```bash
kextstat | grep rdna4
```
期望输出：
```
  XXX  0  com.rdna4.driver (0.2.0)
  YYY  0  com.rdna4.accel (1.0.0)
```

### 2. 硬件初始化（四档全通）
```bash
ioreg -l -n RDNA4Driver | grep -E "PSP,Stage|SMU,Stage|GFX,Stage|ComputeRing"
```
期望输出：
```
"PSP,Stage" = "alive"
"SMU,Stage" = "alive"
"GFX,Stage" = "gfx-alive"
"ComputeRing,Initialized" = Yes
```

### 3. 系统日志
```bash
sudo dmesg | grep -A5 "ComputeRing: init"
```
期望输出：
```
ComputeRing: init() - vramBase=0x..., vramSize=..., doorbellIndex=0
ComputeRing: initialized - ring at VRAM+0x4000000, fence at VRAM+0x4400000
RDNA4: Compute Ring initialized successfully
```

### 4. 无错误日志
```bash
sudo dmesg | grep -i "error\|failed\|panic" | grep -i rdna4
```
期望：无相关错误（或仅有非致命警告）

### 5. SSH 保持可用
```bash
# 从开发 Mac 测试
ssh user@hackintosh "echo SSH OK"
```

---

## 🐛 故障排查

### 编译失败

**问题**: `fatal error: 'IOKit/graphics/IOAccelerator.h' file not found`

**原因**: KDK 版本不匹配或未安装

**解决**:
```bash
# 检查 KDK
ls -la /Library/Developer/ | grep Kernel

# 如果缺少，暂时注释掉 IOAccelerator
# 在 RDNA4Accel.hpp 中：
// #include <IOKit/graphics/IOAccelerator.h>
// class RDNA4Accel : public IOService {  // 暂时用 IOService
```

### 驱动加载失败

**问题**: `kextload` 失败，提示依赖问题

**原因**: OSBundleLibraries 版本不匹配

**解决**:
```bash
# 查看详细错误
sudo kextutil -t build/RDNA4Driver.kext

# 如果版本问题，降低 Info.plist 中的要求版本
```

### 启动后卡死

**问题**: 重启后黑屏或卡在启动画面

**原因**: ComputeRing 初始化触发了硬件问题

**恢复**:
```bash
# 进入 Recovery 模式
# 删除新驱动，恢复备份
rm -rf /Volumes/Macintosh\ HD/Library/Extensions/RDNA4Driver.kext
cp -R /Volumes/Macintosh\ HD/Library/Extensions/RDNA4Driver.kext.backup-* \
    /Volumes/Macintosh\ HD/Library/Extensions/RDNA4Driver.kext
```

### ComputeRing 未初始化

**日志**: `ComputeRing: init() - ...` 未出现

**原因**: GFX 未达到 alive 状态

**检查**:
```bash
ioreg -l -n RDNA4Driver | grep "GFX,Stage"
```

如果不是 `gfx-alive`，说明 GFX 初始化有问题（与 Phase 2.3 无关）

---

## 📊 成功标准

全部以下条件满足即视为 Phase 2.3 框架部署成功：

- ✅ 系统正常启动，无 panic
- ✅ RDNA4Driver 0.2.0 加载成功
- ✅ PSP/SMU/GFX 四档全部 alive
- ✅ `ComputeRing,Initialized = Yes`
- ✅ 系统日志显示 ring 分配在 VRAM+0x4000000
- ✅ 系统日志显示 fence 分配在 VRAM+0x4400000
- ✅ SSH 保持可用

达到以上标准后，可以继续进行 **Phase 2.3 下一步**：集成 air2gfx12 编译真实 shader。

---

*部署清单创建于 2026-10-29*
