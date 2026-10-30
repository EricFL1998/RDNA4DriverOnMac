#!/bin/bash
# Phase 2 Complete 验证脚本

echo "=== Phase 2 验证: NBIO + SMU Setup + GC Power ==="
echo ""

echo "1. 检查关键状态属性:"
ioreg -l -w0 -r -n RDNA4Driver | grep -E "NBIO,Stage|SMU,Setup|SMU,GC|GFX,GRBM|GFX,Bootload|GFX,Stage|PSP,Stage|SMU,Stage"

echo ""
echo "2. 检查初始化日志:"
sudo log show --last boot --style compact --predicate 'eventMessage CONTAINS "NBIO:" OR eventMessage CONTAINS "SMU: GC"' | grep RDNA4 | tail -20

echo ""
echo "3. 检查 GC 寄存器值:"
ioreg -l -w0 -r -n RDNA4Driver | grep -E "GFX,GRBMStatus|GFX,BootloadStatus"

echo ""
echo "=== 验证完成 ==="
