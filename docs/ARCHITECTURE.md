# Architecture and driver interface

## Hardware map

| Resource | Physical address | Details |
|---|---|---|
| Pixel controller | `0xFF203020` | Buffer +0, Backbuffer +4, Resolution +8, Status +12 |
| Pixel buffer A | `0xC8000000` | 128 KiB reservation |
| Pixel buffer B | `0xC8020000` | Second 128 KiB reservation |
| Character controller | `0xFF203030` | Resolution at +8 |
| Character memory | `0xC9000000` | 80 x 60 visible ASCII cells |
| SW PIO | `0xFF200040` | Low four bits are switch states |
| KEY PIO | `0xFF200050` | Low two edge-capture bits at +0xC |

Pixel address: `buffer_base + (y << 9) + x`. Each pixel occupies one byte, but rows reserve 512 bytes. The last visible pixel at (319,239) has offset `0x1DF3F`.

Character address: `0xC9000000 + (text_y << 7) + text_x`. Each character row reserves 128 bytes. Text cell (79,59) has offset `0x1DCF`.

## Linux device lifecycle

`start_video` maps I/O resources, checks controller configuration, clears the two pixel buffers, selects the front/back addresses, and registers a character device using `alloc_chrdev_region`, `cdev_add`, `class_create`, and `device_create`.

Failure paths unwind acquired resources. Removal deletes the device registrations, attempts to restore the original controller pointers, and unmaps I/O. Open descriptors pin the module through `THIS_MODULE`.

`device_read` uses a per-open offset to return the screen dimensions once, followed by EOF. `device_write` copies a bounded command from userspace and serializes its execution with a mutex. The interface does not assemble fragmented writes or accept multiple newline-separated commands in a single write.

## Commands

| Command | Effect |
|---|---|
| `clear` | Clear visible pixels in the drawing buffer |
| `pixel x,y color` | Draw a checked pixel |
| `line x1,y1 x2,y2 color` | Draw an inclusive Bresenham line |
| `box x1,y1 x2,y2 color` | Fill an inclusive rectangle; normalize reversed corners |
| `sync` | Request a vertical-blank swap and wait for completion |
| `erase` | Fill visible character cells with spaces |
| `text x,y string` | Write ASCII text, clipped at the right edge |

Colors are RGB565 hexadecimal, optionally prefixed with `0x`. The driver retains the high channel bits to produce RGB332:

```c
((color >> 8) & 0xe0) | ((color >> 6) & 0x1c) | ((color >> 3) & 0x03)
```

Coordinates outside the visible region and colors above `FFFF` are rejected. The maximum accepted write is 255 bytes. Trailing junk in structured drawing commands is rejected.

## Lines and filled rectangles

Bresenham drawing transposes steep lines and orders endpoints along the major axis. An integer error accumulator decides when to step along the minor axis. Horizontal, vertical, reversed-endpoint, and single-point lines share this routine without floating-point arithmetic.

Filled boxes normalize their corners and visit every pixel in the inclusive region. The animation's five-pixel boxes use top-left limits of x=0..315 and y=0..235.

## Buffer swaps

The Buffer register holds the front address, but writing `1` requests a swap rather than assigning that address. Status bit S is bit 0. A request sets it, and completion at vertical blank clears it.

With `double_buffer=0`, front and back point to buffer A. A swap provides timing without changing the visible memory region. With `double_buffer=1`, front and back point to different reservations. The application clears/draws the hidden buffer before requesting presentation.

The driver resolves the current back address for each drawing command, ensuring it follows completed swaps. A memory barrier orders preceding drawing writes before the swap request. Polling sleeps briefly between checks and times out after 200 ms.

The pixel-size BS field occupies **status bits 11:8**; the RGB332 configuration reports seven, meaning eight bits per symbol. The driver validates the expected 320 x 240 pixel and 80 x 60 character dimensions. Configuration checks do not make accesses to absent or unclocked FPGA hardware safe: the correct bitstream and enabled bridges remain prerequisites.

## Inputs and animation state

The optional `key_sw.ko` uses misc devices. KEY reads capture event bits, return ASCII hex, and acknowledge those bits. SW reads return the current switch mask. Applications reopen the input devices per sample to accommodate course drivers with per-open EOF behavior.

Each box stores position, signed x/y directions, and color. The application draws consecutive chain links and then the boxes, presents the frame, and advances positions. Movement is reflected one pixel substep at a time, preserving bounds even at higher displacement settings.

KEY0 changes speed and KEY1 changes object count. Each setting keeps its own sweep direction and reverses at its limits. Independent 150 ms debounce uses monotonic time. A captured KEY bit cannot count multiple presses occurring before it is consumed.

The text counter increments after successful pixel-frame presentation. Character memory is independent of the swapped pixel buffers, so text itself is not double-buffered.
