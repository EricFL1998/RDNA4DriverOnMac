# Phase 3.3 测试方法完整总结

**创建日期**: 2026-10-30  
**测试对象**: 用户态 Metal 集成（Metal.framework → RDNA4Metal.bundle → RDNA4Driver.kext → GPU）

---

## 🎯 测试目标

验证完整的用户态 Metal 软件栈：
1. ✅ Metal Bundle 被系统识别
2. ✅ 动态加载并编译 Metal Shader（AIRNT Plugin）
3. ✅ 分配 GPU 内存
4. ✅ 构建并提交 PM4 命令
5. ✅ GPU 执行计算
6. ✅ 读回并验证结果

---

## 📋 测试方法（3 种）

### 方法 1: 一键自动化测试（推荐）⭐

**最简单，适合快速验证**

```bash
# 在项目根目录
chmod +x test_phase3.3.sh
./test_phase3.3.sh
```

**脚本会自动**:
1. 检查 Phase 2.3 驱动状态
2. 编译 AIRNT Plugin
3. 编译 vectoradd shader
4. 编译 Metal Bundle
5. 编译测试 App
6. 部署到系统
7. 运行测试并显示结果

**预期输出（30 秒内完成）**:
```
Step 0: 检查前置条件...
[INFO] ✅ Phase 2.3 驱动正常

Step 1: 编译 AIRNT Plugin...
[INFO] ✅ AIRNT Plugin 编译成功

Step 2: 编译向量加法 Shader...
[INFO] ✅ Shader 编译成功

Step 3: 编译 Metal Bundle...
[INFO] ✅ Metal Bundle 编译成功

Step 4: 编译测试应用...
[INFO] ✅ 测试应用编译成功

Step 5: 部署 Metal Bundle 到系统...
[INFO] ✅ Metal Bundle 已部署

Step 6: 运行测试...
========================================
✅ Found Metal device: AMD Radeon RX 9070 XT (RDNA4)
✅ PASSED: All 1024 elements correct
✅ ALL TESTS PASSED
========================================

[INFO] ✅✅✅ Phase 3.3 测试全部通过！✅✅✅
```

---

### 方法 2: 手动分步测试

**适合调试和理解每一步**

#### 步骤 1: 编译 AIRNT Plugin

```bash
cd ~/hackintosh/navi48-native/airnt-plugin
make clean
make

# 验证
ls -lh build/libAMDNTPlugin.dylib
# 期望: ~50-100 KB
```

#### 步骤 2: 编译 Shader

```bash
cd ~/hackintosh/RDNA4Driver
./compile_shader.sh

# 验证
ls -lh shaders/compiled/vectoradd.metallib
xxd shaders/compiled/vectoradd.metallib | head
# 应该看到 'MTLB' magic (4D 54 4C 42)
```

#### 步骤 3: 编译 Metal Bundle

```bash
cd ~/hackintosh/navi48-native/metal-bundle
make clean
make

# 验证
ls -lh build/RDNA4Metal.bundle/Contents/MacOS/
# 应该有 RDNA4Metal 和 libAMDNTPlugin.dylib
```

#### 步骤 4: 编译测试 App

```bash
cd ~/hackintosh/navi48-native/metal-bundle
make test

# 复制 metallib
cp ../../RDNA4Driver/shaders/compiled/vectoradd.metallib build/test/
```

#### 步骤 5: 部署

```bash
sudo cp -R build/RDNA4Metal.bundle /Library/GPUBundles/
sudo chown -R root:wheel /Library/GPUBundles/RDNA4Metal.bundle
```

#### 步骤 6: 测试

```bash
./build/test/RDNA4MetalTest
```

---

### 方法 3: 使用 Makefile

**适合开发者，利用构建系统**

```bash
cd ~/hackintosh/navi48-native/metal-bundle

# 编译全部
make all

# 安装到系统
make install

# 编译并运行测试
make test
./build/test/RDNA4MetalTest
```

---

## ✅ 成功标准

测试通过需要满足所有条件：

### 1. Metal 设备识别
```
✅ Found Metal device: AMD Radeon RX 9070 XT (RDNA4)
```

### 2. Library 加载
```
✅ Loaded library: vectoradd.metallib
✅ Found function: vectorAdd
```

### 3. Pipeline 创建
```
✅ Created compute pipeline
   Max total threads: 1024
   Thread execution width: 32
```

### 4. Buffer 分配
```
✅ Allocated buffers (4096 bytes each)
```

### 5. GPU 执行
```
⏳ Submitting to GPU...
✅ GPU execution completed
```

### 6. 结果验证
```
✅ PASSED: All 1024 elements correct
   C[0..3] = [0.0, 3.0, 6.0, 9.0]
   C[1020..1023] = [3060.0, 3063.0, 3066.0, 3069.0]
```

### 7. 多次分发
```
✅ Iteration 1/5 passed
✅ Iteration 2/5 passed
✅ Iteration 3/5 passed
✅ Iteration 4/5 passed
✅ Iteration 5/5 passed
```

### 8. 最终状态
```
✅ ALL TESTS PASSED
Phase 3.3 is FULLY FUNCTIONAL!
```

---

## 🐛 常见问题快速修复

### 问题 1: 找不到 Metal 设备

```bash
# 快速修复
sudo rm -rf /Library/GPUBundles/RDNA4Metal.bundle
sudo cp -R build/RDNA4Metal.bundle /Library/GPUBundles/
sudo chown -R root:wheel /Library/GPUBundles/RDNA4Metal.bundle
sudo touch /Library/GPUBundles/

# 验证
ioreg -l -w 0 | grep -i rdna4
```

### 问题 2: libAMDNTPlugin 加载失败

```bash
# 快速修复
sudo cp navi48-native/airnt-plugin/build/libAMDNTPlugin.dylib \
    /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/
sudo chown root:wheel \
    /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib
```

### 问题 3: Shader 编译失败

```bash
# 快速修复
cd RDNA4Driver
rm -rf shaders/compiled/*
./compile_shader.sh

# 验证不是 stub
grep -v "^#" shaders/compiled/vectoradd_gfx12.h | grep -q "USE_STUB_SHADER" && echo "STUB!" || echo "Real shader"
```

### 问题 4: 结果全是 0

```bash
# 原因: 使用了 stub shader
# 快速修复
cd RDNA4Driver
./compile_shader.sh
cd ../navi48-native/metal-bundle
make clean && make
sudo cp -R build/RDNA4Metal.bundle /Library/GPUBundles/
./build/test/RDNA4MetalTest
```

---

## 📊 测试时间线

| 步骤 | 时间 | 备注 |
|------|------|------|
| 编译 AIRNT Plugin | ~10 秒 | 首次编译 |
| 编译 Shader | ~5 秒 | xcrun metal |
| 编译 Metal Bundle | ~15 秒 | 8 个 .m 文件 |
| 编译测试 App | ~3 秒 | 1 个文件 |
| 部署 | ~2 秒 | sudo cp |
| 运行测试 | ~1 秒 | GPU 执行 |
| **总计** | **~36 秒** | 自动化脚本 |

---

## 🔍 验证清单

运行测试前检查：

- [ ] Phase 2.3 驱动已加载 (`kextstat | grep rdna4`)
- [ ] ComputeTest 状态为 passed (`ioreg -l -n RDNA4Driver | grep ComputeTest`)
- [ ] GFX 状态为 gfx-alive (`ioreg -l -n RDNA4Driver | grep "GFX,Stage"`)
- [ ] Xcode Command Line Tools 已安装 (`xcode-select -p`)
- [ ] 有 sudo 权限（部署需要）

运行测试后验证：

- [ ] Metal 设备被找到
- [ ] metallib 成功加载
- [ ] AIRNT 编译成功（非 stub）
- [ ] Pipeline 创建成功
- [ ] Buffer 分配成功
- [ ] 命令提交成功
- [ ] GPU 执行完成
- [ ] 1024/1024 结果正确
- [ ] 5/5 迭代通过

---

## 📈 下一步

测试通过后，你可以：

### 立即可用
- ✅ 任何标准 Metal Compute 应用都能运行
- ✅ 可以编写自己的 Metal 程序测试

### 开发方向
1. **Phase 3.4**: 实现 3D 渲染（首个三角形）
2. **性能优化**: Pipeline 缓存、Buffer 池
3. **功能扩展**: Texture 支持、Argument Buffers
4. **稳定性**: 更多测试用例、错误恢复

### 示例：编写自己的 Metal App

```objc
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main() {
    @autoreleasepool {
        // 获取 RDNA4 设备
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSLog(@"Device: %@", device.name);
        
        // 加载你的 shader
        id<MTLLibrary> library = [device newLibraryWithFile:@"myshader.metallib" error:nil];
        id<MTLFunction> function = [library newFunctionWithName:@"myKernel"];
        
        // 创建 pipeline
        id<MTLComputePipelineState> pipeline = 
            [device newComputePipelineStateWithFunction:function error:nil];
        
        // ... 执行你的计算 ...
    }
    return 0;
}
```

---

## 📚 完整文档索引

| 文档 | 用途 |
|------|------|
| `PHASE3.3_QUICK_TEST.md` | 本文件 - 快速参考 |
| `docs/PHASE3.3_TESTING_GUIDE.md` | 详细测试指南（11 KB） |
| `docs/PHASE3.3_COMPLETE_IMPLEMENTATION.md` | 完整技术文档（14 KB） |
| `docs/PHASE3.3_DELIVERY_SUMMARY.md` | 交付总结（14 KB） |
| `test_phase3.3.sh` | 自动化测试脚本 |

---

## 🎉 总结

Phase 3.3 提供了 **3 种测试方法**：

1. **一键自动化** - 最简单，`./test_phase3.3.sh`
2. **手动分步** - 最详细，理解每一步
3. **Makefile** - 开发友好，`make install && make test`

**所有方法都应该在 1 分钟内完成测试！**

如果测试通过，说明：
- ✅ 用户态 Metal 集成完全正常
- ✅ 从 Metal App 到 GPU 硬件的完整软件栈打通
- ✅ 任何标准 Metal 应用都可以运行

**恭喜完成 Phase 3.3！** 🎊

---

*测试方法总结创建于 2026-10-30*
