# Phase 3.3 快速测试参考

## 🚀 一键测试（推荐）

```bash
chmod +x test_phase3.3.sh
./test_phase3.3.sh
```

## ✅ 成功输出

```
✅✅✅ Phase 3.3 测试全部通过！✅✅✅
用户态 Metal 集成完全正常！
任何标准 Metal 应用现在都可以在 RDNA4 GPU 上运行！
```

## 🔧 手动测试步骤

### 1. 编译所有组件

```bash
# AIRNT Plugin
cd navi48-native/airnt-plugin && make

# Shader
cd ../../RDNA4Driver && ./compile_shader.sh

# Metal Bundle
cd ../navi48-native/metal-bundle && make

# 测试 App
make test
```

### 2. 部署

```bash
sudo cp -R build/RDNA4Metal.bundle /Library/GPUBundles/
sudo chown -R root:wheel /Library/GPUBundles/RDNA4Metal.bundle
```

### 3. 运行测试

```bash
./build/test/RDNA4MetalTest
```

## 🐛 常见问题

### 找不到 Metal 设备
```bash
# 检查 Bundle
ls -la /Library/GPUBundles/RDNA4Metal.bundle

# 检查日志
sudo dmesg | grep -i metal
```

### libAMDNTPlugin 加载失败
```bash
# 确认存在
ls -la /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib

# 检查权限
sudo chown root:wheel /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib
```

### 结果验证失败
```bash
# 检查是否使用 stub
grep USE_STUB_SHADER RDNA4Driver/shaders/compiled/vectoradd_gfx12.h

# 重新编译真实 shader
cd RDNA4Driver && ./compile_shader.sh
```

## 📊 期望性能

- 命令提交: < 1 ms
- 向量加法 1K: < 0.01 ms  
- 多次分发: > 1000 Hz

## 📚 详细文档

- **完整测试指南**: `docs/PHASE3.3_TESTING_GUIDE.md`
- **技术文档**: `docs/PHASE3.3_COMPLETE_IMPLEMENTATION.md`
- **交付总结**: `docs/PHASE3.3_DELIVERY_SUMMARY.md`

---

*快速参考创建于 2026-10-30*
