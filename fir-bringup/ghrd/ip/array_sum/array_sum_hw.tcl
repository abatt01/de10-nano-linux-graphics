package require -exact qsys 16.1

set_module_property NAME array_sum
set_module_property VERSION 1.0
set_module_property DISPLAY_NAME "Array Sum Accelerator"
set_module_property DESCRIPTION "Sums an array of 32-bit numbers in HPS memory and interrupts when done"
set_module_property GROUP "Tutorial"
set_module_property AUTHOR "Tutorial"
set_module_property EDITABLE false
set_module_property INSTANTIATE_IN_SYSTEM_MODULE true

add_fileset QUARTUS_SYNTH QUARTUS_SYNTH "" ""
set_fileset_property QUARTUS_SYNTH TOP_LEVEL array_sum
add_fileset_file array_sum.v VERILOG PATH array_sum.v TOP_LEVEL_FILE

# Clock
add_interface clock clock end
set_interface_property clock clockRate 0
add_interface_port clock clk clk Input 1

# Reset
add_interface reset reset end
set_interface_property reset associatedClock clock
set_interface_property reset synchronousEdges DEASSERT
add_interface_port reset reset reset Input 1

# Avalon-MM register interface
add_interface csr avalon end
set_interface_property csr addressUnits WORDS
set_interface_property csr associatedClock clock
set_interface_property csr associatedReset reset
set_interface_property csr bitsPerSymbol 8
set_interface_property csr readLatency 0
set_interface_property csr readWaitTime 1
set_interface_property csr writeWaitTime 0
set_interface_property csr maximumPendingReadTransactions 0
add_interface_port csr avs_address address Input 4
add_interface_port csr avs_read read Input 1
add_interface_port csr avs_readdata readdata Output 32
add_interface_port csr avs_write write Input 1
add_interface_port csr avs_writedata writedata Input 32

# Avalon-MM master for 64-bit burst reads
add_interface m0 avalon start
set_interface_property m0 addressUnits SYMBOLS
set_interface_property m0 associatedClock clock
set_interface_property m0 associatedReset reset
set_interface_property m0 bitsPerSymbol 8
set_interface_property m0 burstOnBurstBoundariesOnly false
set_interface_property m0 burstcountUnits WORDS
set_interface_property m0 doStreamReads false
set_interface_property m0 doStreamWrites false
set_interface_property m0 linewrapBursts false
set_interface_property m0 readLatency 0
add_interface_port m0 avm_address address Output 32
add_interface_port m0 avm_burstcount burstcount Output 5
add_interface_port m0 avm_read read Output 1
add_interface_port m0 avm_waitrequest waitrequest Input 1
add_interface_port m0 avm_readdata readdata Input 64
add_interface_port m0 avm_readdatavalid readdatavalid Input 1

# Interrupt to the HPS
add_interface interrupt_sender interrupt end
set_interface_property interrupt_sender associatedAddressablePoint csr
set_interface_property interrupt_sender associatedClock clock
set_interface_property interrupt_sender associatedReset reset
add_interface_port interrupt_sender irq irq Output 1
