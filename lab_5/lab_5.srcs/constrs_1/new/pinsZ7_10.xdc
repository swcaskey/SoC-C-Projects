## PWM Output - LED0
set_property -dict { PACKAGE_PIN M14 IOSTANDARD LVCMOS33 } [get_ports pwm0]

## AXI GPIO Inputs - Zybo Z7-10 Buttons
set_property -dict { PACKAGE_PIN K18 IOSTANDARD LVCMOS33 } [get_ports {btns_4bits[0]}]
set_property -dict { PACKAGE_PIN P16 IOSTANDARD LVCMOS33 } [get_ports {btns_4bits[1]}]
set_property -dict { PACKAGE_PIN K19 IOSTANDARD LVCMOS33 } [get_ports {btns_4bits[2]}]
set_property -dict { PACKAGE_PIN Y16 IOSTANDARD LVCMOS33 } [get_ports {btns_4bits[3]}]
