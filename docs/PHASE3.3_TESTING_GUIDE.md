# Phase 3.3 完整测试指南

**目标**: 在黑苹果上测试完整的用户态 Metal 集成

---

## 📋 前置条件检查

### 1. 确认 Phase 2.3 已通过

```bash
# 检查驱动版本
kextstat | grep rdna4
# 期望: com.rdna4.driver (0.6.5)

# 检查 ComputeTest 状态
ioreg -l -n RDNA4Driver | grep "ComputeTest,Stage"
# 期望: "ComputeTest,Stage" = "passed"

# 检查错误数
ioreg -l -n RDNA4Driver | grep "Errors"
# 期望: "Errors" = 0
```

**如果 Phase 2.3 未通过，Phase 3.3 无法测试！**

### 2. 确认开发工具已安装

```bash
# Xcode Command Line Tools
xcode-select -p
# 期望: /Library/Developer/CommandLineTools 或 /Applications/Xcode.app/Contents/Developer

# Clang 编译器
clang --version
# 期望: Apple clang version 14.0+ 或 15.0+

# Make
make --version
```

---

## 🔧 步骤 1: 编译 AIRNT Plugin

```bash
cd ~/hackintosh/navi48-native/airnt-plugin

# 清理旧编译
make clean

# 编译
make

# 检查产物
ls -lh build/libAMDNTPlugin.dylib
# 期望: 文件存在，大小 ~50-100 KB

# 测试加载
otool -L build/libAMDNTPlugin.dylib
# 应该看到依赖项（Foundation.framework 等）
```

**如果编译失败**:
- 检查是否有 Phase 1 的 air2gfx12 代码
- 确认 Makefile 中的路径正确

---

## 🔧 步骤 2: 编译 Shader

```bash
cd ~/hackintosh/RDNA4Driver

# 确认 shader 源码存在
ls -lh shaders/vectoradd.metal
# 期望: 文件存在

# 运行编译脚本
chmod +x compile_shader.sh
./compile_shader.sh

# 检查产物
ls -lh shaders/compiled/
# 期望:
#   vectoradd.air
#   vectoradd.metallib
#   vectoradd_gfx12.s  (汇编)
#   vectoradd_gfx12.o  (ELF object)
#   vectoradd_gfx12.bin (机器码)
#   vectoradd_gfx12.h  (C 数组)

# 验证 metallib
xxd shaders/compiled/vectoradd.metallib | head
# 应该看到 'MTLB' magic (4D 54 4C 42)

# 验证机器码不是空的
wc -c shaders/compiled/vectoradd_gfx12.bin
# 期望: 大于 100 字节
```

**如果编译失败**:
```bash
# 检查 Metal 编译器
xcrun -sdk macosx metal --version

# 手动编译测试
xcrun -sdk macosx metal -c shaders/vectoradd.metal -o /tmp/test.air
# 如果失败，检查 Metal shader 语法
```

---

## 🔧 步骤 3: 编译 Metal Bundle

```bash
cd ~/hackintosh/navi48-native/metal-bundle

# 清理
make clean

# 编译
make

# 检查产物
ls -lh build/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal
# 期望: 动态库存在，大小 ~100-200 KB

# 检查 AIRNT plugin 是否已复制
ls -lh build/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib
# 期望: 文件存在

# 检查 Info.plist
plutil -lint build/RDNA4Metal.bundle/Contents/Info.plist
# 期望: "Info.plist: OK"

# 检查符号
nm build/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal | grep RDNA4Device
# 应该看到 RDNA4Device 相关符号
```

**常见编译错误**:

### 错误 1: 找不到 Foundation.framework
```bash
# 解决: 指定 SDK
make CFLAGS="-isysroot $(xcrun --show-sdk-path)"
```

### 错误 2: 链接错误
```bash
# 检查依赖
otool -L build/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal
```

---

## 🔧 步骤 4: 编译测试 App

```bash
cd ~/hackintosh/navi48-native/metal-bundle

# 编译测试程序
make test

# 或手动编译
mkdir -p build/test
clang -framework Foundation -framework Metal \
      -o build/test/RDNA4MetalTest \
      test/RDNA4MetalTest.m

# 检查
ls -lh build/test/RDNA4MetalTest
# 期望: 可执行文件存在

# 复制 metallib 到测试 bundle（如果测试 app 需要）
cp ~/hackintosh/RDNA4Driver/shaders/compiled/vectoradd.metallib \
   build/test/
```

---

## 🚀 步骤 5: 部署 Metal Bundle

```bash
cd ~/hackintosh/navi48-native/metal-bundle

# 备份现有的（如果有）
if [ -d /Library/GPUBundles/RDNA4Metal.bundle ]; then
    sudo cp -R /Library/GPUBundles/RDNA4Metal.bundle \
        /Library/GPUBundles/RDNA4Metal.bundle.backup-$(date +%Y%m%d)
fi

# 安装新 bundle
sudo cp -R build/RDNA4Metal.bundle /Library/GPUBundles/
sudo chown -R root:wheel /Library/GPUBundles/RDNA4Metal.bundle

# 验证安装
ls -la /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/
# 应该看到 RDNA4Metal 和 libAMDNTPlugin.dylib
```

---

## 🧪 步骤 6: 运行测试（无需重启）

Phase 3.3 是用户态代码，**不需要重启**！直接测试：

```bash
cd ~/hackintosh/navi48-native/metal-bundle

# 运行测试
./build/test/RDNA4MetalTest
```

### 期望输出（成功）

```
========================================
RDNA4 Metal Test - Phase 3.3
========================================

✅ Found Metal device: AMD Radeon RX 9070 XT (RDNA4)

========================================
Device Information
========================================
Name: AMD Radeon RX 9070 XT (RDNA4)
Registry ID: 0x7550
Low Power: NO
Headless: NO
Removable: YES
Max Working Set: 16.00 GB
Max Threads Per Threadgroup: 1024 x 1024 x 1024
========================================

========================================
Test: Vector Addition
========================================
✅ Loaded library: vectoradd.metallib
✅ Found function: vectorAdd
✅ Created compute pipeline
   Max total threads: 1024
   Thread execution width: 32
✅ Allocated buffers (4096 bytes each)
✅ Initialized input data
   A[0..3] = [0.0, 1.0, 2.0, 3.0]
   B[0..3] = [0.0, 2.0, 4.0, 6.0]
✅ Encoded compute command
   Threadgroups: 4
   Threads per group: 256
⏳ Submitting to GPU...
✅ GPU execution completed
🔍 Verifying results...
✅ PASSED: All 1024 elements correct
   C[0..3] = [0.0, 3.0, 6.0, 9.0]
   C[1020..1023] = [3060.0, 3063.0, 3066.0, 3069.0]

========================================
Test: Multiple Dispatches
========================================
✅ Iteration 1/5 passed
✅ Iteration 2/5 passed
✅ Iteration 3/5 passed
✅ Iteration 4/5 passed
✅ Iteration 5/5 passed
✅ All 5 iterations passed

========================================
Test Summary
========================================
✅ ALL TESTS PASSED
========================================
Phase 3.3 is FULLY FUNCTIONAL!
User-space Metal applications can now
submit compute work to RDNA4 GPU!
========================================
```

---

## 🐛 故障排查

### 问题 1: 找不到 Metal 设备

**症状**: `❌ FATAL: No Metal device found`

**原因**: Metal Bundle 未被识别

**排查步骤**:
```bash
# 1. 检查 Bundle 是否存在
ls -la /Library/GPUBundles/RDNA4Metal.bundle

# 2. 检查 Info.plist
plutil -p /Library/GPUBundles/RDNA4Metal.bundle/Contents/Info.plist

# 3. 检查系统日志
sudo dmesg | grep -i metal
sudo dmesg | grep -i gpu

# 4. 检查 IORegistry
ioreg -l -w 0 | grep -i rdna4

# 5. 强制刷新 GPU Bundle 缓存
sudo rm -rf /System/Library/Caches/com.apple.metal
sudo touch /Library/GPUBundles/
```

**解决**:
- 确认 Info.plist 中的 IOPCIMatch 与设备 ID 匹配
- 检查 MetalPluginClassName 是否正确（RDNA4Device）

### 问题 2: 找不到 libAMDNTPlugin.dylib

**症状**: `Failed to load libAMDNTPlugin.dylib`

**排查**:
```bash
# 检查 plugin 是否在 bundle 中
ls -la /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib

# 检查权限
ls -l /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib
# 应该是 root:wheel

# 测试直接加载
otool -L /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib
```

**解决**:
```bash
# 重新复制 plugin
sudo cp navi48-native/airnt-plugin/build/libAMDNTPlugin.dylib \
    /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/
sudo chown root:wheel /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib
```

### 问题 3: 找不到 vectorAdd 函数

**症状**: `Function 'vectorAdd' not found`

**排查**:
```bash
# 检查 metallib
strings ~/hackintosh/RDNA4Driver/shaders/compiled/vectoradd.metallib | grep vectorAdd

# 手动测试 metallib
xcrun -sdk macosx metal-objdump -macho -section-headers \
    ~/hackintosh/RDNA4Driver/shaders/compiled/vectoradd.metallib
```

**解决**:
- 重新编译 shader: `cd RDNA4Driver && ./compile_shader.sh`
- 确认 metallib 路径正确

### 问题 4: 编译失败

**症状**: `Compilation failed for 'vectorAdd'`

**排查**:
```bash
# 查看详细日志
./build/test/RDNA4MetalTest 2>&1 | grep -A10 "Compilation failed"

# 检查 AIRNT plugin
nm /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib | grep AIRNTCompile
```

**解决**:
- 检查 AIR bitcode 是否有效
- 检查 AIRNT plugin 版本是否匹配

### 问题 5: IOUserClient 连接失败

**症状**: `Failed to open IOUserClient`

**排查**:
```bash
# 检查 RDNA4Accel 是否加载
kextstat | grep RDNA4Accel

# 检查 IORegistry
ioreg -l -n RDNA4Accel

# 检查权限
ls -l /Library/Extensions/RDNA4Accel.kext
```

**解决**:
```bash
# 重新加载驱动
sudo kextunload /Library/Extensions/RDNA4Accel.kext
sudo kextload /Library/Extensions/RDNA4Accel.kext
```

### 问题 6: 命令提交失败

**症状**: `Command buffer failed with status: 4`

**排查**:
```bash
# 查看驱动日志
sudo dmesg | grep -A10 "SubmitCommands"

# 检查 ComputeRing 状态
ioreg -l -n RDNA4Driver | grep ComputeRing

# 检查 GFX 状态
ioreg -l -n RDNA4Driver | grep "GFX,Stage"
# 必须是 "gfx-alive"
```

**解决**:
- 确认 Phase 2.3 正常工作
- 检查 PM4 命令格式是否正确

### 问题 7: 结果验证失败

**症状**: `❌ FAILED: 1024 errors out of 1024 elements`

**可能原因**:
1. **使用了 stub shader**: 检查 `vectoradd_gfx12.h` 是否包含 `USE_STUB_SHADER`
2. **Shader 未正确执行**: GPU 没有运行计算
3. **Buffer 地址错误**: User data 配置不正确

**排查**:
```bash
# 检查是否是 stub
grep "USE_STUB_SHADER" RDNA4Driver/shaders/compiled/vectoradd_gfx12.h

# 如果找到，说明是 stub，需要重新编译真实 shader
cd RDNA4Driver
./compile_shader.sh
```

---

## 📊 性能基准测试

如果测试通过，可以运行性能基准：

```bash
# 创建简单的性能测试
cat > bench.m << 'EOF'
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        // ... 加载 library, 创建 pipeline ...
        
        NSTimeInterval start = [NSDate timeIntervalSinceReferenceDate];
        
        for (int i = 0; i < 100; i++) {
            // 提交命令
            [commandBuffer commit];
            [commandBuffer waitUntilCompleted];
        }
        
        NSTimeInterval elapsed = [NSDate timeIntervalSinceReferenceDate] - start;
        NSLog(@"100 iterations in %.3f seconds (%.1f Hz)", elapsed, 100.0/elapsed);
    }
}
EOF

clang -framework Foundation -framework Metal -o bench bench.m
./bench
```

**期望性能**:
- 提交率: > 1000 Hz
- 单次向量加法: < 1 ms

---

## ✅ 成功标准

Phase 3.3 测试通过的标准：

- ✅ `MTLCreateSystemDefaultDevice()` 返回 RDNA4Device
- ✅ 能加载 `.metallib` 文件
- ✅ AIRNT 编译成功（无 stub）
- ✅ 创建 Pipeline 成功
- ✅ Buffer 分配成功
- ✅ 命令提交成功
- ✅ GPU 执行完成
- ✅ 结果验证通过（1024/1024 正确）
- ✅ 多次分发稳定（5/5 通过）

---

## 🔜 下一步

测试通过后，你可以：

1. **运行自己的 Metal App**: 任何标准 Metal Compute App 都应该能工作
2. **开始 Phase 3.4**: 实现 3D 渲染（首个三角形）
3. **性能优化**: Pipeline 缓存、Buffer 池等
4. **更多测试**: 不同大小的 workload、并发提交等

---

*Phase 3.3 测试指南创建于 2026-10-30*
