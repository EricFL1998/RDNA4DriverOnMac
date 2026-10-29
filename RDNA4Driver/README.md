# RDNA4Driver

RX 9070 XT (Navi 48, `0x1002:0x7550`) 的基础 IOKit 驱动。当前版本完成设备匹配、PCI 初始化和 BAR0/BAR2/BAR5 映射；不包含 Metal 或完整 GPU 加速。

完整修复过程和验证证据见 `../RDNA4_STATUS_REPORT.md`。

## 当前状态

已在 macOS 26.7.1 (25G241) 上验证：

- `probe()` 识别 `0x7550`。
- `start()` 成功。
- BAR0、BAR2、BAR5 映射成功。
- 通过 on-die TMR 和真实 AMD discovery 二进制发现 46 个 IP，GC 为 12.0.1、DCN 为 4.1.0。
- 与 `RDNA4FBMinimal` 从不同 `IOMatchCategory` 同时挂载到同一个 PCI 设备。
- 从 Auxiliary Kernel Collection (AuxKC) 加载。

寄存器 `0x1A18` 仍返回 `0xffffffff`，需要后续调查，不能据此认定硬件初始化已经全部正确。

## 项目结构

```text
RDNA4Driver/
├── src/
│   ├── core/
│   │   ├── RDNA4Driver.cpp
│   │   └── RDNA4Driver.hpp
│   └── kmod_info.c
├── Info.plist
├── Makefile
└── build/RDNA4Driver.kext
```

## 构建

需要 Xcode Command Line Tools 和 `../navi48/MacKernelSDK`：

```bash
make clean
make
plutil -lint build/RDNA4Driver.kext/Contents/Info.plist
```

输出：`build/RDNA4Driver.kext` (x86_64)。

当前仍有两个既有编译警告：参数 shadow 和整数精度转换。

## 安装

最终架构是系统盘/AuxKC，不是 OpenCore 注入。OpenCore 中 `RDNA4Driver.kext` 的 `Kernel -> Add` 条目应保持 `Enabled=false`。

```bash
make install
sudo kmutil load -p /Library/Extensions/RDNA4Driver.kext
```

`make install` 只复制文件并修正所有权。`kmutil load` 可能要求重启或加载审批；按系统提示处理，不要在生产系统上跳过审批。

boot-args 需要包含：

```text
rdna4=1 rdna4-debug=2
```

紧急禁用时将 `rdna4=1` 改为 `rdna4=0`。

完整 GPU 初始化与计算测试用的 boot-args（当前验证通过的组合）：

```text
rdna4=1 rdna4-debug=2 rdna4-psp=1 rdna4-smu=1 rdna4-gfx=1 rdna4-mes-gtt=1 rdna4-compute-test=1
```

| boot-arg | 作用 |
| --- | --- |
| `rdna4-psp=1` | PSP 加载固件（SOS/TOC/RLC/CP/MES/SDMA），后续阶段都依赖它 |
| `rdna4-smu=1` | SMU 握手并上电 GC |
| `rdna4-gfx=1` | GFX 初始化：RS64 程序计数器、RLC CSB/SRM、SH_MEM、门铃、MES、计算队列 |
| `rdna4-mes=0` | 跳过 MES，直接编程 HQD（仅调试；GFX12 正常路径必须走 MES） |
| `rdna4-mec=1` | 非 MES 路径下用 MEC KCQ 而非 GFX ring（仅调试） |
| `rdna4-queue=1` | 非 MES 路径下直接激活 HQD（仅调试） |
| `rdna4-mes-hwr1=1` | 对 SCHED pipe 也发 SET_HW_RESOURCES_1（固件 ≥0x4B，仅调试） |
| `rdna4-mes-gtt=1` | MQD/wptr 放在 GART 系统内存而非 VRAM（上游 KFD/MES 约定） |
| `rdna4-compute-test=1` | 启动时跑 vectorAdd，结果在 `ioreg -r -n RDNA4Driver` 的 `ComputeTest,*` 和内核日志 |
| `rdna4-compute-test=2` | 额外跑逐包探针（每个 SET_SH_REG 单独 IB+fence，`ComputeTest,StepStuckAt`）和 Cobalt64 store_magic 对照着色器（`ComputeTest,Ctrl*`、`ComputeTest,Sh*`） |

`ComputeTest,Stage` 的取值：`passed` / `failed` / `fence-timeout` / `probe-failed`（队列不消费 ring）/ `step-stuck`（某个 PM4 包让 MEC 挂住，见 `StepStuckAt`）。

完整重建 AuxKC 需要与系统构建匹配的 KDK。没有 KDK 时，不要强行使用其他构建的 KDK；本次测试采用禁用 OpenCore 注入、重启清除旧 BootKC 模块后，再用 `kmutil load -p` 重建的方式。

## 验证

```bash
make status
kmutil showloaded --list-only | grep -i rdna4
ioreg -c RDNA4Driver -r -d 1 -w0
sudo log show --last boot --style compact \
  --predicate 'eventMessage CONTAINS[c] "rdna4"'
```

成功日志应包含：

```text
RDNA4: probe: RDNA4 device 0x7550 - SUCCESS, returning this
RDNA4: RDNA4 Driver started successfully
```

## 配套 framebuffer

`../RDNA4FB-minimal` 提供固定模式的控制台 framebuffer，也安装到 `/Library/Extensions` 并从 AuxKC 加载。它不支持 Metal、完整电源管理或原生模式切换。

## 参考

- Cobalt64: https://github.com/Almosst-DEV/Cobalt64
- NVIDIA macOS Driver: https://github.com/nullmoth/nvidia-macos-driver
