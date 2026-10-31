# RDNA4 Triangle Rendering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 `TriangleTest.m` 通过真实的 Metal 库、图形 pipeline 和 GFX12 命令执行，在 RX 9070 XT 上产生并读回正确的三角形像素。

**Architecture:** 保留 `_MTLDevice` 的系统源码编译，用现有 `MTLBUnpacker` 将函数 AIR 与类型交给 AIRNT。render pipeline 通过 `RDNA4Buffer` 持有 GPU shader；命令编码复用共享 PM4 builder，并以实际读回图像作为完成标准。

**Tech Stack:** Objective-C / Objective-C++17、Metal.framework、现有 AIRNT/LLVM 23、GFX12 PM4。

**Spec:** `docs/superpowers/specs/2026-10-28-rdna4-native-metal-comprehensive-report.md`；用户明确要求继续修复并运行 `TriangleTest.m`。

## Global Constraints

- 不修改 Mesa/asahi，不调用不存在的 `RDNA4Accel` 上传 selector。
- 不复用 compute 的 `COMPUTE_PGM_RSRC1/2` 作为 GS/PS 寄存器。
- 每个真实阻断单独定位、单独验证；库加载或 pipeline 创建不等于绘制成功。
- 保留无关未跟踪文件；提交、推送另等用户明确要求。

## Review Focus

- 真实 MTLB 的条目布局：解析 `NAME/TYPE/MDSZ/OFFT`，验证两个具名 shader，而非猜测数据偏移。
- Vertex/Fragment/Compute 类型：测试 shader 的真实编译结果，不把 graphics 当 kernel。
- Shader GPU buffer 生命周期：验证代码上传与 buffer 读回，释放由 `RDNA4Buffer` 管理。
- 20 字节顶点 stride 与属性：核对 `triangle.metal`，测试实际 PM4，不沿用 16 字节占位。
- 仅背景的错误成功：像素与期望图不符时 `TriangleTest` 必须返回非零。

---

### Task 1: MTLB 函数提取与 AIRNT 编译

**Files:**
- Test: `navi48-native/metal-bundle/test/ShaderCompilationTest.m`
- Modify: `navi48-native/metal-bundle/Makefile`
- Rename: `navi48-native/metal-bundle/src/RDNA4Library.m` → `.mm`
- Modify: `navi48-native/metal-bundle/src/RDNA4Library.mm`

**Interfaces:** `MTLBUnpacker::unpack` 产生 `FunctionEntry{name,type,bitcode}`；MTLB Vertex/Fragment/Compute 分别映射 AIRNT_VERTEX/FRAGMENT/KERNEL。`newFunctionWithName:` 返回真实 `RDNA4Function`。

- [ ] 添加通过 Metal 源码创建 library 并获取 `vertexShader`、`fragmentShader` 的测试；支持 bundle 和 shader 路径参数。
- [ ] 对当前部署运行 `MetalSuperCompileProbe`，确认以两个函数均缺失失败（已于本计划执行前复现）。
- [ ] 用 Objective-C++ 和现有解包器替换两个猜测式扫描方法；解析失败时返回错误，类型按条目映射。
- [ ] 构建 bundle 和 x86_64 AIRNT，安装并签名后重新运行 shader 测试；两个函数必须非空且具有非空 GFX12 代码。

### Task 2: 图形 pipeline 与 shader buffer

**Files:**
- Modify: `navi48-native/metal-bundle/src/RDNA4Device_Complete.m`
- Modify: `navi48-native/metal-bundle/src/graphics/RDNA4RenderPipelineState.m`
- Modify: `navi48-native/metal-bundle/src/graphics/RDNA4RenderPipelineState.h`（如内部属性需要）
- Test: `navi48-native/metal-bundle/test/RenderPipelineTest.m`

**Interfaces:** device 返回 `[[RDNA4RenderPipelineState alloc] initWithDevice:descriptor:error:]`；pipeline 用两个 `id<MTLBuffer>` 持有 vertex/fragment machine code，地址来自 `RDNA4Buffer.gpuAddress`。

- [ ] 添加创建真实 render pipeline、验证两个 shader buffer 内容与地址的测试，先确认旧 client 路径失败。
- [ ] 删除手工 client 分配、上传与释放，改为 shared GPU buffer 所有权。
- [ ] 运行测试并检查 AIRNT 的 GS/PS 元数据；缺失时保留显式失败，不发送猜测寄存器。
- [ ] 对照 `xlat12_repack.h` 和实际 LLVM AMDGPU 输出定位 graphics 元数据差异，再实现经测试的映射。

### Task 3: 渲染编码与图像验证

**Files:**
- Modify: `navi48-native/metal-bundle/src/RDNA4CommandBuffer.m`
- Modify: `navi48-native/metal-bundle/src/graphics/RDNA4RenderCommandEncoder.m`
- Modify: `navi48-native/metal-bundle/src/graphics/RDNA4RenderPM4Builder.mm` 与共享 PM4 builder（以实际缺口为准）
- Test: `navi48-native/metal-bundle/test/TriangleTest.m`

**Interfaces:** command buffer 创建 render encoder 并提交其 PM4；encoder 按 pipeline 的 vertex descriptor 编码 20 字节顶点，而非忽略 buffer。Triangle 以独立顶点覆盖关系和颜色容差判定像素。

- [ ] 为缺失 render encoder、顶点输入/stride、clear 和 draw 的已知边界添加失败测试；先验证 `_pipelineReady` 在 draw 中生效。
- [ ] 接通最小 Triangle 路径，不支持的状态返回错误，不能静默发空命令。
- [ ] 在测试机运行带超时的 `TriangleTest`，检查真实 GPU 完成状态与读回像素。
- [ ] 失败时依据 ring/PM4/shader/像素证据继续定位；未得到正确图像前不报告成功。

### Final verification

- [ ] 运行 `make -C navi48-native/metal-bundle test-registration` 及相关 bundle 构建与 shader/pipeline 测试。
- [ ] 运行 `make -C tools/air2gfx12 test_unpacker`，确认复用解析器没有回归。
- [ ] 运行 Triangle 和已有 GFX、用户客户端、Metal 硬件回归；检查 `git status`，删除本次生成的临时输出。
