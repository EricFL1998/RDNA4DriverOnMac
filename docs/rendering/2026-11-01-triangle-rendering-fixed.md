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

## 五、2026-11-01 晚：硬件参数路径打通

结论：**"SGPR soffset 编码被这块芯片丢弃"不成立**，硬件参数路径本身一直可用。
之前失败是两个与寻址无关的原因，外加一种不可靠的验证方式。

### 证据：SADDR 形式是好的

- 已交付的顶点缓冲路径本身就发 `global_store_b128 v1, v[3:6], s[16:17]`（SADDR），
  且 `mismatches: 0`；
- 让着色器写完 ring 再**自己读回**，SADDR 与 64 位 VGPR 两种形式都读到了刚写的标记。

CPU 侧通过驱动窗口读 ring 不可靠：ring 内容在 GPU 缓存里，未回写时窗口读到的
是旧数据（本次实测两次全 0，而着色器自己读回正常）。判断"是否落地"必须用
着色器内回读，或直接看像素阶段的读取结果。

### 真正的两个根因

1. **`ds_param_load` 的隐式 M0 是 primitive mask**（像素阶段的 `s4`，
   `ac_shader_args.h` 里 `prim_mask` 位于 user SGPR 之后的固定槽位）。固定
   `m0 = 0` 时每个 lane 都读到同一条顶点记录，整块三角形呈现单一颜色。
2. **`ds_param_load` 只取本 lane 的顶点值，不做插值**。Mesa
   （`aco_lower_to_hw_instr.cpp` 的 `emit_interp_instr_gfx11`）的序列是
   `ds_param_load` → `v_interp_p10_f32_inreg(p, I, p)` → `v_interp_p2_f32_inreg(p, J, tmp)`，
   用像素 ABI 里的 I/J 做重心插值。

### ring 写入地址

`ac_nir_store_parameters_to_attr_ring()`：一个参数一个 vec4，

```
ring_base + ((s5 & 0x7fff) << 9) + param * 512 + (export_thread % 32) * 16
```

- `s5` 是 `gs_attr_offset`，**不是常数**：实测同一张卡上依次为 `0x0 / 0xB00 /
  0x1600`，每次 `+0xB00`，即 `+0x160000` 字节——每个 SE 的 attribute ring 就是
  `0x160000`，它选的是 SE 切片。早先"四个切片每轮丢一个"就是这个偏移在移动。
- `s8..s11` 是 USER_DATA word 0..3（ring 描述符），实测
  `0e010000 / c0000080 / 00160000 / 0043ffac`；word 1 高位的 `0xC0000000` 是
  `SWIZZLE_ENABLE`，当地址用必须 `& 0xffff`。
- 参数之间是 512 字节（`index_stride * element_size = 32 * 16`），不是 16。

### gfx12 的等待

`gfx12` 把等待拆开：`s_waitcnt`（`0xbf890000`）不含 store 计数，写 ring 之后
要用 `s_wait_storecnt 0x0`（`0xbfc10000`）保证在 primitive export 前提交。

### 交付行为

- 默认走**硬件参数路径**：顶点阶段写 ring，像素阶段
  `ds_param_load` + `v_interp`；TriangleTest `mismatches: 0`，连续三次一致。
- `N48_VB_PARAM=1` 回退到原来的顶点缓冲 + 软件插值路径（同样 `mismatches: 0`）。

### 内联汇编不可用

本管线没有目标汇编解析器：

```
LLVM ERROR: Inline asm not supported by this streamer because we don't have an asm parser for this target
```

需要强制 64 位 VGPR 地址形式时，用"发散 load 把基址拉进 VGPR"的办法（基址经一次
按 lane 取址的 `global_load` 往返，指令选择无法再拆成 SGPR saddr）。

工具：`tools/n48-auto-test/ring-scan.sh`（跑一次 StageProbe → 解析该次 ring 基址
→ 按该基址扫描）。
