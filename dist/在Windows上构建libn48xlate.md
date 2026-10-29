# 在 Windows 上构建真正的 libn48xlate.dylib

> 目标: 在 Windows 机器上用 Docker 交叉编译 macOS x86_64 的 `libn48xlate.dylib`
> 用于替换 Navi48Metal.bundle 里的 stub,启用进程内实时 shader 翻译。
>
> 只需 Docker Desktop,无需 WSL2 内部手动配置。

---

## 1. 准备工作

### 1.1 安装 Docker Desktop

1. 下载 [Docker Desktop for Windows](https://www.docker.com/products/docker-desktop/)
2. 安装时勾选 **"Use WSL 2 instead of Hyper-V"** (推荐)
3. 启动 Docker Desktop,确保托盘图标显示 **"Docker Desktop is running"**

### 1.2 准备构建文件

把以下 3 个文件放到同一个目录 (例如 `C:\n48-xlate\`):

```
C:\n48-xlate\
├── 0001-metal2vulkan-changes-for-n48xlate.patch   (已在 dist/xlate-patches/)
├── n48xlate\                                       (目录, 已在 dist/xlate-patches/)
└── Dockerfile                                      (已在 dist/xlate-patches/)
```

> 这 3 个文件都已在 `dist/xlate-patches/` 目录下,直接整个拷贝过去即可。

**也可以直接拷贝 dist 目录到 Windows:**
```
xcopy /E /I \\Mac\hackintosh\dist C:\n48-build
```

---

## 2. 构建步骤

打开 PowerShell 或 CMD,执行:

```powershell
# 进入构建目录
cd C:\n48-xlate

# 构建 Docker 镜像 (只需一次, 以后复用)
docker build -t n48-xlate .

# 运行容器 (交互式)
docker run --rm -it -v C:\n48-xlate:/output n48-xlate bash
```

进入容器后,依次执行:

```bash
# 1. 克隆 metal2vulkan 源码
git clone https://github.com/steelbrain/metal2vulkan.git
cd metal2vulkan

# 2. 切换到 Cobalt64 指定的 commit
git checkout 43c46ac8a24adf1a6e872b8a52c706ec9614fad0

# 3. 应用 Cobalt64 的 patch
git apply /patches/0001-metal2vulkan-changes-for-n48xlate.patch

# 4. 复制 n48xlate crate 到工作区
cp -R /n48xlate .

# 5. 构建 (x86_64 macOS target)
cargo build --release -p n48xlate --target x86_64-apple-darwin

# 6. 确认产物
ls -lh target/x86_64-apple-darwin/release/libn48xlate.dylib
# 预期: Mach-O 64-bit dynamically linked shared library x86_64
file target/x86_64-apple-darwin/release/libn48xlate.dylib
```

**退出容器前,把产物拷到共享目录:**
```bash
cp target/x86_64-apple-darwin/release/libn48xlate.dylib /output/
exit
```

回到 Windows,文件在 `C:\n48-xlate\libn48xlate.dylib`。

---

## 3. 验证产物

在容器里或 Mac 上验证:

```bash
# 必须是 Mach-O x86_64,不是 Linux ELF
file libn48xlate.dylib
# 预期输出: Mach-O 64-bit dynamically linked shared library x86_64

# 导出符号检查 (应包含 n48x_abi_version, n48x_translate, n48x_free)
nm -g libn48xlate.dylib | grep n48x
```

---

## 4. 部署到 PC

把 `libn48xlate.dylib` 拷贝到目标 PC,替换 bundle 内的 stub:

```bash
# 在 PC 上:
# 备份 stub
sudo cp /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib \
        ~/libn48xlate.dylib.stub-backup

# 替换为真正的翻译器
sudo cp libn48xlate.dylib /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/

# 修复权限和签名
sudo chown root:wheel /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib
sudo chmod 755 /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib
sudo xattr -cr /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib
codesign --force --sign - /Library/GPUBundles/Navi48Metal.bundle/Contents/Resources/libn48xlate.dylib
codesign --force --sign - /Library/GPUBundles/Navi48Metal.bundle

# 验证
codesign --verify --strict -v /Library/GPUBundles/Navi48Metal.bundle && echo "OK"
```

重启 PC 后生效。

---

## 5. 常见问题

### 5.1 Docker 拉取镜像慢

```powershell
# 配置国内镜像加速器 (Docker Desktop → Settings → Docker Engine):
{
  "registry-mirrors": [
    "https://docker.m.daocloud.io",
    "https://docker.1panel.live"
  ]
}
```

### 5.2 cargo 下载依赖慢

在容器内执行:
```bash
# 换国内 crates.io 镜像
mkdir -p ~/.cargo
cat > ~/.cargo/config.toml <<'CARGOEOF'
[source.crates-io]
replace-with = 'rsproxy-sparse'
[source.rsproxy-sparse]
registry = "sparse+https://rsproxy.cn/index/"
CARGOEOF

# 然后重新 build
cargo build --release -p n48xlate --target x86_64-apple-darwin
```

### 5.3 容器内 git clone 失败

```bash
# 手动下载 zip 替代 git clone
curl -L -o metal2vulkan.zip https://github.com/steelbrain/metal2vulkan/archive/43c46ac8.zip
unzip metal2vulkan.zip && mv metal2vulkan-43c46ac8* metal2vulkan
cd metal2vulkan
# 后续步骤相同
```

### 5.4 产物是 Linux ELF 而不是 Mach-O

说明 target 不对。检查:
```bash
rustup target list --installed
# 必须包含: x86_64-apple-darwin

# 如果没有:
rustup target add x86_64-apple-darwin
```

### 5.5 Windows 路径挂载问题

如果 `-v C:\n48-xlate:/output` 不生效,尝试:
```powershell
# 用正斜杠
docker run --rm -it -v C:/n48-xlate:/output n48-xlate bash

# 或在 Docker Desktop → Settings → Resources → File Sharing
# 确保 C:\ 盘已共享
```

---

## 6. 完整 PowerShell 一键脚本

```powershell
# C:\n48-xlate\build.ps1
# 前提: Dockerfile, patch, n48xlate/ 在当前目录

Write-Host "=== Building n48-xlate Docker image ==="
docker build -t n48-xlate .

Write-Host "=== Running build container ==="
docker run --rm -v ${PWD}:/output n48-xlate bash -c "
set -e
git clone --quiet https://github.com/steelbrain/metal2vulkan.git
cd metal2vulkan
git checkout --quiet 43c46ac8a24adf1a6e872b8a52c706ec9614fad0
git apply /patches/0001-metal2vulkan-changes-for-n48xlate.patch
cp -R /n48xlate .
cargo build --release -p n48xlate --target x86_64-apple-darwin 2>&1
cp target/x86_64-apple-darwin/release/libn48xlate.dylib /output/
echo '=== Build complete ==='
file /output/libn48xlate.dylib
"

Write-Host "=== Output ==="
file libn48xlate.dylib
```

---

## 7. 文件来源说明

| 文件 | 来源 |
|---|---|
| `0001-metal2vulkan-changes-for-n48xlate.patch` | Cobalt64 仓库 `third-party/metal2vulkan-n48/patches/` |
| `n48xlate/` | Cobalt64 仓库 `third-party/metal2vulkan-n48/n48xlate/` |
| `Dockerfile` | 本指南配套 (基于 `rust:1-bookworm`) |

这三个文件组合在一起,使得 Windows 上的 Docker 可以独立完成
libn48xlate.dylib 的交叉编译,无需访问 Mac 或 Linux 物理机。
