# QSFP28 interface 0, lane 1 - Quad 134, GTYE4_CHANNEL_X0Y40 / GTYE4_COMMON_X0Y10
set_property -dict {LOC L53 } [get_ports qsfp0_rx1_p] ;# MGTYRXP0_134
set_property -dict {LOC L54 } [get_ports qsfp0_rx1_n] ;# MGTYRXN0_134
set_property -dict {LOC L48 } [get_ports qsfp0_tx1_p] ;# MGTYTXP0_134
set_property -dict {LOC L49 } [get_ports qsfp0_tx1_n] ;# MGTYTXN0_134
#set_property -dict {LOC K51 } [get_ports qsfp0_rx2_p] ;# MGTYRXP1_134 GTYE4_CHANNEL_X0Y41
#set_property -dict {LOC K52 } [get_ports qsfp0_rx2_n] ;# MGTYRXN1_134 GTYE4_CHANNEL_X0Y41
#set_property -dict {LOC L44 } [get_ports qsfp0_tx2_p] ;# MGTYTXP1_134 GTYE4_CHANNEL_X0Y41
#set_property -dict {LOC L45 } [get_ports qsfp0_tx2_n] ;# MGTYTXN1_134 GTYE4_CHANNEL_X0Y41
#set_property -dict {LOC J53 } [get_ports qsfp0_rx3_p] ;# MGTYRXP2_134 GTYE4_CHANNEL_X0Y42
#set_property -dict {LOC J54 } [get_ports qsfp0_rx3_n] ;# MGTYRXN2_134 GTYE4_CHANNEL_X0Y42
#set_property -dict {LOC K46 } [get_ports qsfp0_tx3_p] ;# MGTYTXP2_134 GTYE4_CHANNEL_X0Y42
#set_property -dict {LOC K47 } [get_ports qsfp0_tx3_n] ;# MGTYTXN2_134 GTYE4_CHANNEL_X0Y42
#set_property -dict {LOC H51 } [get_ports qsfp0_rx4_p] ;# MGTYRXP3_134 GTYE4_CHANNEL_X0Y43
#set_property -dict {LOC H52 } [get_ports qsfp0_rx4_n] ;# MGTYRXN3_134 GTYE4_CHANNEL_X0Y43
#set_property -dict {LOC J48 } [get_ports qsfp0_tx4_p] ;# MGTYTXP3_134 GTYE4_CHANNEL_X0Y43
#set_property -dict {LOC J49 } [get_ports qsfp0_tx4_n] ;# MGTYTXN3_134 GTYE4_CHANNEL_X0Y43

# QSFP0 REFCLK0, 156.25 MHz from the user SI570
#set_property -dict {LOC T42 } [get_ports qsfp0_mgt_refclk_0_p] ;# MGTREFCLK0P_134
#set_property -dict {LOC T43 } [get_ports qsfp0_mgt_refclk_0_n] ;# MGTREFCLK0N_134

# QSFP0 REFCLK1, from the SI546 programmable oscillator
set_property -dict {LOC R40 } [get_ports qsfp0_mgt_refclk_1_p] ;# MGTREFCLK1P_134
set_property -dict {LOC R41 } [get_ports qsfp0_mgt_refclk_1_n] ;# MGTREFCLK1N_134

# ethernet_sfp_10g drives sfp_fs = 2'b10. On U280 the SI546 has a single
# frequency select pin plus an active low output enable, so:
#   qsfp0_fs[1] = 1 -> QSFP0_FS  = 161.1328125 MHz (0 would select 156.25 MHz)
#   qsfp0_fs[0] = 0 -> QSFP0_OEB = oscillator output enabled
set_property -dict {LOC G32 IOSTANDARD LVCMOS18 SLEW SLOW DRIVE 8} [get_ports {qsfp0_fs[1]}]
set_property -dict {LOC H32 IOSTANDARD LVCMOS18 SLEW SLOW DRIVE 8} [get_ports {qsfp0_fs[0]}]

# 156.25 MHz MGT reference clock (SI546, FS = 0)
#create_clock -period 6.400 -name qsfp0_mgt_refclk_1 [get_ports qsfp0_mgt_refclk_1_p]

# 161.1328125 MHz MGT reference clock (SI546, FS = 1)
create_clock -period 6.206 -name qsfp0_mgt_refclk_1 [get_ports qsfp0_mgt_refclk_1_p]

set_false_path -to [get_ports {qsfp0_fs[*]}]
set_output_delay 0 [get_ports {qsfp0_fs[*]}]

# QSFP28 interface 1 - Quad 135, GTYE4_CHANNEL_X0Y44..47, not used
#set_property -dict {LOC G53 } [get_ports qsfp1_rx1_p] ;# MGTYRXP0_135
#set_property -dict {LOC G54 } [get_ports qsfp1_rx1_n] ;# MGTYRXN0_135
#set_property -dict {LOC G48 } [get_ports qsfp1_tx1_p] ;# MGTYTXP0_135
#set_property -dict {LOC G49 } [get_ports qsfp1_tx1_n] ;# MGTYTXN0_135
#set_property -dict {LOC M42 } [get_ports qsfp1_mgt_refclk_1_p] ;# MGTREFCLK1P_135
#set_property -dict {LOC M43 } [get_ports qsfp1_mgt_refclk_1_n] ;# MGTREFCLK1N_135
