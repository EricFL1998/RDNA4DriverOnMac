#!/bin/bash
# Phase 3 验证脚本 - PSP + SMU + GC 通电

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🎯 Phase 3 完整验证：PSP + SMU Setup + GC 通电"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

ssh -4 -i ssh-keys/n48-debug -o UserKnownHostsFile=/tmp/n48_known_hosts_v6 \
  -o ConnectTimeout=10 louxuanchen@192.168.31.57 << 'VERIFY'

echo "1️⃣ 驱动加载状态"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
kextstat | grep rdna4

echo ""
echo "2️⃣ 所有驱动属性"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
ioreg -l -w0 -r -n RDNA4Driver | grep "\"[A-Z]" | grep "=" | head -50

echo ""
echo "3️⃣ PSP 初始化日志"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
sudo log show --last boot --predicate 'eventMessage CONTAINS "PSP:"' --style compact | grep RDNA4

echo ""
echo "4️⃣ SMU 初始化日志"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
sudo log show --last boot --predicate 'eventMessage CONTAINS "SMU:"' --style compact | grep RDNA4

echo ""
echo "5️⃣ GFX 初始化日志"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
sudo log show --last boot --predicate 'eventMessage CONTAINS "GFX:"' --style compact | grep RDNA4

echo ""
echo "6️⃣ 关键寄存器状态"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
ioreg -l -w0 -r -n RDNA4Driver | grep -E "PSP,|SMU,|GFX,|NBIO," | head -30

VERIFY

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🎯 Phase 3 成功标志："
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ PSP,Stage = alive              → PSP 固件加载成功"
echo "✅ SMU,Stage = alive              → SMU 初始化成功"
echo "✅ SMU,SetupStage = complete      → SMU Setup 完成"
echo "✅ SMU,GCPowerState = active      → GC 通电成功"
echo "✅ GFX,GRBMStatus != 0            → GC 寄存器响应（🎉核心目标！）"
echo "✅ GFX,BootloadStatus & 0x80000000 → RLC autoload 完成"
echo ""
