set_property BITSTREAM.CONFIG.UNUSEDPIN pulldown [current_design]
set_property BITSTREAM.GENERAL.COMPRESS true [current_design]
set_property BITSTREAM.CONFIG.SPI_BUSWIDTH 4 [current_design]
set_property BITSTREAM.CONFIG.CONFIGRATE 63.8 [current_design]
set_property BITSTREAM.CONFIG.SPI_FALL_EDGE YES [current_design]
set_property BITSTREAM.CONFIG.SPI_32BIT_ADDR YES [current_design]
set_property BITSTREAM.CONFIG.CONFIGFALLBACK Enable [current_design]
set_property BITSTREAM.CONFIG.EXTMASTERCCLK_EN DISABLE [current_design]
set_property BITSTREAM.CONFIG.OVERTEMPSHUTDOWN Enable [current_design]
set_property CONFIG_VOLTAGE 1.8 [current_design]
set_property CONFIG_MODE SPIx4 [current_design]
set_property CFGBVS GND [current_design]

# Warn if the design exceeds what the card can supply
set_operating_conditions -design_power_budget 160

# USER_SI570_CLOCK, 156.25 MHz - input of clk_wiz_0
set_property -dict {LOC G30 IOSTANDARD LVDS} [get_ports clk_user_clk_p]
set_property -dict {LOC F30 IOSTANDARD LVDS} [get_ports clk_user_clk_n]

# CPU_RESET button, active low
set_property -dict {LOC L30 IOSTANDARD LVCMOS18} [get_ports resetn]
set_false_path -from [get_ports {resetn}]
set_input_delay 0 [get_ports {resetn}]

# HBM_CATTRIP - active high HBM over-temperature indicator to the satellite
# controller. Driving it high shuts the card power off, so the block design
# ties it low. Keep the pulldown as a second line of defence.
set_property -dict {LOC D32 IOSTANDARD LVCMOS18 PULLDOWN true} [get_ports hbm_cattrip]
set_false_path -to [get_ports {hbm_cattrip}]
set_output_delay 0 [get_ports {hbm_cattrip}]
