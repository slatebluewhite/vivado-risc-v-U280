#!/bin/bash
# JTAG-boot an already-built bitstream + kernel + boot.elf. Never builds anything:
# missing artifacts abort with the make command that would create them.
#
#   ./jtag-boot.sh                     # CONFIG/BOARD from workspace/config
#   ./jtag-boot.sh rocket64b2 u280     # explicit config/board
#   ./jtag-boot.sh -b path/to/file.bit # explicit bitstream (CONFIG/BOARD only for boot.elf/Image lookup)
#   ./jtag-boot.sh -n ...              # dry run: check and show what would boot, don't touch the FPGA

set -e
cd -P "$(dirname "$0")"

DRY_RUN=
BIT=
while getopts nb: opt; do
  case $opt in
    n) DRY_RUN=1 ;;
    b) BIT=$OPTARG ;;
    *) exit 1 ;;
  esac
done
shift $((OPTIND-1))

# defaults from workspace/config, overridable by positional args
if [ -f workspace/config ]; then
  eval "$(sed -n 's/^ *\(CONFIG\|BOARD\|ROOTFS\|HW_SERVER_ADDR\) *= *\(.*\)/\1=\2/p' workspace/config)"
fi
CONFIG=${1:-${CONFIG:-rocket64b2}}
BOARD=${2:-${BOARD:-nexys-video}}
ROOTFS=${ROOTFS:-NFS}
HW_SERVER_ADDR=${HW_SERVER_ADDR:-localhost:3121}

[ -n "$BIT" ] || BIT=workspace/$CONFIG/vivado-$BOARD-riscv/$BOARD-riscv.runs/impl_1/riscv_wrapper.bit
IMAGE=linux-stable/arch/riscv/boot/Image
BOOTELF=workspace/boot.elf

fail=0
require() { # file, hint
  if [ -f "$1" ]; then
    printf '  %-14s %s  (%s)\n' "$3" "$1" "$(date -r "$1" '+%m-%d %H:%M')"
  else
    printf '  %-14s MISSING: %s\n                 -> %s\n' "$3" "$1" "$2"
    fail=1
  fi
}

echo "Boot plan: CONFIG=$CONFIG BOARD=$BOARD ROOTFS=$ROOTFS"
require "$BIT"     "make CONFIG=$CONFIG BOARD=$BOARD bitstream" "bitstream"
require "$IMAGE"   "make linux"                                 "kernel"
require "$BOOTELF" "make u-boot bootloader"                     "boot.elf"
[ $fail = 0 ] || { echo "Aborted - nothing was programmed."; exit 1; }

# ramdisk: NFS boot needs only a 32-byte dummy; make it ourselves, no make involved
if [ "$ROOTFS" = NFS ]; then
  mkdir -p debian-riscv64
  dd if=/dev/zero of=debian-riscv64/ramdisk bs=32 count=1 status=none
elif [ ! -f debian-riscv64/ramdisk ]; then
  echo "ROOTFS=$ROOTFS needs debian-riscv64/ramdisk -> make jtag-boot JTAG_BOOT=1 (once)"; exit 1
fi

[ -n "$DRY_RUN" ] && { echo "Dry run - stopping before FPGA programming."; exit 0; }

# Vivado tools on PATH; fall back to newest install under /opt/Xilinx
if ! command -v xsdb >/dev/null; then
  settings=$(ls -1v /opt/Xilinx/Vivado/*/settings64.sh 2>/dev/null | tail -1)
  [ -n "$settings" ] || { echo "xsdb not found and no Vivado under /opt/Xilinx"; exit 1; }
  source "$settings"
fi

# hw_server must be listening
if ! (exec 3<>/dev/tcp/${HW_SERVER_ADDR%:*}/${HW_SERVER_ADDR#*:}) 2>/dev/null; then
  echo "Starting hw_server..."
  nohup hw_server >/dev/null 2>&1 &
  sleep 2
fi

env HW_SERVER_URL=tcp:$HW_SERVER_ADDR xsdb -quiet board/jtag-freq.tcl
env BITSTREAM="$BIT" HW_SERVER_URL=tcp:$HW_SERVER_ADDR xsdb -quiet board/jtag-boot.tcl

echo
echo "CPU started. Console: sudo miniterm /dev/ttyUSB2 115200  (U280)"
