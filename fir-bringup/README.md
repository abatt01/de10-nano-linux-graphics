# DE10-Nano fixed-point FIR accelerator

A 128-tap low-pass FIR for signed 16-bit PCM. The Cyclone V FPGA reads samples from HPS DDR, filters them, writes the results back, and signals completion to Linux with an interrupt. The board runner compares the FPGA output bit-for-bit with a Cortex-A9 C implementation and reports elapsed times for both paths.

[Build, validate, run on the board, and restore the prior FPGA image](fir128/README.md).

The project includes the FIR RTL, ARM runner, deterministic audio fixture generator, integer reference, RTL testbench, and device-tree overlay. The FPGA build uses Terasic's Revision E System CD as an external dependency.
