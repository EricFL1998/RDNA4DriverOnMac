#!/bin/bash
set -e

PC_IP="${1:-192.168.31.57}"
SSH_KEY="ssh-keys/n48-debug"
KNOWN_HOSTS="/tmp/n48_known_hosts_v6"

echo "=== 正在检查 PC ($PC_IP) 上 RDNA4Driver 全功能运行状态 ==="

ssh -4 -i "$SSH_KEY" -o UserKnownHostsFile="$KNOWN_HOSTS" -o ConnectTimeout=10 "louxuanchen@$PC_IP" << 'EOF'
echo "--- 1. 当前启动参数 ---"
sysctl kern.bootargs

echo ""
echo "--- 2. IORegistry 显卡驱动属性 ---"
ioreg -l -w0 -r -n RDNA4Driver | grep -E "Stage|State|VRAM|Version|SignOfLife|Bootload|GRBM|GMC|NBIO|PSP|SMU|GFX" || echo "未找到 RDNA4Driver 实例"

echo ""
echo "--- 3. 内核最近日志 (dmesg) ---"
sudo dmesg | grep "RDNA4:" | tail -n 35 || true
EOF
