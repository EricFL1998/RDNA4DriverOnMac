#!/bin/bash
# Phase 2 最终验证 - NBIO + SMU 完整测试

echo "=== Phase 2 最终验证：NBIO Doorbell + SMU Setup ==="
echo ""

ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  -o ConnectTimeout=10 louxuanchen@192.168.31.57 << 'VERIFY'

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "1. 驱动加载状态"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
kextstat | grep rdna4

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "2. 驱动初始化阶段（关键属性）"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
ioreg -l -w0 -r -n RDNA4Driver | grep -E "NBIO,Stage|SMU,SetupStage|SMU,GCPowerState|GFX,Stage|PSP,Stage|SMU,Stage"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3. GC 寄存器状态（最关键！）"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
ioreg -l -w0 -r -n RDNA4Driver | grep -E "GFX,GRBMStatus|GFX,BootloadStatus"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "4. NBIO 初始化日志"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
sudo log show --last boot --style compact --predicate 'eventMessage CONTAINS "NBIO:"' | grep RDNA4

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "5. SMU Setup 日志"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
sudo log show --last boot --style compact --predicate 'eventMessage CONTAINS "SMU: "' | grep RDNA4 | tail -25

VERIFY

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "Phase 2 成功标志："
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ NBIO,Stage = complete              → NBIO 初始化成功"
echo "✅ SMU,SetupStage = complete          → SMU 完整流程成功"
echo "✅ SMU,GCPowerState = active/partial  → GC 通电"
echo "✅ GFX,GRBMStatus != 0                → GC 寄存器响应（核心目标！）"
echo "✅ GFX,BootloadStatus & 0x80000000    → RLC autoload 完成"
echo ""
