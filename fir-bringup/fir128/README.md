# DE10-Nano fixed FIR bring-up

The single case is a 32 KiB mono signed 16-bit little-endian PCM clip at 48 kHz, 16,384 samples, with 1.5 kHz and 12 kHz tones at amplitudes 14,000 and 11,000. No supplied clip or coefficient table was found in the workspace. On Ubuntu, `python3 fixture.py` regenerates the clip and coefficient files. The C initializer and RTL ROM are generated from the same 128-entry Hamming-windowed, unity-DC sinc at a nominal 6 kHz cutoff, quantized to signed Q1.15. [SHA256SUMS](SHA256SUMS) records the clip, coefficient and validation output digests. The Python oracle is only used for host validation.

The baseline uses a zeroed circular history, signed products and a signed 64-bit accumulator. The FPGA clears its own 128-sample history at start and uses one MAC with a signed 48-bit accumulator. Both round ties away from zero and saturate to signed 16 bits. The runner times one execution of each path using ARM `CLOCK_MONOTONIC`. The FPGA time includes DDR staging, barriers, register programming, the IRQ wait and output copy-back. Mapping, file reads, IRQ setup and comparison are outside the timers.

## Ubuntu build and validation

Install Quartus Prime Lite 22.1std with Cyclone V device support, including Platform Designer. On Ubuntu, install the host tools with `sudo apt install build-essential python3 patch device-tree-compiler iverilog openssh-client`.

Extract the [Terasic Revision E System CD](https://download.terasic.com/downloads/cd-rom/de10-nano/) v1.1.0. From this directory, point `GHRD_DIR` at the extracted `Demonstrations/SoC_FPGA/DE10_NANO_SoC_GHRD` folder. Set `QUARTUS_ROOTDIR` if Quartus is installed outside the default path shown below.

```sh
cd fir128
export GHRD_DIR=/absolute/path/to/Demonstrations/SoC_FPGA/DE10_NANO_SoC_GHRD
export QUARTUS_ROOTDIR="$HOME/intelFPGA_lite/22.1std/quartus"
python3 fixture.py
./validate.sh
mkdir -p build
set -o pipefail
./build.sh 2>&1 | tee build/build.log
```

The build copies the System CD project inputs into `build/`, applies the included Platform Designer migration, generates the FIR system, and writes `build/fir128.rbf`, `build/fir128.dtbo`, and `build/firmware.sha256`. The standalone device-tree compiler warns about the parent address-cell context because the overlay gains its actual parent when applied.

To build the array-sum reference image and overlay from the same System CD, run `./build.sh array_sum`. Outputs and checksums go into `build/array_sum/`. The deployment procedure below assumes a known-working array-sum image is already installed and loaded on the board. Keep its actual original firmware files for rollback.

The validation script checks SHA-256, compares all C outputs with an integer oracle and compiles the runner with `-O2`. Where an existing Icarus Verilog installation is available it also checks signed quantization vectors, invalid setup, Avalon command stability under backpressure, all input and output addresses and samples, and the completion IRQ after the final write response.

## Memory and registers

The generated System Designer wiring uses the tutorial’s lightweight HPS-to-FPGA bridge at offset `0x40000`, the HPS FPGA-to-HPS AXI slave for DDR, and completion on `f2h_irq0[8]` (GIC SPI 48, GIC ID 80, active-high level). The overlay uses the same IRQ and maps the CSR at physical `0xff240000`, span `0x40`. The low four words reproduce the PIO header and its write-one-to-clear edge latch.

| Offset | Definition |
| --- | --- |
| 0x00 | PIO data, done on bit 0 |
| 0x04 | PIO input direction, reads zero |
| 0x08 | PIO IRQ mask, bit 0 |
| 0x0c | PIO edge capture, write-one-to-clear bit 0 |
| 0x10 | Start, write bit 0 while idle |
| 0x14 | Status, bits 0 busy, 1 done, 2 fault |
| 0x18 | Physical input address |
| 0x1c | Sample count |
| 0x20 | Physical output address |
| 0x2c | Image ID, `0x46495231` |

The RTL accepts only input `[0x3f800000, 0x3f808000)`, output `[0x3fc00000, 0x3fc08000)` and count 16,384. It transfers four little-endian samples per 64-bit beat and raises successful completion after the final downstream AXI write response. The generated AXI master uses a nonbufferable transaction. Verify that the boot reports physical DDR through `0x3fffffff`, Linux RAM is capped at `0x3effffff` by `mem=0x3F000000` or equivalent, and that the staging area is unused by other firmware, devices or FPGA masters. The workspace copies support the CSR and IRQ definitions. The live boot excludes both ranges from Linux System RAM.

## Read-only board preflight

Run from Ubuntu with the working SSH account and existing host key.

```sh
cd fir128
TARGET=root@de10-nano.local  # Replace with the board's known SSH host and account.
set -o pipefail
ssh -o StrictHostKeyChecking=yes "$TARGET" 'sh -s' < board_state.sh | tee board-state.log
```

Verify the board boots Linux with FPGA manager, configfs device-tree overlays, GPIO sysfs, and GCC. Compare the live DDR map with the ranges above and confirm that no firmware, devices, or external JTAG clients use them during the run.

## Swap and rollback

These steps assume the active configfs slot is `array_sum` with `path=array_sum.dtbo`, the manager reports `operating`, and the original DTBO and RBF are still installed. Verify these conditions and record the original file hashes before proceeding. Adapt the procedure to the actual overlay loading mechanism if the preflight differs.

Stage the files from **Ubuntu** using the verified SSH target. If the board lacks an SFTP subsystem, use `scp -O` with a current OpenSSH client.

```sh
set -e
STAGE=$(ssh -o StrictHostKeyChecking=yes "$TARGET" 'mktemp -d /tmp/fir128.XXXXXX')
test -n "$STAGE"
sed -n '/  mixed\.s16le$/p' SHA256SUMS > build/clip.sha256
scp -o StrictHostKeyChecking=yes build/fir128.rbf build/fir128.dtbo \
    build/firmware.sha256 build/clip.sha256 mixed.s16le fir128_test.c \
    reference.h coeff.inc ../sw/fpga_irqs_off.c "$TARGET:$STAGE/"
ssh -t -o StrictHostKeyChecking=yes "$TARGET" "cd '$STAGE' && exec sh"
```

Run on the **board** as root, from the staging directory.

```sh
OV=/sys/kernel/config/device-tree/overlays
(
set -eu
test "$(cat "$OV/array_sum/status")" = applied
test "$(cat "$OV/array_sum/path")" = array_sum.dtbo
test ! -e "$OV/fir128"
test ! -e /lib/firmware/fir128.rbf
test ! -e /lib/firmware/fir128.dtbo
sha256sum -c firmware.sha256
sha256sum -c clip.sha256
gcc -Wall -Wextra -Werror -std=c11 -O2 fir128_test.c -o fir128_test
gcc -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64 fpga_irqs_off.c -o fpga_irqs_off
cp fir128.rbf fir128.dtbo /lib/firmware/
chmod 0644 /lib/firmware/fir128.rbf /lib/firmware/fir128.dtbo
cat /proc/interrupts > irq-before.log
rmdir "$OV/array_sum"
./fpga_irqs_off
mkdir "$OV/fir128"
printf '%s' fir128.dtbo > "$OV/fir128/path"
test "$(cat "$OV/fir128/status")" = applied
cat /sys/class/fpga_manager/fpga0/state
test "$(cat /sys/class/fpga_manager/fpga0/state)" = operating
tr -d '\000' < /proc/device-tree/soc/base-fpga-region/firmware-name
test "$(tr -d '\000' < /proc/device-tree/soc/base-fpga-region/firmware-name)" = fir128.rbf
./fir128_test mixed.s16le > board-result.log 2>&1
cat board-result.log
cat /proc/interrupts > irq-after.log
)
```

The runner checks `FIR1` and arms the bound GPIO interrupt before start. It requires a GPIO poll event and successful device status before copy-back. If a step fails, save `board-result.log` if present, manager/bridge and overlay status, `/proc/interrupts`, and `dmesg | tail -100`, then roll back. Do not launch another FIR run.

**Proposed rollback from the board staging directory.** This applies after the old overlay has been removed. If the old overlay remains applied, preserve it. Run from the same board staging directory.

```sh
set -eu
if [ -d "$OV/fir128" ]; then rmdir "$OV/fir128"; fi
./fpga_irqs_off
mkdir "$OV/array_sum"
printf '%s' array_sum.dtbo > "$OV/array_sum/path"
test "$(cat "$OV/array_sum/status")" = applied
test "$(cat /sys/class/fpga_manager/fpga0/state)" = operating
test "$(tr -d '\000' < /proc/device-tree/soc/base-fpga-region/firmware-name)" = array_sum.rbf
```

After restoration, verify the tutorial ID `0x53554d31` at CSR offset `0x2c` and check the GPIO binding against the preflight. No reboot is part of these steps.

## Result

The runner reports ARM and FPGA elapsed times, speedup, and a sample-by-sample comparison. Back in the original Ubuntu shell, copy the result and IRQ logs into the local build directory.

```sh
scp -o StrictHostKeyChecking=yes \
    "$TARGET:$STAGE/board-result.log" "$TARGET:$STAGE/irq-before.log" \
    "$TARGET:$STAGE/irq-after.log" build/
```

## Guitar clips

The board accepts one 16,384-frame window (341.3 ms at 48 kHz). Export a mono,
16-bit PCM WAV at 48 kHz from your recorder or audio editor, then select a
window by its starting frame. A generated guitar-style pluck is available as
a reproducible test fixture.

```sh
mkdir -p build/guitar
python3 guitar.py demo build/guitar/demo.wav
python3 guitar.py prepare build/guitar/demo.wav build/guitar/input.s16le
# For a recording, use its WAV path and optionally add --offset 4800.
gcc -std=c11 -Wall -Wextra -Werror -O2 fir128_test.c -o build/fir128_test_host
build/fir128_test_host --reference build/guitar/input.s16le build/guitar/reference.s16le
./validate.sh build/guitar/input.s16le build/guitar/reference.s16le
python3 guitar.py wrap build/guitar/reference.s16le build/guitar/reference.wav
```

To measure the reference on ARM without touching the current overlay, copy
`fir128_test.c`, `reference.h`, `coeff.inc`, and `input.s16le` to a board
staging directory. In that directory, compile and run the reference.

```sh
gcc -std=c11 -Wall -Wextra -Werror -O2 fir128_test.c -o fir128_test
./fir128_test --reference input.s16le arm.s16le
```

Once the FIR overlay is loaded and the buffer preflight passes, use the
comparison mode. This writes raw output files after the timed comparison.
Copy them back to the host and use `guitar.py wrap` to listen to either result.

```sh
./fir128_test input.s16le arm.s16le fpga.s16le
```

## Stock FFT baseline

The factory setup loads the stock RBF from the boot partition before Linux
starts. Linux then applies `de10-nano.dtbo` through
`de10-nano-fpga-init.service`. Before using this baseline, verify both stock
files and confirm that the boot service is enabled. Keep a working serial
console available, disconnect external JTAG, and schedule interruption of the
FPGA-backed desktop and FFT services.

After staging and verifying the FIR files as above, use the following stock
overlay teardown in place of the `array_sum` removal. Run from the board
staging directory as root after compiling `fpga_irqs_off`.

```sh
set -eu
OV=/sys/kernel/config/device-tree/overlays
test "$(ls -A "$OV")" = de10-nano.dtbo
test "$(cat "$OV/de10-nano.dtbo/status")" = applied
systemctl stop de10-nano-xfce-init.service de10-nano-fpga-leds.service de10-nano-fftsw-init.service
if [ -d /sys/module/fft_driver ]; then rmmod fft_driver; fi
test ! -d /sys/module/fft_driver
for driver in leds-gpio gpio-keys; do
    case "$driver" in leds-gpio) name=leds ;; gpio-keys) name=keys ;; esac
    base=/sys/bus/platform/drivers/$driver
    device=soc:base-fpga-region:$name
    if [ -L "$base/$device" ]; then printf '%s' "$device" > "$base/unbind"; fi
done
rmdir "$OV/de10-nano.dtbo"
./fpga_irqs_off
```

Apply and verify the FIR overlay, then run the comparison once. Save the
comparison log and PCM files off the board before rebooting. Reboot to restore
the stock bitstream and overlay, then confirm the stock overlay is applied, the
FPGA manager is operating, and the stock FPGA LED devices have returned.

The first live guitar-style run matched all 16,384 samples. The ARM took
22.968 ms and the FPGA end-to-end path took 47.828 ms. See the
[project report](../docs/guitar-fir-report.pdf) and
[terminal output](../docs/guitar-fir-terminal.png).
