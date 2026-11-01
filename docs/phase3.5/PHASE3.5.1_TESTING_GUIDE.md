# Phase 3.5.1 IOSurface 测试指南

**测试目标**: 验证 IOSurface 支持的基础功能  
**前置条件**: Phase 3.4 已完成并验证通过

---

## 📋 测试文件列表

| 文件 | 说明 | 大小 |
|------|------|------|
| `IOSurfaceTest.m` | 完整 IOSurface 测试（7 个测试项） | 12.8 KB |
| `QuickIOSurfaceTest.m` | 快速测试（5 个基础测试，无 shader） | 5.3 KB |
| `run_iosurface_test.sh` | 完整测试脚本 | 4.0 KB |
| `quick_iosurface_test.sh` | 快速测试脚本 | 0.8 KB |

---

## 🚀 快速开始

### 方法 1: 快速测试（推荐，5 分钟）

```bash
cd navi48-native/metal-bundle/test

# 设置执行权限
chmod +x quick_iosurface_test.sh

# 运行测试
./quick_iosurface_test.sh
```

**期望输出**:
```
========================================
  Quick IOSurface Test
========================================

Test 1/5: Creating IOSurface...
✅ IOSurface created: 256x256

Test 2/5: Getting Metal device...
✅ Metal device: AMD Radeon RX 9070 XT (RDNA4)

Test 3/5: Creating texture from IOSurface...
✅ Texture created from IOSurface
   Size: 256x256
   Storage: 0 (0=Shared)

Test 4/5: Writing to IOSurface (CPU)...
✅ Written test pattern to IOSurface

Test 5/5: Reading from texture (GPU access)...
   Read via Metal:
     [0,0] = 0xFFFF0000 (expected 0xFFFF0000)
     [1,0] = 0xFF00FF00 (expected 0xFF00FF00)
     [0,1] = 0xFF0000FF (expected 0xFF0000FF)
✅ CPU write → GPU read: PERFECT MATCH

========================================
  Summary: 5/5 tests passed
========================================
✅ ALL TESTS PASSED
IOSurface basic functionality works!

Next: Implement full IOSurface support in RDNA4Texture
```

---

### 方法 2: 完整测试（需要 shader，10 分钟）

**步骤 1**: 编译 shader
```bash
cd navi48-native/metal-bundle/test

xcrun -sdk macosx metal -c triangle.metal -o triangle.air
xcrun -sdk macosx metallib triangle.air -o triangle.metallib
```

**步骤 2**: 运行完整测试
```bash
chmod +x run_iosurface_test.sh
./run_iosurface_test.sh
```

**期望输出**: 7 个测试全部通过
```
Test 1: IOSurface creation             ✅
Test 2: Metal device                   ✅
Test 3: Texture from IOSurface         ✅
Test 4: CPU write to IOSurface         ✅
Test 5: GPU read from IOSurface        ✅
Test 6: GPU render to IOSurface        ✅
Test 7: Performance                    ✅

Phase 3.5.1 IOSurface support: PASSED
```

---

## 📊 测试覆盖

### 测试 1: IOSurface 创建
- ✅ 创建 256x256 BGRA8 IOSurface
- ✅ 验证尺寸和格式
- ✅ 检查内存分配

### 测试 2: Metal 设备
- ✅ 获取 RDNA4 Metal 设备
- ✅ 确认设备支持 IOSurface

### 测试 3: 从 IOSurface 创建纹理
- ✅ `newTextureWithDescriptor:iosurface:plane:`
- ✅ `MTLStorageModeShared` 模式
- ✅ `MTLTextureUsageRenderTarget` 用途

### 测试 4: CPU 写入
- ✅ `IOSurfaceLock` / `IOSurfaceUnlock`
- ✅ 获取 CPU 地址 `IOSurfaceGetBaseAddress`
- ✅ 写入测试图案

### 测试 5: GPU 读取
- ✅ `[texture getBytes:...]` 读回
- ✅ 验证 CPU 写入的数据可被 GPU 读取
- ✅ 检查 CPU/GPU 一致性

### 测试 6: GPU 渲染（需要 shader）
- ✅ 渲染三角形到 IOSurface-backed texture
- ✅ GPU 写入 → CPU 读取验证
- ✅ 端到端渲染测试

### 测试 7: 性能测试
- ✅ 1000 次 lock/unlock 操作
- ✅ 测量吞吐量（ops/sec）

---

## 🔍 故障排查

### 问题 1: IOSurface 创建失败

**症状**:
```
❌ FAILED: Could not create IOSurface
```

**原因**: 系统不支持或内存不足

**解决**:
```bash
# 检查系统版本
sw_vers

# 检查可用内存
vm_stat

# 尝试更小的尺寸
# 修改 IOSurfaceTest.m 中的 width/height
```

---

### 问题 2: Texture 创建失败

**症状**:
```
❌ FAILED: Could not create texture from IOSurface
```

**原因**: Metal 设备不支持 IOSurface 或格式不兼容

**解决**:
```objc
// 检查设备能力
NSLog(@"Max texture size: %lux%lu", 
      device.maxTexture2DSize, device.maxTexture2DSize);

// 尝试不同格式
desc.pixelFormat = MTLPixelFormatRGBA8Unorm;

// 确保 storageMode 正确
desc.storageMode = MTLStorageModeShared;
```

---

### 问题 3: CPU/GPU 数据不一致

**症状**:
```
⚠️  WARNING: CPU and GPU views differ
```

**原因**: 缓存未同步

**解决**:
```objc
// 在读取前刷新缓存
IOSurfaceFlushProcessorCaches(surface);

// 或使用 synchronize
// (需要在 RDNA4Texture 中实现)
```

---

### 问题 4: 编译错误

**症状**:
```
error: 'IOSurface/IOSurface.h' file not found
```

**解决**:
```bash
# 确认 Xcode 已安装
xcode-select -p

# 重新安装 Command Line Tools
xcode-select --install

# 使用完整路径编译
clang -isysroot $(xcrun --show-sdk-path) ...
```

---

## 📈 性能基准

### 预期性能指标

| 操作 | 预期性能 | 说明 |
|------|---------|------|
| IOSurface 创建 | < 1 ms | 256x256 |
| Texture 创建 | < 1 ms | 从 IOSurface |
| Lock/Unlock | > 10k ops/sec | 空操作 |
| CPU → GPU 同步 | < 0.1 ms | 小区域 |
| GPU → CPU 同步 | < 0.5 ms | 小区域 |

### 实际测试

运行测试后查看 Test 7 的输出：
```
✅ Performance: 15000 lock/unlock ops/sec
   (1000 iterations in 0.067 seconds)
```

如果性能低于预期：
- 检查是否有其他进程占用 GPU
- 确认 IOSurface 是否正确映射
- 验证缓存策略是否合理

---

## ✅ 成功标准

Phase 3.5.1 通过需要：

- [x] 快速测试 5/5 通过
- [x] IOSurface 创建成功
- [x] Texture 创建成功
- [x] CPU 写入 → GPU 读取一致
- [x] 性能达标（> 5k ops/sec）

**可选**:
- [ ] 完整测试 7/7 通过（需要 shader）
- [ ] GPU 渲染到 IOSurface
- [ ] GPU 写入 → CPU 读取一致

---

## 🔜 下一步

Phase 3.5.1 通过后：

1. **实现 RDNA4Texture 的 IOSurface 支持**
   - 完善 `initWithIOSurface:` 方法
   - 正确映射 IOSurface 到 GPU 地址
   - 处理缓存一致性

2. **进入 Phase 3.5.2**
   - 实现 CAMetalLayer
   - 实现 Drawable 管理
   - 测试 nextDrawable

3. **进入 Phase 3.5.3**
   - 实现 Present 机制
   - WindowServer 集成
   - 显示到窗口

---

## 📚 参考资料

### Apple 文档
- [IOSurface Framework](https://developer.apple.com/documentation/iosurface)
- [Metal Texture from IOSurface](https://developer.apple.com/documentation/metal/mtldevice/1433425-newtexturewithdescriptor)
- [CAMetalLayer](https://developer.apple.com/documentation/quartzcore/cametallayer)

### 代码示例
- `IOSurfaceTest.m` - 完整测试
- `QuickIOSurfaceTest.m` - 快速测试
- Phase 3.4 TriangleTest - GPU 渲染参考

---

**测试准备完成！现在可以运行 `./quick_iosurface_test.sh` 开始测试！** 🚀
