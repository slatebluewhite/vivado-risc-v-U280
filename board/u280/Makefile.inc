# Supported Vivado versions for this board: 2021.2 up to 2023.2.
# Newer Vivado releases are not supported - there is no riscv-<ver>.tcl for them.
BOARD_PART  ?= xilinx.com:au280:part0:1.2
XILINX_PART ?= xcu280-fsvh2892-2L-e
CFG_DEVICE  ?= SPIx4 -size 128
CFG_PART    ?= mt25qu01g-spi-x1_x2_x4
CFG_BOOT    ?= -loaddata {up 0x07000000 workspace/boot.elf}
ROOTFS      ?= NFS

# U280 has 2 DDR4 channels of 16GB each - unlike U200/U250 there is no 4 channel
# option, so %m4 configs are built with 2 memory controllers, same as %m2.
ifneq ($(filter %m2 %m4,$(CONFIG)),)
MEMORY_SIZE ?= 0x800000000
else
MEMORY_SIZE ?= 0x400000000
endif
