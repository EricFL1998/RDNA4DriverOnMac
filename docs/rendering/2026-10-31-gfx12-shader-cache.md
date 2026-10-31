# GFX12：VS 入口标记缺失的缓存根因

2026-10-31，RX 9070 XT / GFX1201 实机。

## 结论

`GraphicsPM4Builder::acquireMem()` 将旧版 `CP_COHER_CNTL` 的 action bits
放入了 GFX12 `ACQUIRE_MEM` 最后一字的 `GCR_CNTL`。原值 `0x28c00000`
没有设置 `GLI_INV` 等实际缓存操作；在同一 GPU 地址上传新 shader 后，GPU
仍可执行先前缓存的 ISA。因此，“新入口标记没有写出”不能证明没有启动顶点波。

已将该字修正为 `0x0000c181`，并部署重建后的用户态 RDNA4Metal bundle。
没有修改 shader 地址、RSRC、stage enable、GE_CNTL、ring 配置或 CONTEXT_CONTROL。
本次没有替换内核驱动或重启。

## 实机区分实验

在 draw 前后分别发送 `PIPELINESTAT_START`、`SAMPLE_PIPELINESTAT`，最后停止统计。
两份结果位于独立 GPU buffer，先填充哨兵；前后另有 CP WRITE_DATA 标记。
采样包为 `C0024600 0000021E address_lo address_hi`。
统计索引沿用 Mesa `radv_query.c`：IA vertices=7，IA primitives=6，VS=3，PS=0，
clip input=2，clip primitives=1。TASK 槽未由硬件改写，不能当作有效的零计数。

以下实验均使用 `N48_FEATURE_MASK=0x19f`，显式发送 `NUM_INSTANCES(1)`。
原默认 `0x9f` 是否能在全新启动状态正确建立 instance count，不属于本次验证结果。

| 实验 | GCR_CNTL | 上传的入口标记 | IA verts / prims 增量 | VS 增量 | vertex[0] 实际值 |
|---|---|---|---|---|---|
| 不发送 draw | 原值 | `0x3fc00000` | 0 / 0 | 0 | `0x00000000` |
| 原 draw | 原值 | `0x3fc00000` | 3 / 1 | 3 | `0x00000000` |
| 仅替换 GCR 字 | `0x0000c181` | `0x3fc00000` | 3 / 1 | 3 | `0x3fc00000` |
| 复用地址，改标记为 6.0f | 原值 | `0x40c00000` | 3 / 1 | 3 | **`0x3fc00000`（旧代码）** |
| 部署修复版，无 GCR 覆盖 | `0x0000c181` | `0x40c00000` | 3 / 1 | 3 | **`0x40c00000`** |
| 修复版再改回 1.5f | `0x0000c181` | `0x3fc00000` | 3 / 1 | 3 | **`0x3fc00000`** |

复用地址实验中的 VS 地址均为 `0x800e000000`。修改标记时只替换已反汇编确认的
`v_mov_b32 v4, 0x3fc00000` 的 literal；其余代码、资源占用和流水线配置保持不变。
这比单独观察 VS invocation 计数更直接地证明了执行旧代码及修复后的代码更新。

## 编码与回归

修复后的 `ACQUIRE_MEM`：

```text
C0065800 00000000 FFFFFFFF 00FFFFFF 00000000 00000000 0000000A 0000C181
```

`GCR_CNTL` 字段：GLI_INV=ALL [1:0]，GLK_INV [7]，GLV_INV [8]，
GL2_INV [14]，GL2_WB [15]。GFX12 不添加旧 GL1/metadata 操作。
使用 PFP acquire、全地址范围和默认 parallel sequence；这条命令执行缓存操作，
并不替代调用方对已经运行的 shader 的完成同步。

字段与包格式依据本地 Mesa 的 `src/amd/common/ac_barrier.c`、
`src/amd/common/ac_cmdbuf_cp.c` 及 `build-x86_64/src/amd/common/amdgfxregs.h`。

- 新增包级回归测试先报告唯一失败：`got 0x28c00000 want 0x0000c181`。
- 修复后 `make -C RDNA4Driver test-pm4` 全部通过，包括 user-client lifetime、
  VRAM allocator、PM4、MES API、graphics/compute ring 和 GART 测试。
- `make -C navi48-native/metal-bundle -j4` 构建成功；独立只读审查无本次修复问题。
- 本机构建与实机安装的 RDNA4Metal SHA-256 均为
  `5b4eb346bdd4bb737cad2870b5ecba9b18dbaf26c4540afc35b8a902230a3998`。

## 当前边界

重新编译当前 `TriangleTest.m`、使用修复版 bundle 实测，入口标记已写出
`0x3fc00000`，fence 正常完成。`TriangleTest` 仍返回 **1**：中心像素是背景
`0xFF333333`。探针的 PS 增量仍为 0，驱动四个 SE 的 attribute ring 哨兵仍未变化；
最新探针快照 `RptrWb=SwWptr=2477`、`FaultLo=0`。

因此本次修复的是 **shader 更新未对 GPU 生效**，不是完整三角形渲染。
下一步应从已经执行的 NGG shader 的系统输入、分支和导出继续追踪。
入口探针本身写入顶点 buffer 的前 16 字节，会破坏原顶点数据，不能用于正常像素验收。
用户态 ring dump 当前返回空数据；ring 未变化结论来自驱动哨兵，不能用 `0/0` 的
用户态 dump 推断。TriangleTest 的旧日志文字 `marker would be deadbeef` 也不是实际标记值。

## 实验材料位置

- 本机临时目录：`/tmp/rdna4-front-end-20261031/`，包括 `FrontEndProbe.m`、VS ELF/机器码、
  red/green 测试日志、bundle 构建日志及 `evidence/` 实机输出。
- 实机临时目录：`/tmp/rdna4-front-end/`，包括 probe、各轮日志、修复版 bundle 主程序。
- 实机原主程序备份：`/tmp/rdna4-front-end/RDNA4Metal.before-gcr`。

临时目录不是持久存储；上表保留了本轮关键结果。
