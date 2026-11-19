#!/bin/sh
# Run as root on the board. Read-only inventory for the pre-swap review.
set -eu
uname -a
printf '\nDevice tree model and compatible\n'
for item in model compatible; do
    tr '\000' '\n' < "/proc/device-tree/$item"
done
printf '\nKernel command line and physical resources\n'
cat /proc/cmdline /proc/iomem
printf '\nBoot DT memory nodes and reservations\n'
for node in /proc/device-tree/memory* /proc/device-tree/reserved-memory/*; do
    if [ -f "$node/reg" ]; then
        printf '%s\n' "$node"
        od -An -tx1 "$node/reg"
    fi
done
printf '\nFPGA manager, bridge and installed overlay state\n'
ls -la /sys/kernel/config/device-tree/overlays 2>/dev/null || :
for state in /sys/class/fpga_manager/*/state /sys/class/fpga_bridge/*/state \
             /sys/class/fpga_bridge/*/enable \
             /sys/kernel/config/device-tree/overlays/*/status \
             /sys/kernel/config/device-tree/overlays/*/path; do
    if [ -f "$state" ]; then
        printf '%s\n' "$state"
        cat "$state"
    fi
done
for file in /proc/device-tree/soc/base-fpga-region/firmware-name /sys/module/firmware_class/parameters/path; do
    if [ -r "$file" ]; then
        printf '%s\n' "$file"
        tr '\000' '\n' < "$file"
    fi
done
sha256sum /lib/firmware/array_sum.rbf /lib/firmware/array_sum.dtbo 2>/dev/null || :
printf '\nGPIO controller labels and IRQ routes\n'
for label in /sys/class/gpio/gpiochip*/label; do
    [ ! -f "$label" ] || cat "$label"
done
cat /proc/interrupts
command -v gcc
