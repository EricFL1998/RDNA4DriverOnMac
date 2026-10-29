#!/bin/bash
# Phase 2 修复版本部署脚本

set -e

echo "=== Phase 2 修复版本部署 ==="
echo ""
echo "此版本特性："
echo "  - NBIO 初始化已启用"
echo "  - 添加了完整的错误检查和日志"
echo "  - 失败时安全降级，不阻塞系统启动"
echo "  - 寄存器写入前验证可访问性"
echo ""

# 确保在正确的目录
cd "$(dirname "$0")"

# 检查 kext 是否存在
if [ ! -d "RDNA4Driver/build/RDNA4Driver.kext" ]; then
    echo "错误：找不到编译好的 kext"
    echo "请先运行: make -C RDNA4Driver all"
    exit 1
fi

echo "步骤 1: 上传 kext 到实机..."
scp -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
    -o ConnectTimeout=10 \
    -r RDNA4Driver/build/RDNA4Driver.kext louxuanchen@192.168.31.57:/tmp/

if [ $? -ne 0 ]; then
    echo "错误：无法连接到实机"
    echo "请确保："
    echo "  1. 机器已启动并可 SSH 连接"
    echo "  2. 已删除旧的问题驱动"
    exit 1
fi

echo ""
echo "步骤 2: 安装并重启..."
ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
    -o ConnectTimeout=10 louxuanchen@192.168.31.57 << 'EOF'
set -e
echo "  - 备份旧驱动（如果存在）"
if [ -d /Library/Extensions/RDNA4Driver.kext ]; then
    sudo cp -R /Library/Extensions/RDNA4Driver.kext /tmp/RDNA4Driver.kext.backup.$(date +%s)
    sudo rm -rf /Library/Extensions/RDNA4Driver.kext
fi

echo "  - 安装新驱动"
sudo cp -R /tmp/RDNA4Driver.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/RDNA4Driver.kext
sudo chmod -R 755 /Library/Extensions/RDNA4Driver.kext

echo "  - 刷新 kext 缓存"
sudo kextcache -invalidate /

echo "  - 记录重启前的时间"
sysctl -n kern.boottime

echo "  - 准备重启"
echo "系统将在 3 秒后重启..."
sleep 3
sudo shutdown -r now
EOF

echo ""
echo "步骤 3: 等待系统重启..."
echo "（这个脚本会在后台监控）"

# 等待 SSH 断开
sleep 5

# 记录旧的 boot time
OLD_BOOT=$(ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
    -o ConnectTimeout=3 louxuanchen@192.168.31.57 'sysctl -n kern.boottime' 2>/dev/null | \
    sed -n 's/.*sec = \([0-9]*\).*/\1/p' || echo "0")

echo "旧的启动时间: $OLD_BOOT"
echo ""

# 等待重启完成（最多 3 分钟）
for i in {1..36}; do
    echo "等待中... ($i/36, 每次等待 5 秒)"
    sleep 5
    
    NEW_BOOT=$(ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
        -o ConnectTimeout=3 louxuanchen@192.168.31.57 'sysctl -n kern.boottime' 2>/dev/null | \
        sed -n 's/.*sec = \([0-9]*\).*/\1/p' || echo "0")
    
    if [ "$NEW_BOOT" != "0" ] && [ "$NEW_BOOT" != "$OLD_BOOT" ]; then
        echo ""
        echo "=== 系统已重启！==="
        echo "新的启动时间: $NEW_BOOT"
        echo ""
        
        # 等待系统完全稳定
        sleep 10
        
        echo "步骤 4: 验证驱动状态..."
        ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
            louxuanchen@192.168.31.57 << 'VERIFY'
echo "=== 驱动状态 ==="
ioreg -l -w0 -r -n RDNA4Driver | grep -E "NBIO,Stage|SMU,Setup|SMU,GC|GFX,GRBM|GFX,Bootload|PSP,Stage|SMU,Stage" | head -15

echo ""
echo "=== NBIO 初始化日志 ==="
sudo log show --last boot --style compact --predicate 'eventMessage CONTAINS "NBIO:"' | grep RDNA4 | tail -20
VERIFY
        
        echo ""
        echo "=== 部署完成 ==="
        exit 0
    fi
done

echo ""
echo "警告：系统在 3 分钟后仍未启动"
echo "请检查物理机器状态"
exit 1
