# Usage with Vitis IDE:
# In Vitis IDE create a Single Application Debug launch configuration,
# change the debug type to 'Attach to running target' and provide this 
# tcl script in 'Execute Script' option.
# Path of this script: /home/user/Desktop/lab_3/Vitis/zynq_keypad_system/_ide/scripts/systemdebugger_zynq_keypad_system_standalone.tcl
# 
# 
# Usage with xsct:
# To debug using xsct, launch xsct and run below command
# source /home/user/Desktop/lab_3/Vitis/zynq_keypad_system/_ide/scripts/systemdebugger_zynq_keypad_system_standalone.tcl
# 
connect -url tcp:127.0.0.1:3121
targets -set -nocase -filter {name =~"APU*"}
rst -system
after 3000
targets -set -filter {jtag_cable_name =~ "Digilent Zybo 210279A42C4EA" && level==0 && jtag_device_ctx=="jsn-Zybo-210279A42C4EA-13722093-0"}
fpga -file /home/user/Desktop/lab_3/Vitis/zynq_keypad/_ide/bitstream/zynq_keypad_system_wrapper.bit
targets -set -nocase -filter {name =~"APU*"}
loadhw -hw /home/user/Desktop/lab_3/Vitis/zynq_keypad_system_wrapper/export/zynq_keypad_system_wrapper/hw/zynq_keypad_system_wrapper.xsa -mem-ranges [list {0x40000000 0xbfffffff}] -regs
configparams force-mem-access 1
targets -set -nocase -filter {name =~"APU*"}
source /home/user/Desktop/lab_3/Vitis/zynq_keypad/_ide/psinit/ps7_init.tcl
ps7_init
ps7_post_config
targets -set -nocase -filter {name =~ "*A9*#0"}
dow /home/user/Desktop/lab_3/Vitis/zynq_keypad/Debug/zynq_keypad.elf
configparams force-mem-access 0
targets -set -nocase -filter {name =~ "*A9*#0"}
con
