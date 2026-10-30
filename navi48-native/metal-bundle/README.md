# RDNA4Metal.bundle - Native Metal Bundle for RDNA4 GPUs

macOS Metal Bundle 实现，为 AMD RDNA4 (Navi 48) GPU 提供完整的 MTLDevice 接口。

---

## 架构

```
Metal.framework (Apple)
        ↓
/Library/GPUBundles/RDNA4Metal.bundle
        ├── Info.plist (Bundle 元信息)
        ├── Contents/MacOS/
        │   ├── RDNA4Metal (主二进制，Objective-C)
        │   └── libAMDNTPlugin.dylib (编译器插件)
        └── Contents/Resources/
            └── default.metallib (默认着色器库，可选)
```

---

## 实现进度

### Phase 3.2 (当前阶段) - 骨架实现

| 类 | 状态 | 说明 |
|----|------|------|
| `RDNA4Device` | ✅ 骨架完成 | 实现 MTLDevice 协议，属性返回正确值 |
| `RDNA4CommandQueue` | ✅ 骨架完成 | 创建命令缓冲 |
| `RDNA4CommandBuffer` | ✅ 骨架完成 | 命令编码与提交（stub） |
| `RDNA4Buffer` | ✅ 骨架完成 | Host-side 内存分配（临时） |
| `RDNA4Texture` | 🔜 Phase 3.4 | 纹理资源 |
| `RDNA4Library` | 🔜 Phase 3.3 | Metallib 加载与函数查询 |
| `RDNA4Function` | 🔜 Phase 3.3 | Shader 函数元信息 |
| `RDNA4ComputePipelineState` | 🔜 Phase 3.3 | 编译后的 Compute Pipeline |
| `RDNA4RenderPipelineState` | 🔜 Phase 3.4 | 编译后的 Render Pipeline |
| `RDNA4ComputeCommandEncoder` | 🔜 Phase 3.3 | Compute 命令编码 |

### Phase 3.3 (下一步) - Compute Kernel 执行

需要实现：
1. `RDNA4Library` 加载 `.metallib` 并提取 AIR bitcode
2. `RDNA4ComputePipelineState` 调用 AIRNT plugin 编译为 GFX12 机器码
3. `RDNA4ComputeCommandEncoder` 生成 PM4 命令流
4. `RDNA4CommandBuffer.commit` 通过内核驱动提交到 GPU Ring Buffer

### Phase 3.4 (更长期) - 3D 渲染

需要实现：
- Vertex Fetch 配置
- Fragment Shader 编译
- Render Target 配置
- EXP 指令生成（Color Export）

---

## Bundle 结构

### Info.plist

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>
    <string>com.rdna4.metal</string>
    <key>CFBundleName</key>
    <string>RDNA4Metal</string>
    <key>CFBundleVersion</key>
    <string>1.0.0</string>
    <key>MetalPluginClassName</key>
    <string>RDNA4Device</string>
    <key>MetalPluginName</key>
    <string>RDNA4</string>
    <key>MetalDeviceRegistryID</key>
    <integer>0x7550</integer>
</dict>
</plist>
```

### 目录布局

```
RDNA4Metal.bundle/
├── Contents/
│   ├── Info.plist
│   ├── MacOS/
│   │   ├── RDNA4Metal            # 主二进制（此目录编译产物）
│   │   └── libAMDNTPlugin.dylib  # 从 airnt-plugin/ 拷贝
│   └── Resources/
│       └── (可选资源文件)
```

---

## 编译

```bash
cd navi48-native/metal-bundle
make
```

生成 `RDNA4Metal.bundle`。

---

## 部署

```bash
# 方式 1: 开发测试（不需要 SIP 禁用）
make install_dev
# 安装到 ~/Library/GPUBundles/

# 方式 2: 系统级部署（需要 SIP 禁用 + 签名）
sudo make install_system
# 安装到 /Library/GPUBundles/
```

---

## 使用示例

```objc
#import <Metal/Metal.h>

// Metal.framework 会自动发现并加载 RDNA4Metal.bundle
NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
for (id<MTLDevice> device in devices) {
    NSLog(@"Device: %@", device.name);
    if ([device.name containsString:@"RDNA4"]) {
        // 找到我们的设备
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> buffer = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
        // ...
    }
}
```

---

## 限制（Phase 3.2）

当前骨架实现的限制：

- ❌ Buffer 数据存储在 host 内存，不在 GPU VRAM
- ❌ CommandBuffer.commit() 不实际提交到 GPU
- ❌ Compute Pipeline 编译返回空 stub
- ❌ 纹理、采样器、深度缓冲未实现

这些将在 Phase 3.3 与内核驱动联调时实现。

---

## 测试

```bash
# 编译测试程序
make test

# 运行测试
./bin/test_rdna4_device
```

---

*Part of RDNA4 macOS Native Metal Driver Project - Phase 3*
