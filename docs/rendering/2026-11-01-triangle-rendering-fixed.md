# 三角形渲染打通记录（2026-11-01）

状态：**常量色与插值色三角形都能渲染**，`TriangleTest` 连续三次
`mismatches: 0`、`Rendered non-clear pixels: 60000`、`center=0xFF40407F`。

## 一、PA/SC 不产出片元

gfx12 没有 CLEAR_STATE，下面三组上下文寄存器在每次冷启动后都是随机值
（两次启动读到过 `PA_SU_HARDWARE_SCREEN_OFFSET=0x0c62040e`、
`DB_EQAA=0x00134504`、`DB_Z_INFO=0x100764dc`）。驱动此前一个都没写，
于是零片元或 VM fault。现已全部写入 `GraphicsPM4Builder::kPreamble`：

| 寄存器 | 值 | 作用 |
|---|---|---|
| `PA_SU_HARDWARE_SCREEN_OFFSET` (0x28234) | 0 | 屏幕坐标平移；随机值把三角形移出目标 |
| `PA_SC_WINDOW_OFFSET` (0x28200) | 0 | 同上 |
| `PA_SC_CONSERVATIVE_RASTERIZATION_CNTL` (0x28c54) | 0x100000 | `NULL_SQUAD_AA_MASK_ENABLE` |
| `DB_Z_INFO` / `DB_STENCIL_INFO` | 0 / 0x20000000 | 无深度附件时必须是 INVALID |
| `DB_RENDER_OVERRIDE` / `_OVERRIDE2` | 0 / 0x08000000 | 随机值的 FORCE_Z_DIRTY 会让 DB 写随机地址 |
| `DB_EQAA` / `DB_ALPHA_TO_MASK` | 0 / 0x18700 | 随机值声称 32 采样并开 alpha-to-mask，DB 会吞掉全部片元 |

`setColorAttachment` 另外补写 `CB_COLOR0_BASE_EXT`（地址 [47:40]）和
`CB_COLOR0_ATTRIB3`（`MIP0_DEPTH=1 | RESOURCE_TYPE=2D | COLOR_SW_MODE=LINEAR`），
缺这两项时首批片元会写到一个高 8 位随机的地址从而 VM fault。

真机验证方式：`StageProbe`（`/tmp/rdna4-pa-sc-audit-20261031/`）在 draw 前用
`N48_PROBE_REGS` 逐组覆盖，观察 `SAMPLE_PIPELINESTAT` 的 PS 计数与帧缓冲。

## 二、插值颜色为黑

`ds_param_load` 读到 0。成因链（逐项在真机上排除）：

1. attribute ring 的 shader 写入路径在本机不落地 —— 同一 ring 内**常量地址**
   的 store 能读到，**任何计算地址**的 store（`global_store` 的 SGPR-soffset
   与 64 位 vaddr 两种形式、以及 Mesa 原样的
   `amdgcn.struct_buffer_store` + `buffer_store_b128 idxen`）都读不到。
2. `SPI_SHADER_GS_OUT_CONFIG_PS` 的 `NUM_INTERP` 实际是 1（`0x800` 的 bit 11），
   参数个数链路（插件→Library→Encoder→PM4）全程正确。

因此当前实现绕开硬件参数缓存：

- 顶点阶段把 varying 写进 **vertex buffer** 的固定 slot：
  `vb + 0x40 + param*0x100 + vertex*16`（shader 对 VB 的写已验证可落地）；
- 顶点阶段同时给 `exp pos0` 带上 DONE（Mesa `ac_nir_export_position` 的做法）；
- 像素阶段通过 PS user SGPR 拿到 VB 地址（`RSRC2_PS.USER_SGPR=4`，地址放在
  user data word 2..3，正好对应 LLVM 片元约定里第一个 inreg 参数所在的 s2），
  用与硬件相同的公式插值：`v = v0 + I*(v1-v0) + J*(v2-v0)`；
- 图元顶点顺序相对索引旋转一位，读取 slot 时对应旋转。

限制：这条路径假定三个顶点就是索引 0/1/2，且只有一个 varying；属于当前
单三角形管线范围内的实现，不是通用的 NGG 参数路径。

## 三、颜色通道顺序

`CB_COLOR0_INFO.COMP_SWAP` 之前对 BGRA8 目标置 1，写出的字节序与调用方期望相反
（`TriangleTest` 把第一个字节当 R）。改为不交换后 `mismatches` 从 51754 降到 0。

## 四、复现命令

```bash
make -C tools/air2gfx12 test                 # 6 个测试
make -C RDNA4Driver test-pm4                 # 394 项断言
make -C navi48-native/metal-bundle           # bundle
make -C navi48-native/airnt-plugin TARGET_ARCH=x86_64 \
     LLVM_CONFIG=/tmp/n48-llvm-x86_64/build/bin/llvm-config
# 目标机
cd /tmp/n48probe && sudo ./TriangleTest      # mismatches: 0
```
