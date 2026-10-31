# PA/SC → SPI 状态审计（2026-10-31）

状态：尚未修复三角形。已复现零像素，并找到命令流未初始化的实际非零状态。
测试机在两个屏幕偏移归零的实验中发生 GPU VM fault，MES 恢复失败；后续 GPU 实验等待重启授权。
本轮未修改或部署驱动、Metal bundle、AIRNT 插件。

## 本轮新增的真机证据

以同一条 GFX IB 内的 `SAMPLE_PIPELINESTAT` 采集 draw 前后差值；另用
`PFP_SYNC_ME`、`COPY_DATA(REG → memory)` 在 draw 前采集当前寄存器。
后者可以读到与 PM4 一致的 viewport、PS 程序地址等值，而现有 IORegistry 的
`GfxDiag,Rd,*` 全零读回不能代表 draw 正在使用的状态。

| 实验 | VS | IA primitives | CLIP_IN | CLIP_PRIMS | PS | 非背景像素 |
|---|---:|---:|---:|---:|---:|---:|
| 无 draw 对照 | 0 | 0 | 0 | 0 | 0 | 0 |
| 当前程序，guard-band 四项显式为 0 | 3 | 1 | 1 | 4 | 0 | 0 |
| guard-band 四项改为 1.0 | 3 | 1 | 1 | 4 | 0 | 0 |
| 再归零 WINDOW_OFFSET | 3 | 1 | 1 | 4 | 0 | 0 |
| 再归零 HARDWARE_SCREEN_OFFSET | 超时 | — | — | — | 未取得有效结束采样 | 未验证 |

这些数值是计数器差值；PS 项是 invocation 统计，不是直接的 SPI wave 计数。
`CLIP_PRIMS +4` 不应直接解读成 API 提交了四个三角形。旧实验的
`CLIP_PRIMS +0` 不再适用于当前程序。VS 为 288 B、PS 为 140 B；
顶点缓冲地址 `0x800e002000`，PS 探针地址为 `0x800e002080`。

## draw 前直接读回的未初始化状态

以下寄存器在基线 PM4 中没有写入，且在 draw 前实际非零：

| 地址 | 寄存器 | 读回 |
|---|---|---|
| `0x28200` | `PA_SC_WINDOW_OFFSET` | `0x14106910` |
| `0x28234` | `PA_SU_HARDWARE_SCREEN_OFFSET` | `0x0c62042e` |
| `0x283d0` | `PA_SC_VRS_OVERRIDE_CNTL` | `0x00007084` |
| `0x28c54` | `PA_SC_CONSERVATIVE_RASTERIZATION_CNTL` | `0x136d6917` |
| `0x28644` | `SPI_INTERP_CONTROL_0` | `0x0000284c` |

因此，viewport 的 scale/offset 与 scissor 的 TL/BR 正确，不能排除额外屏幕偏移。
“没有 CLEAR_STATE，所以未写寄存器都是零”这一前提已被实际读回否定。

`PA_SC_VRS_OVERRIDE_CNTL=0x7084` 中 `VRS_SURFACE_ENABLE`、
`RATE_HINT_WRITE_BACK_ENABLE`、`VRS_FEEDBACK_RATE_OVERRIDE` 均为 1。
写 `PA_SC_VRS_INFO=0` 或旁路 VS 的两个 rate combiner，并没有关闭这些功能。
尚不能仅凭此认定 VRS 是唯一根因。

guard-band 的四项地址为 `0x2842c/0x28430/0x28434/0x28438`。
本轮分别显式写 0 和 1.0 都没有恢复 PS；它们不是本轮得到确认的单独根因。
这些测试依次使用同一个硬件上下文，因此后续实验继承前一步写入的值。

## 两项屏幕偏移归零后的故障

仅归零 WINDOW_OFFSET 时仍正常完成、零像素；随后再将
HARDWARE_SCREEN_OFFSET 归零，fence 18 超时，最后完成的 fence 为 17。

- `GCVM_L2_PROTECTION_FAULT_STATUS_LO32 = 0x1713`
- `CID = 11`，上游 Linux 将其标为 `SQG/PC/SC`（共享 client，不能据此只指向 SC）。
- `RW = 0`，`VMID = 0`。
- fault page 对应地址 `0x577d052b9000`。
- `GraphicsQueue,MesResetOk = 0`，queue-only 恢复也失败。

这证明纠正偏移改变了执行路径，但并不证明 PS 已经运行或颜色输出已修复。
下一步需在恢复 GPU 后关闭意外开启的 VRS surface/feedback，再观察 fault、
PS invocation 和入口标记。若仍需排除参数缓存，应使用没有插值指令的常量 PS
配合 NO_PC_EXPORT；只设置 NO_PC_EXPORT 而继续运行 ds_param_load 不能构成完整隔离。

## 另一个寄存器名称问题

GFX12 的 `0x28750` 是 `SX_PS_DOWNCONVERT_CONTROL`，`0x28754` 才是
`SX_PS_DOWNCONVERT`。当前 `GraphicsRegs.hpp` 的宏名与地址不一致。
此前把 `0xff → 0` 解释为“清除保留的格式 0xf”不适用于实际写入的 CONTROL 地址；
Mesa 的 preamble 在 CONTROL 上使用 `0xff`。本轮没有修改这项状态，CB 路径仍待
PS 真正运行后验证。

## 可复现实验材料

本机目录：`/private/tmp/rdna4-pa-sc-audit-20261031/`。

- `StageProbe.m` / `StageProbe`：基于当前 TriangleTest 的临时统计与寄存器读回工具。
  扩展版会在 fence 失败时也输出入口标记、CP 标记、寄存器及原始统计槽，避免丢失挂起证据。
  不把未写入的结束采样当成有效计数。
- `N48_COPY_REGS=1` 开启 draw 前寄存器读回。
- `N48_PROBE_REGS=地址=值,...` 在 draw 紧前方应用覆盖，避免被后续 pipeline 设置覆盖。
- `N48_PROBE_LIBRARY=路径` 选择诊断 metallib。
- `constant.metal` / `constant.metallib`：已编译的常量红色 PS 对照；尚未在真机运行。
- `original.metallib` / `original-TriangleTest`：本次测试使用的原始真机输入。
- `pa-sc-stats-control.log`、`pa-sc-stats-baseline.log`、`pa-sc-stats-guardband.log`、
  `pa-sc-stats-readback.log`、`pa-sc-window-offset.log`、`pa-sc-both-offsets.log`、
  `pa-sc-timeout.ioreg`：完整采样记录。

初版 StageProbe 的无 draw、正常 draw 和 COPY_DATA 路径均已真机运行；最新扩展版
与常量 PS 仅完成本机编译，尚未完成重启后的验证。TriangleTest 的现有退出条件仅
要求非背景像素数达到阈值，最终仍应另外核对形状与插值颜色，不能将该退出码单独
作为完整渲染正确的证据。

真机部署文件 SHA-256：

```text
RDNA4Metal:
19e667e2da823e81dbc8ff2252a2c01695c7fbeb5746e5820c0279df5a43159f
libAMDNTPlugin.dylib:
94b6f1351bb8efca06a3af4cdea5dca40c33ae1eee847df7111fec458ce54c06
triangle.metallib:
b8539f89c14134cba39a3b131256e96ebd6bebce301f18106baff8ccac951c0c
```

## 参考实现

- Mesa `radv_query.c`：`pipeline_statistics_indices` 和 `SAMPLE_PIPELINESTAT`。
- Mesa `radv_cmd_buffer.c`：`radv_emit_override_vrs_state`、`radv_emit_msaa_state`、
  `radv_emit_guardband_state`。
- Mesa `gfx12.json`：上述寄存器的地址与字段。
- Linux [`gfxhub_v12_0.c`](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/gfxhub_v12_0.c)：fault CID 名称。
- Mesa 源码快照及寄存器 JSON 已保存于同一实验目录；NGG/perfcounter 补充源码固定在
  mirror commit `fd8ef2befafd01190e33f12dbfd109b924c8ebc3`，其他文件从该 mirror 的 main 获取。
