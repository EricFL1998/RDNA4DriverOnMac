# Metal 设备注册修复指南

**问题**: `MTLCreateSystemDefaultDevice()` 返回 nil

**原因**: Metal Bundle 缺少设备工厂函数

---

## 修复内容

### 1. 新增文件

```
navi48-native/metal-bundle/src/RDNA4MetalPlugin.m
```

**功能**: 
- 导出 `RDNA4CreateDevice()` - Metal.framework 调用的入口
- 导出 `RDNA4GetAllDevices()` - 备用接口
- Bundle 加载/卸载日志

### 2. 更新的文件

#### Info.plist
```xml
<key>MetalDeviceCreationFunction</key>
<string>RDNA4CreateDevice</string>

<key>SupportsRenderShaders</key>
<true/>
```

#### Makefile
```makefile
OBJC_SOURCES = \
    ... \
    $(SRC_DIR)/RDNA4MetalPlugin.m \
    ...

CFLAGS = ... \
    -exported_symbols_list <(echo "_RDNA4CreateDevice"; echo "_RDNA4GetAllDevices")
```

---

## 重新编译和安装

```bash
cd ~/hackintosh/navi48-native/metal-bundle

# 清理
make clean

# 编译
make

# 安装
sudo make install

# 验证安装
ls -la /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/
# 应该看到 RDNA4Metal 和 libAMDNTPlugin.dylib

# 检查符号导出
nm /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal | grep RDNA4Create
# 应该看到：
# 000000000001xxxx T _RDNA4CreateDevice
# 000000000001xxxx T _RDNA4GetAllDevices
```

---

## 测试

### 最小测试

```objc
#import <Metal/Metal.h>

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device) {
            NSLog(@"✅ Found device: %@", device.name);
            return 0;
        } else {
            NSLog(@"❌ No Metal device found");
            return 1;
        }
    }
}
```

编译并运行：

```bash
clang -framework Foundation -framework Metal -o test_device test_device.m
./test_device
```

**期望输出**:
```
========================================
RDNA4Metal Bundle Loaded
  Version: 1.0.0 Phase 3.4
  Build Date: 2026-10-30
========================================
RDNA4CreateDevice: Metal.framework is requesting device creation
RDNA4CreateDevice: ✅ Created device: AMD Radeon RX 9070 XT (RDNA4)
✅ Found device: AMD Radeon RX 9070 XT (RDNA4)
```

---

## 如果还是找不到设备

### 检查清单

1. **Bundle 位置正确**
   ```bash
   ls -la /Library/GPUBundles/RDNA4Metal.bundle
   ```

2. **Info.plist 正确**
   ```bash
   cat /Library/GPUBundles/RDNA4Metal.bundle/Contents/Info.plist | grep MetalDeviceCreation
   ```
   应该看到 `<string>RDNA4CreateDevice</string>`

3. **符号导出正确**
   ```bash
   nm /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal | grep RDNA4Create
   ```
   应该看到 `T _RDNA4CreateDevice`（不是 `t` 或 `U`）

4. **Bundle 权限正确**
   ```bash
   ls -la /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/
   ```
   应该是 `root:wheel`

5. **清除系统缓存**
   ```bash
   sudo rm -rf /System/Library/Caches/com.apple.metal
   sudo touch /Library/GPUBundles/
   ```

6. **检查系统日志**
   ```bash
   log stream --predicate 'subsystem == "com.apple.Metal"' --level debug
   ```
   然后运行测试，看 Metal.framework 的日志

7. **检查 IOKit 匹配**
   ```bash
   ioreg -l | grep -i rdna4
   ```
   确认驱动加载了

---

## 高级调试

### 如果符号导出有问题

创建显式导出文件：

```bash
cat > exports.txt << 'EOF'
_RDNA4CreateDevice
_RDNA4GetAllDevices
EOF
```

修改 Makefile:
```makefile
CFLAGS = ... \
    -exported_symbols_list exports.txt
```

### 如果 Metal.framework 不调用工厂函数

可能需要注册为 IOService matching：

在 Info.plist 中添加：
```xml
<key>IOMatchCategory</key>
<string>IOAccelerator</string>

<key>IOClass</key>
<string>IOAccelerator</string>

<key>IOProviderClass</key>
<string>IOPCIDevice</string>

<key>IOPCIMatch</key>
<string>0x75501002</string>
```

---

## 预期结果

修复后，Metal 设备注册流程：

```
1. Metal.framework 启动
   ↓
2. 扫描 /Library/GPUBundles/*.bundle
   ↓
3. 读取 RDNA4Metal.bundle/Contents/Info.plist
   ↓
4. 找到 MetalDeviceCreationFunction = "RDNA4CreateDevice"
   ↓
5. dlopen RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal
   ↓
6. 调用 RDNA4CreateDevice()
   ↓
7. 返回 RDNA4Device 实例
   ↓
8. MTLCreateSystemDefaultDevice() 返回设备 ✅
```

现在应该能通过了！

---

*修复指南创建于 2026-10-30*
