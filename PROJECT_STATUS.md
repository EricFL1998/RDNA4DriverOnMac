# RDNA4 macOS 驱动项目 - 当前状态

**最后更新**: 2026-10-29  
**当前阶段**: Phase 2.3 框架完成，等待黑苹果部署

---

## 📊 整体进度

```
Phase 1: 离线编译器                    ████████████████████ 100%
Phase 2.1: IOAccelerator 服务           ████████████████████ 100%
Phase 2.2: WindowServer 硬件加速        ████████████████░░░░  80% (可验证)
Phase 2.3: Compute Kernel 框架          ████████████████████ 100%
Phase 2.3: 首个真实 Kernel 执行         ████████░░░░░░░░░░░░  40% (下一步)
Phase 3: Metal Bundle 集成              ████████░░░░░░░░░░░░  40% (骨架完成)
Phase 4: 生产级优化                     ░░░░░░░░░░░░░░░░░░░░   0%
```

---

## ✅ 已完成（可用）

### Phase 1: 离线工具链 (100%)

**位置**: `tools/air2gfx12/`

- ✅ CLI 编译器：`.metallib` → GFX12 汇编/ELF
- ✅ 5 个单元测试：100% 通过
- ✅ 45 个生产着色器：100% 通过（0.2 秒/着色器）
- ✅ 完整文档与实用脚本

**验收**: 8/8 项全部通过

### Phase 2.1: IOAccelerator 服务 (100%)

**位置**: `RDNA4Accel/`

- ✅ 继承自 `IOAccelerator`（已修复）
- ✅ 发布设备属性到 IORegistry
- ✅ IOUserClient 接口（stub）
- ✅ 完整 Makefile 与文档

**状态**: 已在黑苹果安装，运行正常

### Phase 2.3: Compute Kernel 框架 (100%)

**位置**: `RDNA4Driver/` (新增功能)

- ✅ ComputeRing 类（Ring Buffer + Doorbell + Fence）
- ✅ Buffer 分配 API（VRAM 管理）
- ✅ PM4 命令提交接口
- ✅ 生命周期集成（start/stop/cleanup）
- ✅ 485 行新代码 + 完整文档

**状态**: 代码完成，等待黑苹果部署测试

### Phase 3: 用户态运行时 (40%)

**位置**: `navi48-native/`

- ✅ AIRNT Plugin：`libAMDNTPlugin.dylib`（编译器封装）
- ✅ Metal Bundle 骨架：`RDNA4Metal.bundle`（MTLDevice 接口）
- 🔜 实际 GPU 通信（Phase 3.3）

---

## 🔄 进行中

### Phase 2.2: WindowServer 硬件加速 (80%)

**目标**: 让 WindowServer 识别硬件加速器

- ✅ RDNA4Accel 注册为 IOAccelerator
- ✅ 设备属性发布完成
- 🔄 等待验证 `gpu 0x0ae3 accel != 0x0`
- 🔜 确认 WindowServer 退出 CPU 软解

**下一步**: 在黑苹果上检查 WindowServer 日志

### Phase 2.3: 首个真实 Compute Kernel (40%)

**目标**: 向量加法 `result[i] = a[i] + b[i]` 在 GPU 上执行

**已完成**:
- ✅ ComputeRing 框架
- ✅ Buffer 分配
- ✅ PM4 提交接口
- ✅ Fence 同步机制

**待完成**:
- 🔜 集成 air2gfx12 编译 shader
- 🔜 集成 PM4Builder 生成命令流
- 🔜 Shader 上传到 VRAM
- 🔜 配置 COMPUTE_PGM_RSRC1/2
- 🔜 DISPATCH_DIRECT 提交
- 🔜 读回结果验证

**预计时间**: 1-2 天

---

## 📋 待办事项（优先级排序）

### 🔴 高优先级（立即执行）

1. **部署 Phase 2.3 到黑苹果**
   - 运行 `./deploy_phase2.3.sh`
   - 验证 ComputeRing 初始化
   - 检查系统日志

2. **编译向量加法 Shader**
   - 使用 `tools/air2gfx12`
   - 生成 GFX12 机器码
   - 提取 SGPR/VGPR 元数据

3. **集成 PM4Builder 到 RDNA4Driver**
   - 复制 `tools/air2gfx12/src/PM4Builder.hpp/cpp`
   - 添加到 Makefile
   - 生成 DISPATCH_DIRECT 封包

### 🟡 中优先级（本周完成）

4. **实现 Shader 上传**
   - 分配 VRAM Buffer 存放机器码
   - memcpy 到 GPU 可访问地址
   - 配置 COMPUTE_PGM_LO/HI

5. **完整的 testVectorAdd() 实现**
   - 添加真实 Shader 调用
   - 提交 DISPATCH_DIRECT
   - 等待 Fence
   - 读回 VRAM 验证结果

6. **添加 boot-arg 触发**
   - `rdna4-test-compute=1` 启动时运行测试
   - 详细日志输出
   - 错误恢复机制

### 🟢 低优先级（未来迭代）

7. **真实 Buffer 分配器**
   - 替换简单线性分配
   - 实现 Buddy 或 Slab 算法
   - 碎片管理

8. **多 Ring 支持**
   - Graphics Ring
   - DMA Ring
   - 优先级调度

9. **Phase 3.3 完整实现**
   - Metal Bundle 与驱动集成
   - 用户态命令提交
   - 完整的 Buffer 管理

---

## 🗂️ 文件结构

```
hackintosh/
├── tools/
│   └── air2gfx12/                 ✅ Phase 1 完成
│       ├── bin/air2gfx12
│       ├── src/ (1068 行)
│       ├── tests/ (8/8 通过)
│       └── README.md
│
├── RDNA4Driver/                   ✅ Phase 2.3 框架完成
│   ├── src/
│   │   ├── core/
│   │   │   ├── RDNA4Driver.hpp   ✏️ 已修改
│   │   │   └── RDNA4Driver.cpp   ✏️ 已修改 (+485 行)
│   │   └── hardware/
│   │       ├── ComputeRing.hpp   🆕 新增
│   │       └── ComputeRing.cpp   🆕 新增
│   ├── Makefile                  ✏️ 已修改
│   └── build/RDNA4Driver.kext    📦 待部署
│
├── RDNA4Accel/                    ✅ Phase 2.1 完成
│   ├── src/
│   │   ├── RDNA4Accel.hpp        ✏️ 已修复 (IOAccelerator)
│   │   └── RDNA4Accel.cpp        ✏️ 已修复
│   └── build/RDNA4Accel.kext     📦 已部署
│
├── navi48-native/                 ⚡ Phase 3 用户态完成
│   ├── airnt-plugin/             ✅ AIRNT 编译器插件
│   └── metal-bundle/             ✅ Metal Bundle 骨架
│
├── docs/
│   ├── PHASE1_COMPLETION_REPORT.md
│   ├── PHASE3_USERSPACE_COMPLETION_REPORT.md
│   ├── PHASE2.3_IMPLEMENTATION.md          🆕
│   ├── DEPLOYMENT_CHECKLIST_PHASE2.3.md    🆕
│   └── PHASE2.3_COMPLETION_SUMMARY.md      🆕
│
├── QUICK_START_PHASE2.3.md        🆕 快速开始
├── deploy_phase2.3.sh             🆕 部署脚本
└── PROJECT_STATUS.md              🆕 本文件
```

---

## 📈 代码统计

| 模块 | 文件数 | 代码行数 | 测试 | 文档 |
|------|--------|---------|------|------|
| air2gfx12 | 11 | 1068 | 8/8 ✅ | 完整 ✅ |
| RDNA4Driver | 14 | ~3500 | 手动 | 部分 |
| RDNA4Accel | 2 | ~800 | 手动 | 完整 ✅ |
| AIRNT Plugin | 2 | ~500 | 基础 | 完整 ✅ |
| Metal Bundle | 2 | ~800 | 无 | 完整 ✅ |
| **总计** | **31** | **~6700** | | |

---

## 🎯 近期目标

### 本周（Phase 2.3 完整实现）

- [x] ComputeRing 框架
- [ ] 部署到黑苹果测试
- [ ] 集成 air2gfx12 编译
- [ ] 集成 PM4Builder
- [ ] 首个向量加法成功

### 下周（Phase 3.3 开始）

- [ ] Metal Bundle 实际 GPU 通信
- [ ] 用户态命令提交
- [ ] Buffer 管理完善
- [ ] 首个 Metal App 运行

### 本月（Phase 3.4）

- [ ] 3D 三角形渲染
- [ ] Vertex + Fragment Shader
- [ ] Render Target 配置
- [ ] 简单图形应用运行

---

## 🤝 协作建议

### 如果你要参与开发

1. **阅读文档**：
   - `ROADMAP_NATIVE_METAL.md` - 总体路线
   - `docs/PHASE2.3_IMPLEMENTATION.md` - 当前技术细节
   - `QUICK_START_PHASE2.3.md` - 快速上手

2. **验证环境**：
   - 黑苹果已启动（四档全通）
   - SSH 可访问
   - KDK 已安装

3. **开始贡献**：
   - Phase 2.3 完整实现（高优先级）
   - 文档补充与改进
   - 测试用例编写

---

## 📞 联系方式

- **项目地址**: (待补充)
- **问题反馈**: 提交 GitHub Issue
- **技术讨论**: (待补充)

---

*项目状态更新于 2026-10-29*
