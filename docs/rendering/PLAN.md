# Phase 3.4 完整实现计划（方案 A）

**开始日期**: 2026-10-30  
**预计完成**: 2026-12-04（5 周）  
**前置条件**: ✅ Phase 3.3 已验证通过

---

## 🎯 总体目标

实现完整的 3D 渲染支持，从 Compute 扩展到 Graphics：
1. ✅ Render Pipeline State（渲染管线）
2. ✅ Vertex/Fragment Shader 支持
3. ✅ Texture 管理（包括 Render Target）
4. ✅ Graphics Ring（驱动端）
5. ✅ Render Command Encoder
6. ✅ 绘制首个三角形

**最终目标**: 运行标准 Metal 3D 应用，绘制三角形到纹理

---

## 📅 5 周详细计划

### Week 1: 基础架构与 Texture（2026-10-30 至 11-06）

#### Day 1-2: 项目结构与接口设计
**任务**:
- [ ] 创建目录结构
- [ ] 定义所有类的接口（.h 文件）
- [ ] 创建测试框架
- [ ] 编写第一个测试用例

**交付物**:
- `RDNA4Texture.h` - Texture 接口
- `RDNA4RenderPipelineState.h` - Render Pipeline 接口
- `RDNA4RenderCommandEncoder.h` - Render Encoder 接口
- `test/TriangleTest.m` - 三角形测试骨架

#### Day 3-5: Texture 完整实现
**任务**:
- [ ] Texture 分配（VRAM）
- [ ] IOSurface 支持（可选）
- [ ] Pixel Format 支持
- [ ] Mipmap 支持（基础）
- [ ] 纹理上传/下载

**交付物**:
- `RDNA4Texture.m` (~400 行)
- Texture 单元测试

#### Day 6-7: Render Target 配置
**任务**:
- [ ] MTLRenderPassDescriptor 实现
- [ ] Color Attachment 配置
- [ ] Load/Store Actions
- [ ] Clear Color 支持

**交付物**:
- `RDNA4RenderPassDescriptor.h/m` (~200 行)

**Week 1 里程碑**: ✅ Texture 和 Render Target 完全可用

---

### Week 2: Render Pipeline State（2026-11-07 至 11-13）

#### Day 1-3: Pipeline Descriptor
**任务**:
- [ ] MTLRenderPipelineDescriptor 实现
- [ ] Vertex Descriptor（顶点输入布局）
- [ ] Color Attachment Descriptor
- [ ] Blend State（混合状态，基础）

**交付物**:
- `RDNA4VertexDescriptor.h/m` (~200 行)
- `RDNA4RenderPipelineDescriptor.h/m` (~250 行)

#### Day 4-7: Pipeline State 实现
**任务**:
- [ ] Pipeline 创建流程
- [ ] Vertex Shader 编译（扩展 AIRNT）
- [ ] Fragment Shader 编译（扩展 AIRNT）
- [ ] 寄存器配置（VS/PS RSRC）
- [ ] Pipeline 缓存（基础）

**交付物**:
- `RDNA4RenderPipelineState.m` (~500 行)
- Pipeline 创建测试

**Week 2 里程碑**: ✅ 可以创建完整的 Render Pipeline

---

### Week 3: Graphics Ring（驱动端）（2026-11-14 至 11-20）

#### Day 1-2: Graphics Ring 架构
**任务**:
- [ ] GraphicsRing 类设计
- [ ] Ring Buffer 分配（复用 ComputeRing 逻辑）
- [ ] Doorbell 配置
- [ ] Fence 机制（Graphics Fence）

**交付物**:
- `GraphicsRing.hpp` (~150 行)
- `GraphicsRing.cpp` 骨架

#### Day 3-5: VGT/PA/RB 初始化
**任务**:
- [ ] VGT（Vertex Grouper/Tesselator）配置
- [ ] PA（Primitive Assembler）配置
- [ ] RB（Render Backend）配置
- [ ] CB（Color Buffer）配置
- [ ] Viewport/Scissor 设置

**交付物**:
- `GraphicsRing.cpp` VGT/PA/RB 初始化代码 (~400 行)

#### Day 6-7: Graphics PM4 命令
**任务**:
- [ ] SET_CONTEXT_REG（Context 寄存器）
- [ ] DRAW_INDEX_AUTO（绘制命令）
- [ ] EVENT_WRITE_EOP（Graphics Fence）
- [ ] Color Buffer 绑定

**交付物**:
- `GraphicsPM4Builder.hpp/cpp` (~400 行)

**Week 3 里程碑**: ✅ Graphics Ring 可以提交绘制命令

---

### Week 4: Render Command Encoder（2026-11-21 至 11-27）

#### Day 1-3: 基础 Encoder 实现
**任务**:
- [ ] Encoder 状态管理
- [ ] setRenderPipelineState
- [ ] setVertexBuffer/setFragmentBuffer
- [ ] setVertexTexture/setFragmentTexture
- [ ] Viewport/Scissor 设置

**交付物**:
- `RDNA4RenderCommandEncoder.m` 基础实现 (~300 行)

#### Day 4-5: 绘制命令
**任务**:
- [ ] drawPrimitives（非索引绘制）
- [ ] drawIndexedPrimitives（索引绘制）
- [ ] drawPrimitives:indirect（间接绘制，基础）
- [ ] PM4 生成与提交

**交付物**:
- `RDNA4RenderCommandEncoder.m` 绘制命令 (~200 行)

#### Day 6-7: Encoder 完善
**任务**:
- [ ] setViewport/setScissorRect
- [ ] setCullMode/setFrontFacing
- [ ] setDepthStencilState（基础，无实际功能）
- [ ] setBlendColor
- [ ] endEncoding

**交付物**:
- `RDNA4RenderCommandEncoder.m` 完整实现 (~600 行总计)

**Week 4 里程碑**: ✅ 可以编码完整的渲染命令

---

### Week 5: 集成测试与完善（2026-11-28 至 12-04）

#### Day 1-2: 首个三角形测试
**任务**:
- [ ] 编写完整的三角形测试
- [ ] 固定功能 Vertex Shader
- [ ] 固定颜色 Fragment Shader
- [ ] 提交并执行
- [ ] 验证渲染结果

**交付物**:
- `test/TriangleTest.m` 完整实现 (~400 行)
- 三角形渲染成功 ✅

#### Day 3-4: 纹理渲染测试
**任务**:
- [ ] 带纹理的三角形
- [ ] Fragment Shader 纹理采样
- [ ] 纹理坐标传递
- [ ] 验证纹理显示

**交付物**:
- `test/TexturedTriangleTest.m` (~300 行)

#### Day 5: 性能与稳定性
**任务**:
- [ ] 多次渲染测试
- [ ] 不同分辨率测试
- [ ] 内存泄漏检查
- [ ] 性能基准测试

**交付物**:
- 性能报告
- Bug 修复

#### Day 6-7: 文档与总结
**任务**:
- [ ] 编写 Phase 3.4 完整文档
- [ ] 创建测试指南
- [ ] 更新项目状态
- [ ] 创建示例代码

**交付物**:
- `docs/PHASE3.4_COMPLETE_IMPLEMENTATION.md`
- `docs/PHASE3.4_TESTING_GUIDE.md`
- `examples/SimpleTriangle.m`

**Week 5 里程碑**: ✅ Phase 3.4 完全完成

---

## 📊 代码量估算

| 模块 | 文件数 | 行数 | 说明 |
|------|--------|------|------|
| **Texture** | 2 | 400 | MTLTexture 实现 |
| **Render Pipeline** | 6 | 950 | Pipeline + Descriptors |
| **Graphics Ring** | 2 | 600 | 驱动端 Ring 管理 |
| **Graphics PM4** | 2 | 400 | PM4 命令构建 |
| **Render Encoder** | 2 | 600 | 命令编码 |
| **测试** | 3 | 700 | 完整测试 |
| **文档** | 3 | ~2000 行 | Markdown |
| **总计** | **20** | **~3650** | 不含注释 |

---

## 🗂️ 文件结构

```
hackintosh/
├── navi48-native/metal-bundle/src/
│   ├── graphics/                          (新增目录)
│   │   ├── RDNA4Texture.h/m               Week 1
│   │   ├── RDNA4RenderPassDescriptor.h/m  Week 1
│   │   ├── RDNA4VertexDescriptor.h/m      Week 2
│   │   ├── RDNA4RenderPipelineDescriptor.h/m  Week 2
│   │   ├── RDNA4RenderPipelineState.h/m   Week 2
│   │   └── RDNA4RenderCommandEncoder.h/m  Week 4
│   └── test/
│       ├── TriangleTest.m                 Week 5
│       ├── TexturedTriangleTest.m         Week 5
│       └── GraphicsBenchmark.m            Week 5
├── RDNA4Driver/src/hardware/
│   ├── GraphicsRing.hpp/cpp               Week 3
│   └── GraphicsPM4Builder.hpp/cpp         Week 3
├── docs/phase3.4/
│   ├── PHASE3.4_ARCHITECTURE.md           Week 1
│   ├── PHASE3.4_WEEK1_REPORT.md           Week 1
│   ├── PHASE3.4_WEEK2_REPORT.md           Week 2
│   ├── PHASE3.4_WEEK3_REPORT.md           Week 3
│   ├── PHASE3.4_WEEK4_REPORT.md           Week 4
│   ├── PHASE3.4_COMPLETE_IMPLEMENTATION.md Week 5
│   └── PHASE3.4_TESTING_GUIDE.md          Week 5
└── examples/
    ├── SimpleTriangle.m                   Week 5
    └── TexturedQuad.m                     Week 5
```

---

## 🎯 关键里程碑

| Week | 里程碑 | 验证标准 |
|------|--------|---------|
| 1 | Texture 可用 | 能分配、上传、下载纹理 |
| 2 | Pipeline 可创建 | 能创建 Render Pipeline |
| 3 | Graphics Ring 工作 | 能提交 Graphics 命令 |
| 4 | Encoder 完整 | 能编码完整渲染命令 |
| 5 | 首个三角形 | 成功渲染三角形到纹理 ✅ |

---

## 🔧 技术难点与解决方案

### 难点 1: Graphics Ring 初始化
**挑战**: VGT/PA/RB 配置复杂

**解决方案**:
- 参考 Cobalt64 的寄存器配置
- 从 Linux amdgpu 驱动学习
- 先实现固定功能，后期优化

### 难点 2: Vertex/Fragment Shader 编译
**挑战**: AIRNT Plugin 需要扩展

**解决方案**:
- VS/FS 的 AIR bitcode 格式不同
- 需要不同的 SGPR/VGPR 配置
- 参考 Phase 3.1 的实现

### 难点 3: Color Buffer 配置
**挑战**: CB 寄存器设置复杂

**解决方案**:
- 先支持最简单的格式（BGRA8）
- 固定分辨率（800x600）
- 后期扩展

### 难点 4: Render Target 绑定
**挑战**: 纹理地址与 CB 寄存器绑定

**解决方案**:
- 复用 Texture 分配逻辑
- CB_COLORx_BASE 寄存器设置
- 参考 Phase 2.3 的 VRAM 管理

---

## 📚 参考资料

### 硬件文档
- AMD RDNA4 ISA 文档（Vertex/Fragment）
- PM4 Packet Format（Graphics）
- VGT/PA/RB 寄存器手册

### 开源项目
- **Cobalt64** - RDNA4 寄存器逆向
- **Linux amdgpu** - Graphics Ring 实现
- **Mesa** - Vertex/Fragment Shader 编译

### 已有代码
- Phase 2.3 ComputeRing - 复用 Ring 逻辑
- Phase 3.1 AIRNT Plugin - 扩展 Shader 编译
- Phase 3.3 Compute Pipeline - 参考架构

---

## ✅ 成功标准

Phase 3.4 完成的标准：

### 功能性
- [ ] 能创建 Render Pipeline
- [ ] 能编译 Vertex/Fragment Shader
- [ ] 能分配 Render Target 纹理
- [ ] 能编码渲染命令
- [ ] Graphics Ring 正常工作
- [ ] GPU 执行渲染
- [ ] 渲染结果正确

### 测试覆盖
- [ ] 单色三角形测试通过
- [ ] 纹理三角形测试通过
- [ ] 多次渲染稳定
- [ ] 无内存泄漏

### 文档完整
- [ ] 技术文档完整
- [ ] 测试指南详尽
- [ ] 示例代码可运行

### 性能基准
- [ ] 渲染延迟 < 5ms
- [ ] 帧率 > 60 FPS（简单场景）
- [ ] 无明显性能瓶颈

---

## 🚀 启动准备

### 今天完成（2026-10-30）
- [x] 创建 Phase 3.4 计划文档
- [ ] 创建项目目录结构
- [ ] 定义所有接口（.h 文件）
- [ ] 设置开发环境

### 明天开始（2026-10-31）
- [ ] Week 1 Day 1: 开始 Texture 实现
- [ ] 创建第一个测试用例
- [ ] 每日进度跟踪

---

## 📈 进度跟踪

使用 TodoList 跟踪每日进度：

```bash
# 每天更新
echo "Day X: 完成 Y 任务" >> docs/phase3.4/PROGRESS.md
```

每周生成报告：
- Week 1: `PHASE3.4_WEEK1_REPORT.md`
- Week 2: `PHASE3.4_WEEK2_REPORT.md`
- ...

---

**Phase 3.4 方案 A（完整实现）计划已准备就绪！**

**预计 5 周后（2026-12-04）完成首个三角形渲染！** 🎉

---

*计划创建于 2026-10-30*
