# RDNA4Accel.kext - IOAccelerator Service Provider

**Phase 2 核心驱动** - 为 RDNA4 GPU 提供 IOAccelerator 服务，使 Metal.framework 能够发现和使用硬件加速。

---

## 功能概述

### Phase 2（当前实现）

- ✅ 注册为 `IOAccelerator` 服务
- ✅ 发布设备属性到 IORegistry（vendor/device ID, VRAM, 能力）
- ✅ 提供 IOUserClient 接口（stub 实现）
- ✅ 让 WindowServer 识别硬件加速器

### Phase 3.3（未来扩展）

- 🔜 实际的 Buffer 分配（VRAM 管理）
- 🔜 命令提交到 GPU Compute Ring
- 🔜 Fence 同步机制
- 🔜 与 RDNA4Driver 的完整集成

---

## 架构

```
Metal.framework
        ↓ 发现 IOAccelerator
RDNA4Accel.kext (本驱动)
        ↓ IOResourceMatch
RDNA4Driver.kext (主驱动)
        ↓ 硬件控制
AMD RX 9070 XT GPU
```

---

## 编译

### 前置条件

1. **KDK (Kernel Development Kit)** 已安装
2. **macOS 版本**: 27.x (Tahoe)
3. **内核版本**: 27.0.0

### 构建命令

```bash
cd RDNA4Accel

# 检查 KDK 是否存在
make info

# 编译
make

# 测试 kext 有效性
make test
```

### 输出

```
build/RDNA4Accel.kext/
├── Contents/
│   ├── Info.plist
│   └── MacOS/
│       └── RDNA4Accel
```

---

## 安装与部署

### 方式 1: 手动安装到系统

```bash
# 拷贝到 /Library/Extensions/
sudo cp -R build/RDNA4Accel.kext /Library/Extensions/

# 设置权限
sudo chown -R root:wheel /Library/Extensions/RDNA4Accel.kext

# 重建 kext 缓存
sudo kextcache -i /

# 重启
sudo reboot
```

### 方式 2: 通过 OpenCore 注入（推荐）

```bash
# 拷贝到 U 盘 EFI
cp -R build/RDNA4Accel.kext /Volumes/1401/EFI/OC/Kexts/

# 编辑 config.plist，添加 Kernel/Add 条目：
# - BundlePath: RDNA4Accel.kext
# - Enabled: true
# - ExecutablePath: Contents/MacOS/RDNA4Accel
```

---

## 验证

### 1. 检查 kext 是否加载

```bash
kextstat | grep -i rdna4
```

期望看到：
```
com.rdna4.driver
com.rdna4.accel
```

### 2. 检查 IORegistry

```bash
ioreg -l | grep -i "IOAccelerator"
```

期望看到：
```
+-o RDNA4Accel  <class RDNA4Accel>
    | {
    |   "IOMatchCategory" = "IOAccelerator"
    |   "model" = "AMD Radeon RX 9070 XT"
    |   "vendor-id" = 0x1002
    |   "device-id" = 0x7550
    |   "vram-size" = 17179869184
    |   "capabilities" = ["Metal","Compute","Graphics"]
    | }
```

### 3. 检查 Metal 设备发现

```bash
# 使用 system_profiler
system_profiler SPDisplaysDataType

# 或者运行简单的 Metal 测试程序
cat > /tmp/test_metal.m << 'EOF'
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

int main() {
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    NSLog(@"Found %lu Metal device(s)", devices.count);
    for (id<MTLDevice> device in devices) {
        NSLog(@"  - %@", device.name);
    }
    return 0;
}
EOF

clang -framework Metal -framework Foundation /tmp/test_metal.m -o /tmp/test_metal
/tmp/test_metal
```

### 4. 检查 WindowServer 日志

```bash
log show --predicate 'process == "WindowServer"' --last 5m | grep -i "gpu\|accel"
```

期望看到：
```
gpu 0x0ae3 accel != 0x0
```

---

## IOUserClient 接口

### 方法列表（Phase 2 stub 实现）

| Selector | 方法名 | 输入 | 输出 | 状态 |
|----------|--------|------|------|------|
| 0 | `GetDeviceInfo` | - | vendorID, deviceID | ✅ 实现 |
| 1 | `AllocateBuffer` | size | address | 🔜 Phase 3.3 |
| 2 | `FreeBuffer` | address | - | 🔜 Phase 3.3 |
| 3 | `SubmitCommands` | commands | - | 🔜 Phase 3.3 |
| 4 | `WaitFence` | fenceValue, timeout | - | 🔜 Phase 3.3 |

### 调用示例

```cpp
#include <IOKit/IOKitLib.h>

io_connect_t conn;
io_service_t service = IOServiceGetMatchingService(
    kIOMasterPortDefault,
    IOServiceMatching("RDNA4Accel")
);

kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
if (kr == KERN_SUCCESS) {
    uint32_t vendorID, deviceID;
    uint64_t outputs[2];
    uint32_t outputCount = 2;
    
    kr = IOConnectCallMethod(conn, 0, NULL, 0, NULL, 0,
                             outputs, &outputCount, NULL, NULL);
    
    vendorID = (uint32_t)outputs[0];
    deviceID = (uint32_t)outputs[1];
    
    printf("Vendor: 0x%x, Device: 0x%x\n", vendorID, deviceID);
    
    IOServiceClose(conn);
}
```

---

## 故障排查

### 问题: kext 加载失败

**症状**: `kextstat` 中看不到 `com.rdna4.accel`

**排查**:
```bash
# 查看详细日志
sudo dmesg | grep -i rdna4

# 手动加载测试
sudo kextutil -t build/RDNA4Accel.kext
```

**常见原因**:
- KDK 版本不匹配
- 依赖的 `RDNA4Driver` 未加载
- Info.plist 配置错误

### 问题: IORegistry 中找不到节点

**症状**: `ioreg` 中没有 `RDNA4Accel`

**排查**:
```bash
# 检查 RDNA4Driver 是否已发布服务
ioreg -l | grep RDNA4Driver

# 检查匹配规则
grep -A5 IOResourceMatch RDNA4Accel/Info.plist
```

**常见原因**:
- `IOProviderClass` 或 `IOResourceMatch` 配置错误
- `RDNA4Driver` 未正确 `registerService()`

### 问题: Metal 仍然看不到设备

**症状**: `MTLCopyAllDevices()` 返回空数组

**排查**:
```bash
# 检查 Metal Bundle 是否安装
ls -la /Library/GPUBundles/ | grep RDNA4

# 检查 IOAccelerator 属性
ioreg -l -n RDNA4Accel | grep -E "IOMatchCategory|MetalPlugin"
```

**常见原因**:
- `RDNA4Metal.bundle` 未部署（Phase 3.2）
- `MetalPluginClassName` 属性未设置

---

## Phase 3.3 扩展计划

当 Phase 3.3 实现时，需要扩展以下功能：

1. **Buffer 管理**:
   ```cpp
   IOReturn method_AllocateBuffer(uint64_t size, uint64_t* outAddress) {
       // 通过 RDNA4Driver 分配 VRAM
       IOBufferMemoryDescriptor* buffer = mainDriver->allocateVRAM(size);
       *outAddress = buffer->getPhysicalAddress();
       return kIOReturnSuccess;
   }
   ```

2. **命令提交**:
   ```cpp
   IOReturn method_SubmitCommands(const void* commands, size_t commandSize) {
       // 提交 PM4 命令流到 GPU Ring
       return mainDriver->submitPM4(commands, commandSize);
   }
   ```

3. **Fence 同步**:
   ```cpp
   IOReturn method_WaitFence(uint64_t fenceValue, uint64_t timeoutNS) {
       // 等待 GPU 完成到指定 fence
       return mainDriver->waitForFence(fenceValue, timeoutNS);
   }
   ```

---

*Part of RDNA4 macOS Native Metal Driver Project - Phase 2*
