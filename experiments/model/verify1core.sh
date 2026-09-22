#!/bin/bash
# E211의 잠긴 예측을 1코어 비트스트림으로 검증한다.
# 비트스트림 완성 후 실행: bash experiments/model/verify1core.sh
set -e
R=/home/jaemin/03_gemmini/vivado-risc-v
SP=/tmp/claude-1001/-home-jaemin-03-gemmini-vivado-risc-v/da30ae44-61a9-4ad2-94ae-000bd6879647/scratchpad
BIT=$R/workspace/rocket64b1gem3i8w256f50/vivado-u280-riscv/u280-riscv.runs/impl_1/riscv_wrapper.bit
[ -f "$BIT" ] || { echo "비트스트림 없음"; exit 1; }

echo "=== 1. 타이밍·면적 ==="
D=$(dirname $BIT)
grep -A 6 "Design Timing Summary" $D/*timing_summary_routed.rpt | tail -1
grep -E "CLB LUTs|^\| CLB  " $D/*utilization_placed.rpt | head -3

echo "=== 2. 프로그래밍 ==="
source /opt/Xilinx/Vivado/2023.2/settings64.sh
env HW_SERVER_URL=tcp:localhost:3121 xsdb -quiet $R/board/jtag-freq.tcl 2>&1 | tail -1
env BITSTREAM=$BIT HW_SERVER_URL=tcp:localhost:3121 xsdb -quiet $R/board/jtag-boot.tcl 2>&1 | tail -2

echo "=== 3. 부팅 대기 ==="
for i in $(seq 1 60); do
  if timeout 3 bash -c 'echo > /dev/tcp/192.168.1.120/22' 2>/dev/null; then
    echo "포트22 열림(${i}회)"; break; fi; sleep 10; done

echo "=== 4. 측정 (코어 1개이므로 taskset 불필요) ==="
rm -f /srv/nfs/debian-riscv64/tmp/{rf1c,sp1c}.log
timeout 900 python3 $SP/tssh.py 'echo debian | sudo -S mount --bind / /mnt2 2>/dev/null; cd /tmp; cp /mnt2/tmp/roofline2 /mnt2/tmp/spill2 . && chmod +x roofline2 spill2 && echo debian | sudo -S bash -c "ulimit -l unlimited; cd /tmp; nproc; timeout 300 ./roofline2 /mnt2/tmp/rf1c.log; timeout 300 ./spill2 /mnt2/tmp/sp1c.log"; echo RC=$?' 850 2>&1 | tail -3

echo "=== 5. 결과 ==="
cat /srv/nfs/debian-riscv64/tmp/rf1c.log 2>/dev/null
echo "--- 큰 크기 ---"
grep -E "^ +[0-9]+³" /srv/nfs/debian-riscv64/tmp/sp1c.log 2>/dev/null
