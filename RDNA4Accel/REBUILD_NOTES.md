# RDNA4Accel 重编译说明

## 修改内容

1. **继承链修复**: `IOService` → `IOAccelerator`
2. **添加头文件**: `#include <IOKit/graphics/IOAccelerator.h>`
3. **元类定义更新**: `OSDefineMetaClassAndStructors(RDNA4Accel, IOAccelerator)`

## 重新编译步骤（在黑苹果上）

```bash
cd RDNA4Accel

# 清理旧文件
make clean

# 重新编译
make

# 卸载旧版本
sudo kextunload /Library/Extensions/RDNA4Accel.kext

# 安装新版本
sudo cp -R build/RDNA4Accel.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Accel.kext
sudo kextcache -i /

# 加载验证
sudo kextload /Library/Extensions/RDNA4Accel.kext
kextstat | grep rdna4
```

## 验证

```bash
ioreg -l | grep -A10 RDNA4Accel | grep IOAccelerator
```

期望看到 `IOAccelerator` 出现在继承链中。
