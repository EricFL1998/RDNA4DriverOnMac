#!/bin/bash
# Phase 2 验证脚本 - 检查 NBIO 和 SMU 初始化结果

echo "=== Phase 2 NBIO + SMU 验证 ==="
echo ""

ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  -o ConnectTimeout=10 louxuanchen@192.168.31.57 << 'VERIFY'

echo "1. 驱动关键状态："
ioreg -l -w0 -r -n RDNA4Driver | grep -E "NBIO,Stage|SMU,Setup|SMU,GC|GFX,GRBM|GFX,Bootload|PSP,Stage|SMU,Stage" | head -20

echo ""
echo "2. NBIO 详细日志："
sudo log show --last boot --style compact --predicate 'eventMessage CONTAINS "NBIO:"' | grep RDNA4 | tail -30

echo ""
echo "3. SMU Setup 日志："
sudo log show --last boot --style compact --predicate 'eventMessage CONTAINS "SMU:"' | grep RDNA4 | tail -20

echo ""
echo "4. GC 状态检查："
ioreg -l -w0 -r -n RDNA4Driver | grep -E "GFX,GRBMStatus|GFX,BootloadStatus"

VERIFY

echo ""
echo "=== 关键指标 ==="
echo "NBIO,Stage = complete          → NBIO 成功"
echo "NBIO,Stage = failed-safe       → NBIO 失败但安全"
echo "SMU,SetupStage = complete      → SMU 完整成功"
echo "SMU,GCPowerState = active      → GC 通电成功！"
echo "GFX,GRBMStatus != 0            → GC 响应成功！"
echo ""
