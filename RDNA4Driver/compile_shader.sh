#!/bin/bash
# 编译 vectorAdd 为 GFX12 机器码并生成驱动头文件。
#
# 注意：早期版本用 Metal (xcrun metal/metallib) + air2gfx12。该路径不可用于
# 真实 dispatch：Apple 的 AIR 后端会把 [[thread_position_in_grid]] 当成普通
# kernarg 参数，所有线程读取同一个下标。现在改用标准 AMDGPU 路径
# （clang -target amdgcn-amd-amdhsa -mcpu=gfx1201），线程 ID 来自硬件
# (ttmp9/v0)，参数通过 s[0:1] 指向的 kernarg 传入。
#
# 依赖：带 AMDGPU 后端的 LLVM（clang / llvm-objcopy / llvm-readelf）。
#   Homebrew:  brew install llvm
#   官方发布:  https://github.com/llvm/llvm-project/releases
#
# 用法：
#   ./compile_shader.sh
#   CLANG=/path/to/clang OBJCOPY=/path/to/llvm-objcopy \
#   READELF=/path/to/llvm-readelf ./compile_shader.sh

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
exec "$HERE/shaders/build_shader.sh"
