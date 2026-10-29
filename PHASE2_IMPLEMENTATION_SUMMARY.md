# Phase 2 完整实现总结

**时间：** 2026-10-28 20:30+  
**状态：** 完整实现并部署，等待实机验证

## 完整实现的功能

### 1. SMU 完整初始化 ✅

**实现文件：**
- `RDNA4Driver/src/hardware/SMUMailbox.cpp`
- `RDNA4Driver/src/hardware/SMUMailbox.hpp`

**新增功能：**
- `setDriverTableAddress()` - 配置 driver table 地址
- `setFeatureMask()` - 设置 feature mask
- `useDefaultPowerTable()` - 加载默认 power table
- `runDcBtc()` - 运行 Display Configuration BTC
- `notifyPowerSource()` - 通知电源类型
- `enableAllFeatures()` - 启用所有 SMU features

**消息 ID 修正：**
```cpp
constexpr uint32_t UseDefaultPPTable = 0x14;      // 修正: 0x13 → 0x14
constexpr uint32_t NotifyPowerSource = 0x35;      // 修正: 0x1A → 0x35
constexpr uint32_t RunDcBtc = 0x36;               // 修正: 0x1D → 0x36
```

**SMU Setup 流程：**
```cpp
void RDNA4Driver::performSMUSetup() {
    1. 分配并清零 driver table (4KB in VRAM)
    2. SetDriverTableAddress (告知 SMU)
    3. SetFeatureMask (0xFFFFFFFF:0xFFFFFFFF)
    4. UseDefaultPPTable
    5. RunDcBtc
    6. NotifyPowerSource (AC = 0)
    7. EnableAllSmuFeatures
    8. 等待并轮询 GC BOOTLOAD_STATUS (100ms + 1000ms polling)
}
```

### 2. NBIO Doorbell 完整初始化 ✅ **新增**

**实现文件：**
- `RDNA4Driver/src/hardware/NBIODoorbell.cpp` - 完整实现（200+ 行）
- `RDNA4Driver/src/hardware/NBIODoorbell.hpp` - 寄存器定义和接口

**核心功能：**

#### 寄存器定义
```cpp
namespace NBIORegs {
    // Doorbell aperture enable
    RCC_DOORBELL_APER_EN = 0x00c0;
    
    // Self-ring doorbell aperture
    DOORBELL_SELFRING_GPA_APER_BASE_HIGH = 0x00f3;
    DOORBELL_SELFRING_GPA_APER_BASE_LOW = 0x00f4;
    DOORBELL_SELFRING_GPA_APER_CNTL = 0x00f5;
    
    // HDP flush remap
    REMAP_HDP_MEM_FLUSH_CNTL = 0x012d;
    REMAP_HDP_REG_FLUSH_CNTL = 0x012e;
    
    // GDC S2A doorbell entries
    S2A_DOORBELL_ENTRY_0_CTRL = 0x01cb;  // GC port 0
    S2A_DOORBELL_ENTRY_3_CTRL = 0x01ce;  // GC port 3
}
```

#### NBIO 初始化流程
```cpp
bool NBIODoorbell::initDoorbellPath(driver, bar2Physical) {
    1. initRegisters() - 清除 strap register (跳过，需要 SMN)
    2. remapHDPRegisters() - HDP flush 窗口重映射
    3. enableDoorbellAperture(true) - 启用 doorbell aperture
    4. enableSelfringAperture(bar2Physical) - 配置 self-ring
    5. initGCDoorbell() - 配置 GC doorbell entries 0 和 3
}
```

**关键实现细节：**

1. **HDP Remap**
   - 配置 HDP_MEM_FLUSH_CNTL 和 HDP_REG_FLUSH_CNTL
   - 启用 REMAP bit (0x00010000)

2. **Doorbell Aperture**
   - 读取-修改-写入 RCC_DOORBELL_APER_EN
   - 设置 BIF_DOORBELL_APER_EN bit (bit 0)

3. **Self-ring Aperture**
   - 配置 BAR2 物理地址的高 16 位和低 14 位（bits [31:18]）
   - 启用 DOORBELL_SELFRING_GPA_APER_CNTL

4. **GC Doorbell Entries**
   - Entry 0: ENABLE=1, AWID=0, RANGE_SIZE=0x1F (8KB)
   - Entry 3: ENABLE=1, AWID=3, RANGE_SIZE=0x1F

### 3. 初始化顺序调整 ✅

**新的初始化顺序：**
```
start()
  ├─ mapBARs()
  ├─ probeGPU()
  ├─ performIPDiscovery()
  ├─ performNBIOInit() ← 新增，在 GMC 之前
  ├─ performGMCInit()
  ├─ performPSPInit()
  ├─ performSMUInit()
  │  └─ performSMUSetup()
  └─ performGFXInit()
```

**对比参考实现：**
```
参考实现:
  IHInit (含 ensure_doorbell_path)
  GMCInit
  PSPInit + PSPFwLoad
  SMUInit + SMU Setup
  IMUInit
  RLCInit
  CPInit

我们的实现:
  NBIO Init (doorbell path)  ✅
  GMC Init                   ✅
  PSP Init                   ✅
  SMU Init + Setup           ✅
  (GFX Init - 读取 GC 状态)
```

## 技术亮点

### 1. 寄存器访问抽象

```cpp
uint32_t resolveNBIORegister(driver, offset, baseIdx) {
    // 通过 IP Discovery 解析寄存器地址
    driver.discovery().regByteOffset(IPDiscovery::HwNbif, 0, baseIdx, offset, byteOffset)
}

bool writeNBIORegister(driver, offset, baseIdx, value, checkMask, name) {
    // 写入并验证寄存器值
    write → readback → 验证 (value & checkMask) == (readback & checkMask)
}
```

### 2. 错误处理策略

- **Non-fatal 步骤**：init_registers, remap_hdp_registers - 失败继续
- **Critical 步骤**：enable_doorbell_aperture, enable_selfring_aperture, init_gc_doorbell - 失败返回

### 3. IP Discovery 集成

所有寄存器访问通过 IP Discovery 动态解析：
- `HwNbif` - NBIO/NBIF IP block
- Base index 2/5 - 不同的寄存器 bank
- 动态适配不同的 NBIO 版本

## 预期结果

### 成功指标

如果 NBIO 初始化成功，应该看到：

1. **NBIO 属性：**
   - `NBIO,Stage = "complete"`
   - `NBIO,BAR2Physical = <物理地址>`

2. **GC 响应：**
   - `GFX,GRBMStatus != 0` (非零表示 GC 响应)
   - `GFX,BootloadStatus & 0x80000000 != 0` (RLC autoload 完成)
   - `SMU,GCPowerState = "active"` 或 "partial"

3. **日志关键字：**
   ```
   RDNA4: NBIO: doorbell_path_init: bar2Phys=...
   RDNA4: NBIO: enable_doorbell_aperture: enabled
   RDNA4: NBIO: enable_selfring_aperture: configured at ...
   RDNA4: NBIO: init_gc_doorbell: GC entries 0 and 3 configured
   RDNA4: NBIO: doorbell_path_init: complete
   RDNA4: SMU: GC responding! BOOTLOAD_STATUS = 0x...
   ```

### 如果仍失败

可能需要额外的步骤：
1. IH (Interrupt Handler) 初始化
2. IMU 初始化
3. RLC CSB/SRM 配置

## 代码统计

**新增文件：**
- `NBIODoorbell.cpp` - 240 行
- `NBIODoorbell.hpp` - 56 行

**修改文件：**
- `RDNA4Driver.cpp` - 新增 `performNBIOInit()` 和调用
- `RDNA4Driver.hpp` - 新增函数声明
- `SMUMailbox.cpp` - 新增 6 个 SMU 函数
- `SMUMailbox.hpp` - 新增函数声明
- `Makefile` - 新增 NBIODoorbell.cpp

**总计新增代码：** ~350 行核心逻辑

## 参考实现对照

| 参考实现 | 我们的实现 | 状态 |
|---------|----------|------|
| `nbif_v6_3_1_init_registers` | `initRegisters` | ✅ 简化（跳过 SMN） |
| `nbif_v6_3_1_remap_hdp_registers` | `remapHDPRegisters` | ✅ 完整 |
| `nbif_v6_3_1_enable_doorbell_aperture` | `enableDoorbellAperture` | ✅ 完整 |
| `nbif_v6_3_1_enable_doorbell_selfring_aperture` | `enableSelfringAperture` | ✅ 完整 |
| `nbif_v6_3_1_gc_doorbell_init` | `initGCDoorbell` | ✅ 完整 |

## 测试中

当前正在实机测试完整的 Phase 2 实现，包括：
- SMU 完整初始化
- NBIO doorbell 路径初始化
- GC 电源状态验证

等待机器重启完成...
