# Metal 设备注册问题排查清单

**问题**: `MTLCreateSystemDefaultDevice()` 返回 nil

---

## ✅ 快速检查（5 分钟）

### 1. Bundle 是否安装

```bash
ls -la /Library/GPUBundles/RDNA4Metal.bundle
```

**期望**: 目录存在，权限为 `drwxr-xr-x root:wheel`

**如果不存在**:
```bash
cd ~/hackintosh/navi48-native/metal-bundle
sudo make install
```

---

### 2. Info.plist 是否正确

```bash
cat /Library/GPUBundles/RDNA4Metal.bundle/Contents/Info.plist | grep -A1 MetalDeviceCreation
```

**期望**:
```xml
<key>MetalDeviceCreationFunction</key>
<string>RDNA4CreateDevice</string>
```

**如果不正确**: 重新编译并安装

---

### 3. 符号是否导出

```bash
nm /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal | grep RDNA4Create
```

**期望**:
```
000000000001xxxx T _RDNA4CreateDevice
000000000001xxxx T _RDNA4GetAllDevices
```

**注意**: 必须是大写 `T`（全局符号），不能是小写 `t`（本地符号）或 `U`（未定义）

**如果不正确**: 检查 Makefile 的 `-exported_symbols_list` 选项

---

### 4. 主库文件存在

```bash
ls -lh /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal
```

**期望**: 文件存在，大小 > 100 KB

---

### 5. AIRNT Plugin 存在

```bash
ls -lh /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/libAMDNTPlugin.dylib
```

**期望**: 文件存在

---

## 🔍 深度检查（10 分钟）

### 6. 运行设备测试

```bash
cd ~/hackintosh/navi48-native/metal-bundle

# 编译测试
clang -framework Foundation -framework Metal \
      -o build/test/DeviceTest \
      test/DeviceRegistrationTest.m

# 运行
./build/test/DeviceTest
```

**期望输出**:
```
========================================
RDNA4Metal Bundle Loaded
  Version: 1.0.0 Phase 3.4
========================================
RDNA4CreateDevice: Metal.framework is requesting device creation
RDNA4CreateDevice: ✅ Created device: AMD Radeon RX 9070 XT (RDNA4)
✅ PASSED: Found Metal device
```

---

### 7. 检查系统日志

打开一个终端，运行日志监控：

```bash
log stream --predicate 'subsystem == "com.apple.Metal"' --level debug
```

在另一个终端运行测试，观察日志输出。

**期望看到**:
- Metal.framework 扫描 GPUBundles
- 加载 RDNA4Metal.bundle
- 调用 RDNA4CreateDevice

---

### 8. 检查 RDNA4 驱动加载

```bash
kextstat | grep -i rdna4
```

**期望**: 看到 `com.rdna4.driver` 和 `com.rdna4.accel`

**如果没有**: 驱动未加载，Metal Bundle 无法工作

---

### 9. 检查 IORegistry

```bash
ioreg -l | grep -i rdna4
```

**期望**: 看到 RDNA4Driver 和 RDNA4Accel 相关条目

---

### 10. 清除缓存并重试

```bash
sudo rm -rf /System/Library/Caches/com.apple.metal
sudo touch /Library/GPUBundles/
```

然后重新运行测试。

---

## 🐛 常见问题

### 问题 1: 符号未导出

**症状**: `nm` 没有显示 `_RDNA4CreateDevice`

**原因**: 链接器配置错误

**解决**: 

创建 `exports.txt`:
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

重新编译：
```bash
make clean && make && sudo make install
```

---

### 问题 2: Bundle 加载但未调用工厂函数

**症状**: 日志显示 bundle 加载，但没有 "RDNA4CreateDevice: ..." 日志

**原因**: Info.plist 配置错误

**检查**:
```bash
plutil -p /Library/GPUBundles/RDNA4Metal.bundle/Contents/Info.plist | grep -i metal
```

**必须包含**:
- `MetalPluginName`
- `MetalPluginClassName`
- `MetalDeviceCreationFunction`
- `MetalPluginType`

---

### 问题 3: dyld 加载错误

**症状**: 
```
dyld: Library not loaded: @rpath/libAMDNTPlugin.dylib
```

**原因**: AIRNT Plugin 未复制到 bundle

**解决**:
```bash
sudo cp ~/hackintosh/navi48-native/airnt-plugin/build/libAMDNTPlugin.dylib \
    /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/
```

---

### 问题 4: Metal.framework 完全不扫描 bundle

**症状**: 日志中完全没有 RDNA4Metal 相关信息

**可能原因**:
1. Bundle 权限错误
2. Info.plist 格式错误
3. Metal.framework 缓存了旧信息

**解决**:
```bash
# 修复权限
sudo chown -R root:wheel /Library/GPUBundles/RDNA4Metal.bundle
sudo chmod -R 755 /Library/GPUBundles/RDNA4Metal.bundle

# 验证 Info.plist
plutil /Library/GPUBundles/RDNA4Metal.bundle/Contents/Info.plist

# 清除缓存
sudo rm -rf /System/Library/Caches/com.apple.metal
sudo touch /Library/GPUBundles/

# 重启（最后手段）
sudo reboot
```

---

### 问题 5: IOKit 匹配失败

**症状**: Metal 找不到设备，但 bundle 已加载

**原因**: IOPCIMatch 不匹配实际硬件

**检查**:
```bash
# 查看实际 PCI ID
ioreg -l -w 0 | grep -i "vendor-id\|device-id"
```

**修改 Info.plist**:
```xml
<key>IOPCIMatch</key>
<string>0x75501002</string>  <!-- 改成实际的 device-id + vendor-id -->
```

---

## 📋 完整排查流程

```bash
#!/bin/bash
# metal_device_debug.sh

echo "1. 检查 bundle 安装..."
ls -la /Library/GPUBundles/RDNA4Metal.bundle || echo "❌ Bundle 不存在"

echo ""
echo "2. 检查 Info.plist..."
grep -q "RDNA4CreateDevice" /Library/GPUBundles/RDNA4Metal.bundle/Contents/Info.plist && \
    echo "✅ Info.plist 包含 RDNA4CreateDevice" || \
    echo "❌ Info.plist 缺少 RDNA4CreateDevice"

echo ""
echo "3. 检查符号导出..."
nm /Library/GPUBundles/RDNA4Metal.bundle/Contents/MacOS/RDNA4Metal | grep -q "T _RDNA4CreateDevice" && \
    echo "✅ 符号已导出" || \
    echo "❌ 符号未导出"

echo ""
echo "4. 检查驱动加载..."
kextstat | grep -q rdna4 && \
    echo "✅ 驱动已加载" || \
    echo "❌ 驱动未加载"

echo ""
echo "5. 检查权限..."
PERMS=$(stat -f "%Sp %Su:%Sg" /Library/GPUBundles/RDNA4Metal.bundle)
echo "   权限: $PERMS"

echo ""
echo "6. 运行快速测试..."
cat > /tmp/test_device.m << 'EOF'
#import <Metal/Metal.h>
int main() {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSLog(dev ? @"✅ 找到设备: %@" : @"❌ 未找到设备", dev.name);
        return dev ? 0 : 1;
    }
}
EOF

clang -framework Foundation -framework Metal -o /tmp/test_device /tmp/test_device.m
/tmp/test_device
rm /tmp/test_device /tmp/test_device.m
```

保存并运行：
```bash
chmod +x metal_device_debug.sh
./metal_device_debug.sh
```

---

## ✅ 成功标志

当一切正常时，应该看到：

```
✅ Bundle 安装在 /Library/GPUBundles/
✅ Info.plist 包含 MetalDeviceCreationFunction
✅ _RDNA4CreateDevice 符号已导出（大写 T）
✅ RDNA4Driver.kext 已加载
✅ IORegistry 中有 RDNA4 设备
✅ MTLCreateSystemDefaultDevice() 返回设备
✅ 设备名称为 "AMD Radeon RX 9070 XT (RDNA4)"
```

---

**如果所有检查都通过，Metal 设备注册就成功了！** 🎉

---

*检查清单创建于 2026-10-30*
