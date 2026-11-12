# Testing and debugging

## Recorded build and initialization

The six user applications and both kernel modules compiled natively against the board's Linux 3.18.0 build tree. The terminal transcript is in [BUILD_LOG.txt](BUILD_LOG.txt).

The first driver insertion failed at the pixel-format check because the BS field was extracted using shift 2 instead of shift 8. The kernel log identified that check. Correcting the expression to `((status >> 8) & 15)` and rebuilding resolved initialization; module loading was confirmed afterward.

The checks below are reproducible acceptance criteria. Timing values are calculated from a nominal 60 Hz refresh and should not be treated as measured benchmark results. The repository does not include captured HDMI screenshots or a demonstration video.

## Before testing

1. Load the course DE10-Nano Computer FPGA graphics image and enable the appropriate bridges.
2. Connect HDMI and select that input on the monitor.
3. Run `make` from the project folder and use root for module operations.
4. Stop the current application before changing module parameters.
5. Do not run competing video applications or KEY event consumers.

## Parts I-III

When no `video` module is loaded:

```sh
insmod video.ko double_buffer=0
cat /dev/video
echo clear > /dev/video
echo 'pixel 0,0 F800' > /dev/video
echo 'pixel 160,120 FFFF' > /dev/video
echo 'pixel 319,239 07E0' > /dev/video
```

Expect one resolution string, `320 240`, followed by EOF. Check the red upper-left pixel, white center pixel, and green lower-right pixel. `echo clear > /dev/video` should remove them.

```sh
./part1
```

Check red, green, and blue vertical thirds. The program intentionally sends one write per pixel.

```sh
./part2
```

Check continuous colored lines. Additional edge cases:

```sh
echo clear > /dev/video
echo 'line 319,239 0,0 F800' > /dev/video
echo 'line 160,0 160,239 07E0' > /dev/video
echo 'line 20,20 20,20 FFFF' > /dev/video
echo 'pixel 320,240 FFFF' > /dev/video
```

The final command should return a write error because the coordinates are outside the display. The earlier commands exercise reversed endpoints, a vertical line, and a single-point line.

```sh
./part3
```

The horizontal line should bounce at the top and bottom. A one-way traversal uses 239 increments: `239 / 60 = 3.98` nominal seconds. Press Ctrl+C and check that the screen clears.

## Part IV

```sh
rmmod video
insmod video.ko double_buffer=1
./part4
```

Check eight colored five-pixel rectangles and seven links. Each box should move diagonally and stay fully in bounds. Stop with Ctrl+C before comparing:

```sh
rmmod video
insmod video.ko double_buffer=0
./part4 --single
```

Observe differences in flicker, tearing, or partially drawn frames. Restore double buffering after stopping the program:

```sh
rmmod video
insmod video.ko double_buffer=1
```

## Part V

Check `/dev/KEY` and `/dev/SW`. Insert `key_sw.ko` only if both nodes are absent and no existing driver owns the registers.

```sh
ls -l /dev/KEY /dev/SW
# Only when both inputs are absent:
# insmod key_sw.ko
./part5
```

| Test | Acceptance criterion |
|---|---|
| Tap KEY0 | Sweep all eight speed levels in both directions |
| Tap KEY1 | Sweep object count from eight to 16, down to one, and back upward |
| All SW off | Chain links visible |
| Any SW on | Boxes visible, links hidden |
| Hold a button | No repeated actions for a single retained press event |
| Ctrl+C | Normal animation shutdown and cleanup |

If an existing KEY driver reports active-high pressed-key levels, use `--key-level`. Do not use this option as an inversion for raw active-low data. Manual reads of the event device consume its pending events.

## Part VI

After stopping Part V:

```sh
echo erase > /dev/video
echo 'text 0,0 DE10-Nano Graphics' > /dev/video
echo 'text 0,1 Character overlay test' > /dev/video
./part6
```

Check the two static text rows, then the increasing frame counter at the upper-left during animation. The default setting targets approximately 60 increments per second; slower settings intentionally reduce that rate. Recheck KEY/SW controls. On Ctrl+C, graphics and text should clear and the terminal should print the presented-frame count.

## Common issues

| Symptom | Diagnosis |
|---|---|
| `rmmod` says module does not exist | The module was not loaded; confirm with `lsmod` before removing it |
| `No such device` during insertion | Inspect `dmesg`; a hardware format/resolution check failed |
| Missing kernel build tree | Set `KDIR` to the configured tree matching `uname -r` |
| Invalid module format | Kernel configuration, release, architecture, or symbol versions do not match |
| No visible pixel drawing in double mode | Drawing targets the back buffer; issue `sync` |
| Synchronization timeout | The controller did not finish a swap; check the FPGA configuration and clocks |
| KEY changes on release or repeats | Check event polarity and the existing driver's level/event contract |
| Module is in use | Stop applications and close open device descriptors first |

```sh
dmesg | tail -n 25
cat /sys/module/video/parameters/double_buffer
```

## Finish

```sh
rmmod video
# Only if the supplied input module was loaded:
# rmmod key_sw
sync
```

Keep source files and build products for later testing. If powering down, use `shutdown -h now` and wait for Linux to halt before removing power.
