# Cobalt64 构建产物 (2026-10-23)

构建机: Apple Silicon Mac (macOS 27.0.1), 全部交叉编译为 x86_64。

## 目录

- `kext/Navi48Bringup.kext` — 阶段 A 内核驱动 (v0.0.664, ad-hoc 签名, 固件已嵌入)
- `metal/libvulkan_radeon.dylib` — 阶段 B RADV-Darwin Vulkan 驱动 (mesa f5cb8ee + Cobalt64 9 patches, x86_64, 27 MB)
- `metal/Navi48Metal.bundle.tar` — 阶段 B Metal 设备插件 bundle (含上述 RADV dylib + libn48xlate stub)
- `metal/libn48xlate.dylib` + `metal/n48xlate_stub.c` — **占位 stub**, 不是真正的翻译器
- `tools/esp-kext.sh` — 目标 PC 上的 ESP 安装脚本
- `xlate-patches/` — Windows Docker 构建真正 libn48xlate 所需全部文件
  (含 ARM Mac 提取的 `MacOSX-sdk.tar.gz` 131MB, Dockerfile, patch, n48xlate/)
- `在Windows上构建libn48xlate.md` / `在Windows上构建libn48xlate-ARM-Mac版.md` — 构建指南

## ⚠️ libn48xlate 说明

真正的 `libn48xlate.dylib` (Rust/metal2vulkan) 在本机构建失败:
Apple Silicon 的 Rust proc-macro dylib 加载器拒绝加载交叉编译的 arm64 dylib
("mis-aligned LINKEDIT string pool"), serde_derive 无法构建。

**影响**: Navi48Metal.bundle 的"进程内实时翻译 shader"路径不可用。
bundle 会退回到预编译 spvcache / daemon 路径 (fill-spvcache.py 可攒缓存)。

**在 PC 上若要启用实时翻译**: 在一台 Intel Mac 或 x86_64 Linux (Docker)
上按 `third-party/metal2vulkan-n48/README.txt` 构建真正的 libn48xlate.dylib,
替换 bundle Resources 内的同名文件即可。

## 部署 (目标 x86_64 PC, RX 9070 XT, OpenCore)

1. kext: `sudo tools/esp-kext.sh install` (kext 拷到 EFI/OC/Kexts), config.plist
   添加 Kernel->Add 条目, boot-arg 加 `navi48bringup=1`
2. Metal: 解包 `Navi48Metal.bundle.tar` 到 PC 的 `/Library/GPUBundles/`
3. 重启验证: `kmutil showloaded | grep -i navi48`

详细步骤见原始指南 `Cobalt64-Mac-构建指南.md`。
