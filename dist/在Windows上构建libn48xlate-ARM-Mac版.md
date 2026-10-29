# 在 Windows 上构建 libn48xlate.dylib (ARM Mac 提取 SDK 版)

> **场景**: 只有一台 Apple Silicon (ARM) Mac + 一台 Windows PC。
> **方案**: 从 ARM Mac 提取 macOS SDK → 拷到 Windows → Windows Docker (osxcross) 交叉编译 x86_64 dylib。
>
> **全程不需要 Intel Mac, 不需要 Linux 物理机, 不需要 Rosetta。**

---

## 0. 原理 (为什么这样能行)

```
ARM Mac (提取 SDK)                Windows PC (Docker)
┌──────────────────┐             ┌──────────────────────────────┐
│ MacOSX.sdk (ARM) │ ──scp/U盘──▶│ rust:1-bookworm + osxcross   │
│ 含 x86_64 系统库  │             │ + MacOSX-sdk.tar.gz          │
└──────────────────┘             │                              │
                                 │ cargo build --target         │
                                 │   x86_64-apple-darwin        │
                                 │        ↓                     │
                                 │ libn48xlate.dylib (x86_64)   │
                                 └──────────────────────────────┘
```

> macOS SDK 是 "fat" 的, 同时包含 arm64 + x86_64 系统库 (libSystem.dylib 等),
> 所以从 ARM Mac 提取的 SDK 完全可用于 x86_64 交叉编译。

---

## 1. ARM Mac: 提取 SDK 并打包

在 ARM Mac 的终端执行:

```bash
# 1. 找到 SDK 路径
SDK=$(xcrun --sdk macosx --show-sdk-path)
echo "SDK path: $SDK"
# 典型输出: /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
#      或: /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk

# 2. 打包 SDK (大小约 200-400 MB, tar.gz 压缩后约 50-100 MB)
cd /tmp
tar -czf MacOSX-sdk.tar.gz -C "$(dirname "$SDK")" "$(basename "$SDK")"

# 3. 验证 tar 包
ls -lh MacOSX-sdk.tar.gz
tar -tzf MacOSX-sdk.tar.gz | head -5
# 应看到: MacOSX.sdk/usr/lib/libSystem.dylib 等条目

# 4. 拷到 Windows (三选一):
#    a) 共享文件夹 (推荐):
mkdir -p ~/shared/n48-build
cp MacOSX-sdk.tar.gz ~/shared/n48-build/
#    b) scp 到 Windows (Windows 需开 SSH 服务):
#       scp MacOSX-sdk.tar.gz <Windows用户名>@<Windows_IP>:C:/n48-xlate/
#    c) U盘 / 网盘
```

---

## 2. Windows: 准备构建目录

在 Windows 上打开 PowerShell,执行:

```powershell
# 1. 创建构建目录
mkdir C:\n48-xlate
cd C:\n48-xlate

# 2. 从 dist 拷贝 3 个基础文件 (根据实际 dist 位置调整)
xcopy /E /I E:\work\hackintosh\dist\dist\xlate-patches C:\n48-xlate

# 3. 把 MacOSX-sdk.tar.gz 拷进来 (从 ARM Mac 的共享文件夹 / U盘 / scp)
copy \\Mac\shared\n48-build\MacOSX-sdk.tar.gz C:\n48-xlate\

# 4. 确认目录内容
dir C:\n48-xlate
# 应看到:
#   0001-metal2vulkan-changes-for-n48xlate.patch
#   Dockerfile (会被下面的版本替换)
#   MacOSX-sdk.tar.gz  ← 新增
#   n48xlate\ (目录)
```

---

## 3. Windows: 写入 Dockerfile

用记事本或 PowerShell 把 `C:\n48-xlate\Dockerfile` 替换为以下内容:

```dockerfile
FROM rust:1-bookworm

# 安装 osxcross 依赖
RUN apt-get update && apt-get install -y \
    clang llvm cmake libxml2-dev uuid-dev libssl-dev bash patch \
    make libz-dev cpio bzip2 xz-utils && rm -rf /var/lib/apt/lists/*

# 克隆 osxcross (稳定 commit)
WORKDIR /opt
RUN git clone --depth 1 https://github.com/tpoechtrager/osxcross.git && \
    cd osxcross && \
    git checkout 9c8767e

# 从构建上下文复制 macOS SDK (上一步准备的)
COPY MacOSX-sdk.tar.gz /opt/osxcross/tarballs/

# 构建 osxcross 工具链 (生成 x86_64-apple-darwin-clang 等)
RUN cd /opt/osxcross && ./build.sh

# 添加 Rust target
RUN rustup target add x86_64-apple-darwin

# 配置 cargo 用 osxcross 链接
ENV PATH="/opt/osxcross/target/bin:$PATH"
ENV CC_x86_64_apple_darwin=x86_64-apple-darwin-clang
ENV CXX_x86_64_apple_darwin=x86_64-apple-darwin-clang++
ENV AR_x86_64_apple_darwin=x86_64-apple-darwin-ar

# 挂载构建材料
COPY 0001-metal2vulkan-changes-for-n48xlate.patch /patches/
COPY n48xlate /n48xlate/
WORKDIR /build

# 验证: 链接器就绪 + SDK 就位
RUN x86_64-apple-darwin-clang --version && \
    ls /opt/osxcross/target/SDK/*/usr/lib/libSystem.dylib
```

---

## 4. Windows: 一键构建

保存以下脚本为 `C:\n48-xlate\build.ps1`, 然后在 PowerShell 执行:

```powershell
# C:\n48-xlate\build.ps1

Write-Host "=== Step 1/3: Building Docker image (含 osxcross + macOS SDK) ===" -ForegroundColor Cyan
docker build -t n48-xlate .
if ($LASTEXITCODE -ne 0) { Write-Host "docker build 失败" -ForegroundColor Red; exit 1 }

Write-Host "`n=== Step 2/3: Running cross-compile container ===" -ForegroundColor Cyan
docker run --rm -v C:\n48-xlate:/output n48-xlate bash -c @'
set -e
echo "=== Cloning metal2vulkan ==="
git clone --quiet https://github.com/steelbrain/metal2vulkan.git
cd metal2vulkan

echo "=== Checking out Cobalt64's pinned commit ==="
git checkout --quiet 43c46ac8a24adf1a6e872b8a52c706ec9614fad0

echo "=== Applying Cobalt64 patch ==="
git apply /patches/0001-metal2vulkan-changes-for-n48xlate.patch

echo "=== Copying n48xlate crate ==="
cp -R /n48xlate .

echo "=== Building x86_64-apple-darwin dylib ==="
export CC_x86_64_apple_darwin=x86_64-apple-darwin-clang
export CXX_x86_64_apple_darwin=x86_64-apple-darwin-clang++
export AR_x86_64_apple_darwin=x86_64-apple-darwin-ar

# 加速 cargo (国内镜像)
mkdir -p ~/.cargo
cat > ~/.cargo/config.toml <<'EOF'
[source.crates-io]
replace-with = "rsproxy-sparse"
[source.rsproxy-sparse]
registry = "sparse+https://rsproxy.cn/index/"
EOF

cargo build --release -p n48xlate --target x86_64-apple-darwin 2>&1

echo "=== Copying artifact to /output ==="
cp target/x86_64-apple-darwin/release/libn48xlate.dylib /output/

echo "=== Verification ==="
file /output/libn48xlate.dylib
nm -g /output/libn48xlate.dylib | grep n48x
echo "=== Done ==="
'@

if ($LASTEXITCODE -ne 0) { Write-Host "docker run 失败" -ForegroundColor Red; exit 1 }

Write-Host "`n=== Step 3/3: Verify output on Windows ===" -ForegroundColor Cyan
if (Test-Path C:\n48-xlate\libn48xlate.dylib) {
    Write-Host "✓ 构建成功!" -ForegroundColor Green
    Write-Host "产物: C:\n48-xlate\libn48xlate.dylib"
    Write-Host "大小: $((Get-Item C:\n48-xlate\libn48xlate.dylib).Length) bytes"
} else {
    Write-Host "✗ 产物不存在" -ForegroundColor Red
    exit 1
}
```

执行:

```powershell
cd C:\n48-xlate
.\build.ps1
```

---

## 5. 验证产物

在容器最后一行应看到:

```
libn48xlate.dylib: Mach-O 64-bit dynamically linked shared library x86_64
```

Windows 上确认 (用 Docker 里的 file 命令):

```powershell
docker run --rm -v C:\n48-xlate:/output n48-xlate file /output/libn48xlate.dylib
# 预期输出: Mach-O 64-bit x86_64
```

导出符号检查 (应在容器内已完成,这里再次确认):

```powershell
docker run --rm -v C:\n48-xlate:/output n48-xlate bash -c "nm -g /output/libn48xlate.dylib | grep n48x"
# 预期输出包含: n48x_abi_version, n48x_translate, n48x_translate_linked, n48x_free
```

---

## 6. 部署到目标 PC

把 `C:\n48-xlate\libn48xlate.dylib` 拷到 PC (scp/U盘/共享), 替换 stub:

```bash
# 在 PC 上:
sudo cp /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib \
        ~/libn48xlate.dylib.stub-backup

sudo cp libn48xlate.dylib /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/

sudo chown root:wheel /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib
sudo chmod 755 /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib
sudo xattr -cr /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib

codesign --force --sign - /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib
codesign --force --sign - /Library/GPUBundles/Navi48Metal.bundle

codesign --verify --strict -v /Library/GPUBundles/Navi48Metal.bundle && echo "✓ OK"
```

重启 PC 生效。

---

## 7. 常见问题

| 问题 | 解法 |
|---|---|
| ARM Mac 无法 `xcrun --show-sdk-path` | 先装 Xcode Command Line Tools: `xcode-select --install` |
| `MacOSX-sdk.tar.gz` 太大传不动 | SDK 一般 50-100 MB (压缩后), 用 U盘 / scp / 网盘都行 |
| Docker build 时 `tarballs/MacOSX-sdk.tar.gz` 找不到 | 确认 tar 包已拷到 `C:\n48-xlate\MacOSX-sdk.tar.gz` |
| Docker 拉取镜像慢 | Docker Desktop → Settings → Docker Engine 加 registry-mirrors (见原指南 §7.1) |
| cargo 依赖下载慢 | build.ps1 已内置 rsproxy.cn 国内镜像 |
| 链接时报 `cannot find -lSystem` | 容器内检查: `ls /opt/osxcross/target/SDK/*/usr/lib/libSystem.dylib`。若不存在, 重新跑 `cd /opt/osxcross && ./build.sh` |
| Windows 挂载不生效 | Docker Desktop → Settings → Resources → File Sharing → 添加 `C:\` |

---

## 8. 完整文件清单

| 文件 | 位置 | 来源 |
|---|---|---|
| `MacOSX-sdk.tar.gz` | `C:\n48-xlate\` | ARM Mac 用 `tar -czf` 提取 |
| `Dockerfile` | `C:\n48-xlate\` | 本指南 §3 |
| `0001-metal2vulkan-changes-for-n48xlate.patch` | `C:\n48-xlate\` | Cobalt64 仓库 `third-party/metal2vulkan-n48/patches/` |
| `n48xlate\` | `C:\n48-xlate\` | Cobalt64 仓库 `third-party/metal2vulkan-n48/n48xlate/` |
| `build.ps1` | `C:\n48-xlate\` | 本指南 §4 |

---

## 9. 时间预算

| 步骤 | 耗时 |
|---|---|
| ARM Mac 提取 SDK | 1 分钟 |
| 拷贝到 Windows | 1-5 分钟 (视网络) |
| `docker build` (首次, 含 osxcross + SDK 解压) | 10-20 分钟 |
| `docker run` (含 cargo build) | 5-15 分钟 |
| **合计** | **约 20-40 分钟** |

首次较慢, 之后镜像已缓存, 重复构建只需 2-3 分钟。

---

祝构建顺利! 卡住时把**具体命令 + 完整错误输出**贴回来。
