# Phase 1.2 集成指南

## 📋 修改总结

### 新增文件 (2)
```
src/hardware/
├── IPDiscovery.hpp    (新增) - IP Discovery 类定义
└── IPDiscovery.cpp    (新增) - IP Discovery 实现
```

### 修改文件 (3)
```
src/core/
├── RDNA4Driver.hpp    (修改) - 添加 IPDiscovery* 成员
└── RDNA4Driver.cpp    (修改) - 集成 IP Discovery 调用

Makefile               (修改) - 添加 IPDiscovery.cpp 编译
```

## 🔧 代码变更详情

### 1. RDNA4Driver.hpp

**添加前置声明**（文件末尾，#endif 之前）：
```cpp
class IPDiscovery;  // Forward declaration
```

**添加私有成员**（private 部分）：
```cpp
// Phase 1.2: IP Discovery
IPDiscovery* ipDiscovery;
bool performIPDiscovery();
```

### 2. RDNA4Driver.cpp

**添加头文件**（文件开头）：
```cpp
#include "../hardware/IPDiscovery.hpp"
```

**初始化成员**（start() 方法中）：
```cpp
ipDiscovery = nullptr;
```

**调用 IP Discovery**（start() 方法，probeGPU() 之后）：
```cpp
// Phase 1.2: IP Discovery
if (!performIPDiscovery()) {
    LOG_ERROR("IP Discovery failed");
    return false;
}
```

**清理资源**（stop() 方法）：
```cpp
// Phase 1.2: 清理 IP Discovery
if (ipDiscovery) {
    delete ipDiscovery;
    ipDiscovery = nullptr;
}
```

**添加实现方法**（文件末尾）：
```cpp
//==============================================================================
// Phase 1.2: IP Discovery
//==============================================================================

bool RDNA4Driver::performIPDiscovery() {
    LOG_DEBUG(1, "Performing IP Discovery...");
    
    ipDiscovery = new IPDiscovery();
    if (!ipDiscovery) {
        LOG_ERROR("Failed to allocate IPDiscovery");
        return false;
    }
    
    if (!ipDiscovery->discover(rmmio, rmmioSize)) {
        LOG_ERROR("IP Discovery failed");
        delete ipDiscovery;
        ipDiscovery = nullptr;
        return false;
    }
    
    if (!ipDiscovery->hasBlock(IPBlockType::GC)) {
        LOG_ERROR("Graphics Compute (GC) not found - critical");
        return false;
    }
    
    LOG("IP Discovery completed successfully");
    return true;
}
```

### 3. Makefile

**修改 CPP_SOURCES**：
```makefile
# 修改前
CPP_SOURCES = $(SRC_DIR)/core/RDNA4Driver.cpp

# 修改后
CPP_SOURCES = $(SRC_DIR)/core/RDNA4Driver.cpp \
	$(SRC_DIR)/hardware/IPDiscovery.cpp
```

## 🏗️ 构建步骤

### 本地（Mac）编译
```bash
cd RDNA4Driver
make clean
make
```

### 传输到黑苹果
```bash
# 方法1: scp
scp -r build/RDNA4Driver.kext user@hackintosh:/tmp/

# 方法2: 使用项目的 SSH 配置
scp -6 -i ../ssh-keys/n48-debug -r build/RDNA4Driver.kext \
    louxuanchen@louxuanchendeMac-Pro.local:/tmp/
```

### 在黑苹果上安装
```bash
ssh user@hackintosh

# 备份旧版本（可选）
sudo cp -r /Library/Extensions/RDNA4Driver.kext \
    /Library/Extensions/RDNA4Driver.kext.backup

# 安装新版本
sudo rm -rf /Library/Extensions/RDNA4Driver.kext
sudo cp -r /tmp/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext
sudo kextcache -i /

# 重启
sudo reboot
```

## 🧪 验证

### 检查加载状态
```bash
kmutil showloaded | grep rdna4
# 应该看到: com.rdna4.driver (0.1.0)
```

### 查看 IP Discovery 日志
```bash
log show --predicate 'process == "kernel"' --last 5m | grep "IP Discovery"
```

**预期输出**：
```
RDNA4: IP Discovery starting...
RDNA4: IP Discovery table signature mismatch (...)
RDNA4: IP Discovery table not found, using hardcoded config
RDNA4: IP Discovery completed via hardcoded config (7 IPs)
RDNA4: ===== Discovered IP Blocks =====
RDNA4:   [ 0] PSP      v12.0.0 @ 0x00010000 (size: 0x1000, instances: 1)
RDNA4:   [ 1] SMU      v12.0.0 @ 0x00016000 (size: 0x1000, instances: 1)
RDNA4:   [ 2] GC       v12.0.1 @ 0x00002000 (size: 0x4000, instances: 1)
RDNA4:   [ 3] DCN      v4.0.1  @ 0x00000000 (size: 0x2000, instances: 1)
RDNA4:   [ 4] SDMA     v7.0.0  @ 0x00004800 (size: 0x800, instances: 1)
RDNA4:   [ 5] MMHUB    v4.0.0  @ 0x00006800 (size: 0x800, instances: 1)
RDNA4:   [ 6] NBIO     v7.11.0 @ 0x00000C00 (size: 0x400, instances: 1)
RDNA4: ================================
RDNA4: IP Discovery completed successfully
```

## 🐛 故障排除

### 编译错误

**错误**: `IPDiscovery.hpp: No such file or directory`
- 检查文件路径：`ls -la src/hardware/IPDiscovery.hpp`
- 检查包含路径是否正确

**错误**: `undefined reference to IPDiscovery::discover`
- 检查 Makefile 中是否添加了 `IPDiscovery.cpp`
- 运行 `make clean && make`

### 运行时错误

**症状**: 系统无法启动
- 使用 `rdna4=0` boot-arg 禁用驱动
- 检查日志看具体错误

**症状**: IP Discovery 失败
- 检查 VRAM 是否正确映射
- Fallback 到硬编码配置应该始终成功

## 📊 代码统计

**新增代码**:
- IPDiscovery.hpp: ~120 行
- IPDiscovery.cpp: ~280 行
- RDNA4Driver 集成: ~50 行
- **总计**: ~450 行

**总代码量**:
- Phase 1.1: 598 行
- Phase 1.2: +450 行
- **累计**: ~1050 行

## ✅ 完成检查清单

- [ ] IPDiscovery.hpp 文件创建
- [ ] IPDiscovery.cpp 文件创建
- [ ] RDNA4Driver.hpp 修改完成
- [ ] RDNA4Driver.cpp 修改完成
- [ ] Makefile 更新完成
- [ ] 本地编译成功
- [ ] 传输到黑苹果
- [ ] 安装并重启
- [ ] IP Discovery 日志正常
- [ ] 系统稳定运行

---

最后更新: 2026-10-28
