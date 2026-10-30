# Phase 2.3 快速参考

## 🚀 一键部署（黑苹果上）

```bash
./deploy_phase2.3_full.sh
```

## ✅ 验证（重启后）

```bash
# 1. 驱动加载
kextstat | grep rdna4

# 2. 测试日志
sudo dmesg | grep "Vector Add Test: PASSED"

# 3. 完整日志
sudo dmesg | grep -A50 "Phase 2.3: Vector Add Test"
```

## 📊 期望结果

```
✅ Vector Add Test: PASSED!
All 1024 elements computed correctly
GPU compute execution successful!
```

## 🔧 故障排查

### Shader 未编译
```bash
cd RDNA4Driver
./compile_shader.sh
```

### 驱动未加载
```bash
sudo kextload /Library/Extensions/RDNA4Driver.kext
```

### Fence 超时
```bash
# 检查 GFX 状态
ioreg -l -n RDNA4Driver | grep "GFX,Stage"
# 必须是 "gfx-alive"
```

## 📚 文档

- **完整部署**: `docs/PHASE2.3_FULL_IMPLEMENTATION.md`
- **故障排查**: 同上文档第 8 节
- **技术细节**: 同上文档第 7 节

## 🎯 成功后下一步

Phase 3.3: Metal Bundle 与驱动集成
- 用户态 Metal App
- Metal.framework 调用
- 完整的 GPU 加速

---

*快速参考创建于 2026-10-29*
