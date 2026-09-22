# UART - FTDI FT4232 port 3, bank 75 (1.8V). Console shows up as /dev/ttyUSB2.
# Pin assignment hardware-verified (JTAG-loaded TX probe, 2026-08-15):
# B33 = FPGA TX, A28 = FPGA RX. The Xilinx AU280 master XDC names these two
# pins the other way round (FTDI perspective) - the board file is correct.
set_property -dict {PACKAGE_PIN B33 IOSTANDARD LVCMOS18} [get_ports usb_uart_txd]
set_property -dict {PACKAGE_PIN A28 IOSTANDARD LVCMOS18} [get_ports usb_uart_rxd]
