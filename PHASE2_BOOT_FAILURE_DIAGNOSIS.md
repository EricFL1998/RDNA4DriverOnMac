# Phase 2 启动失败诊断

**时间：** 2026-10-28 20:45+  
**状态：** 机器启动失败，持续离线 9+ 分钟

## 问题描述

部署完整 NBIO 初始化后，机器重启失败：
- 等待时间：400+ 秒（6 分 40 秒）首次超时
- 额外等待：150 秒（2.5 分钟）
- **总计离线时间：9+ 分钟**
- SSH 连接：持续超时
- 机器状态：未响应

## 可能的原因

### 1. NBIO 寄存器配置错误（最可能）

**问题点：**
- NBIO 寄存器写入可能导致硬件挂起
- 特定的寄存器值可能触发保护机制
- doorbell aperture 配置可能与硬件状态冲突

**具体怀疑：**

#### a) Self-ring Aperture 配置
```cpp
uint32_t baseLow = (addrLow >> 18) & 0x3FFF;  // bits [31:18]
```
- 如果 BAR2 物理地址计算错误，可能导致地址冲突
- Self-ring aperture 与其他硬件重叠

#### b) GC Doorbell Entry 配置
```cpp
entry0 |= (0x1F << 16);  // RANGE_SIZE = 0x1F (8KB)
```
- RANGE_SIZE 可能不正确
- AWID 配置可能冲突

#### c) HDP Remap 值
```cpp
const uint32_t memFlushValue = 0x00010000;  // REMAP bit
```
- 这个值可能不适用于当前硬件状态

### 2. 初始化顺序问题

NBIO 在 GMC 之前初始化，但：
- GMC 可能需要先配置某些内存区域
- NBIO 可能依赖 VRAM 已经可访问

### 3. 缺少前置条件

NBIO 初始化可能需要：
- IH (Interrupt Handler) 先初始化
- 某些 NBIO 寄存器必须在特定状态
- BAR2 映射需要特殊配置

## 恢复方案

### 紧急方案：移除 NBIO 初始化

创建一个禁用 NBIO 的版本：

```cpp
void RDNA4Driver::performNBIOInit() {
    // 临时禁用 NBIO 初始化
    LOG("NBIO: initialization DISABLED (boot-hang workaround)");
    setProperty("NBIO,Stage", "disabled");
    return;
    
    // ... 原始代码 ...
}
```

### 调试方案：逐步启用

分阶段测试 NBIO 功能：

**阶段 1：只测试寄存器读取**
```cpp
void RDNA4Driver::performNBIOInit() {
    uint32_t test = NBIODoorbell::readNBIORegister(*this, 
        NBIORegs::RCC_DOORBELL_APER_EN, 
        NBIORegs::RCC_DOORBELL_APER_EN_BASE_IDX);
    LOG("NBIO: RCC_DOORBELL_APER_EN = 0x%08x", test);
    setProperty("NBIO,TestRead", static_cast<uint64_t>(test), 32);
    // 不写入任何寄存器
}
```

**阶段 2：只启用 doorbell aperture**
```cpp
void RDNA4Driver::performNBIOInit() {
    NBIODoorbell::enableDoorbellAperture(*this, true);
    setProperty("NBIO,Stage", "aperture-only");
    // 跳过其他步骤
}
```

**阶段 3：逐步添加其他功能**
- HDP remap
- Self-ring aperture
- GC doorbell entries

### 参考实现对比检查

需要仔细对比：

1. **寄存器值的来源**
   - 我们的硬编码值是否正确？
   - 参考实现是否动态计算某些值？

2. **写入顺序**
   - 是否必须先写某个寄存器再写另一个？
   - 是否需要延迟？

3. **前置条件检查**
   - 参考实现是否检查某些状态位？
   - 是否需要等待硬件就绪？

## 下一步行动

### 立即需要：

1. **物理访问机器**
   - 检查是否显示画面（黑屏/花屏/冻结）
   - 检查是否能进入恢复模式
   - 可能需要强制重启

2. **启动到安全模式**
   - 如果可以启动，禁用 RDNA4Driver.kext
   - 或者从备份恢复旧版本

3. **回滚到 Phase 2 前的版本**
   - 重新部署没有 NBIO 的版本
   - 验证系统能正常启动

### 代码修复：

1. **实现 boot-arg 开关**
```cpp
void RDNA4Driver::performNBIOInit() {
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("rdna4-nbio", &enabled, sizeof(enabled)) || enabled == 0) {
        LOG("NBIO: initialization skipped (set rdna4-nbio=1 to enable)");
        setProperty("NBIO,Stage", "skipped");
        return;
    }
    // ... 原始代码 ...
}
```

2. **添加详细的错误处理**
```cpp
if (!writeNBIORegister(...)) {
    LOG_ERROR("NBIO: register write failed - ABORTING initialization");
    setProperty("NBIO,Stage", "failed-safe");
    return;  // 不继续，避免进一步损坏
}
```

3. **添加寄存器读回验证**
```cpp
uint32_t before = readNBIORegister(...);
LOG("NBIO: before write: 0x%08x", before);
writeNBIORegister(...);
uint32_t after = readNBIORegister(...);
LOG("NBIO: after write: 0x%08x", after);
```

## 经验教训

1. **硬件初始化需要更保守的测试**
   - 每次只改一个步骤
   - 先读后写，验证每个寄存器
   - 提供回退机制

2. **需要更好的调试支持**
   - boot-arg 开关控制每个功能
   - 详细的寄存器日志
   - 安全模式/降级模式

3. **参考实现的细节很重要**
   - 不能只看函数调用顺序
   - 寄存器值、位域、时序都很关键
   - 可能有隐含的依赖关系

## 文件保留

**成功的代码（需要修复）：**
- `RDNA4Driver/src/hardware/NBIODoorbell.cpp` - 实现完整但需要调试
- `RDNA4Driver/src/hardware/NBIODoorbell.hpp` - 寄存器定义
- `PHASE2_IMPLEMENTATION_SUMMARY.md` - 实现文档

**备份需求：**
- Phase 2 之前的工作版本（Phase 1 完成版）
- 当前版本作为参考（不要部署）

## 当前状态

- ❌ 机器无法启动
- ✅ 代码已实现（但导致启动失败）
- ⚠️ 需要物理访问机器恢复系统

**Phase 2 状态：** 实现完成但未通过实机验证，需要修复启动问题。
