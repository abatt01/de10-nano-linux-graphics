#!/usr/bin/env python3
"""Build the milestone PDF and PNG evidence from saved guitar FIR outputs."""
from datetime import date
from pathlib import Path
import struct

from PIL import Image, ImageDraw, ImageFont
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet
from reportlab.platypus import Image as PDFImage, PageBreak, Paragraph, SimpleDocTemplate, Spacer

DOCS = Path(__file__).resolve().parent
RUN = DOCS.parent / "fir128/build/guitar"
FONT = Path("/usr/share/fonts/truetype/dejavu")
BG = "#111a2a"
FG = "#e5edf5"
MUTED = "#a8b8cd"
TEAL = "#62dbc5"


def font(name, size):
    return ImageFont.truetype(str(FONT / f"DejaVuSans{name}.ttf"), size)


def terminal(path):
    lines = [
        ("Live comparison | DE10-Nano Cortex-A9 and Cyclone V", TEAL),
        *[(line, FG) for line in (RUN / "board-result.log").read_text().splitlines()],
        ("", FG),
        ("FPGA RTL simulation | ARM output used as expected samples", TEAL),
        *[(line, FG) for line in (RUN / "guitar-validation.log").read_text().splitlines()],
    ]
    image = Image.new("RGB", (1080, 40 + 26 * len(lines)), BG)
    draw = ImageDraw.Draw(image)
    for index, (line, color) in enumerate(lines):
        draw.text((22, 16 + index * 26), line, font=font("Mono", 19), fill=color)
    image.save(path, optimize=True)


def read_pcm(name):
    data = (RUN / name).read_bytes()
    return struct.unpack("<16384h", data)


def waveform(path):
    image = Image.new("RGB", (1080, 540), BG)
    draw = ImageDraw.Draw(image)
    draw.text((26, 20), "Guitar-style pluck | first 80 ms", font=font("-Bold", 29), fill=FG)
    draw.text((27, 62), "Same PCM amplitude scale for both waveforms", font=font("", 18), fill=MUTED)
    for index, (name, title, color) in enumerate((
        ("input.s16le", "Unfiltered input", "#89b7ff"),
        ("fpga-live.s16le", "Filtered output captured on the FPGA (identical to ARM)", TEAL),
    )):
        samples = read_pcm(name)[:3840]
        y = 111 + index * 187
        draw.text((84, y), title, font=font("-Bold", 20), fill=color)
        left, right, middle = 84, 1048, y + 86
        for ms in range(0, 81, 10):
            x = left + (right - left) * ms / 80
            draw.line((x, y + 32, x, y + 140), fill="#30415a")
            if index == 1:
                draw.text((x, y + 144), str(ms), font=font("", 15), fill=MUTED, anchor="mt")
        for amplitude in (-25000, 0, 25000):
            level = middle - 52 * amplitude / 25000
            draw.line((left, level, right, level), fill="#30415a")
            draw.text((left - 9, level), f"{amplitude:,}", font=font("", 14), fill=MUTED, anchor="rm")
        for x in range(left, right + 1):
            start = (x - left) * len(samples) // (right - left + 1)
            end = (x - left + 1) * len(samples) // (right - left + 1)
            bucket = samples[start:end]
            draw.line((x, middle - 52 * max(bucket) / 25000,
                       x, middle - 52 * min(bucket) / 25000), fill=color)
    draw.text((566, 506), "Time (ms)", font=font("", 18), fill=MUTED, anchor="mt")
    image.save(path, optimize=True)


def make_pdf(output, terminal_path, waveform_path):
    styles = getSampleStyleSheet()
    styles["Title"].textColor = colors.HexColor("#112a42")
    styles["Heading2"].textColor = colors.HexColor("#0c685c")
    styles["BodyText"].leading = 15
    result = (RUN / "board-result.log").read_text().splitlines()
    arm_time = next(line.split()[-2] for line in result if line.startswith("ARM CPU"))
    fpga_time = next(line.split()[-2] for line in result if line.startswith("FPGA end-to-end"))
    story = []

    def add(text, style="BodyText"):
        story.append(Paragraph(text, styles[style]))

    def figure(path):
        with Image.open(path) as image:
            width, height = image.size
        story.append(PDFImage(str(path), width=492, height=492 * height / width))
        story.append(Spacer(1, 9))

    add("DE10-Nano guitar FIR processing", "Title")
    add(f"Living project report | {date.today().isoformat()}")
    add("Signal path", "Heading2")
    add("A mono 48 kHz, 16-bit PCM WAV is cut to a 16,384-sample window (341.3 ms). "
        "Both compute paths apply a 128-tap Hamming-windowed low-pass FIR with a nominal "
        "6 kHz cutoff and Q1.15 coefficients. Each path starts with zeroed history, rounds ties "
        "away from zero, and saturates to signed 16 bits. Filtered PCM can be wrapped back into a WAV.")
    add("ARM and FPGA implementation", "Heading2")
    add("The Cortex-A9 C reference uses a circular history and a signed 64-bit accumulator. "
        "The Cyclone V RTL uses one MAC, a signed 48-bit accumulator, and HPS DDR staging via "
        "the FPGA-to-HPS bridge. Linux configures the engine through the lightweight bridge and "
        "waits for an interrupt raised after the final downstream AXI write response.")
    add("Live comparison", "Heading2")
    add(f"All <b>16,384 samples matched</b> on the physical board. The ARM took "
        f"<b>{arm_time} ms</b> and the FPGA took <b>{fpga_time} ms end to end</b>. "
        "The current FPGA path was slower for this clip. An earlier ARM-only run "
        "matched the host reference byte-for-byte, and the FPGA RTL simulation "
        "matched that output and passed the DDR and interrupt checks.")
    figure(terminal_path)
    add("Stock FPGA recovery", "Heading2")
    restored = RUN / "stock-restored.log"
    if restored.exists() and "fpga_led7" in restored.read_text():
        add("After reboot, SSH confirmed the stock overlay was applied, the FPGA manager "
            "was operating, and all eight stock FPGA LEDs were present.")
    else:
        add("A reboot was requested after saving the results to restore the stock FFT and "
            "GPIO design. Verification is pending USB reconnection.")

    story.append(PageBreak())
    add("Output waveform", "Title")
    add("The input is a generated guitar-style E minor pluck. Use the same run path with a "
        "recorded guitar WAV to validate a real performance.")
    story.append(Spacer(1, 14))
    figure(waveform_path)
    add("What the timing covers", "Heading2")
    add("The ARM time covers C filtering. The FPGA end-to-end time includes DDR staging, "
        "register programming, the IRQ wait, and copying the output to userspace. "
        "The reported ratio is ARM time divided by FPGA time for one run. "
        "The experiment timed the whole FPGA path, so the data cannot isolate the "
        "contribution of any individual stage.")
    add("Reproduce", "Heading2")
    add('See <link href="https://github.com/abatt01/de10-nano-linux-graphics/blob/fir-bringup/fir-bringup/fir128/README.md" '
        'color="#0c685c">the run guide</link> for fixture generation, host and RTL validation, '
        "Quartus build, board preflight, deployment, rollback, and guitar WAV conversion.")
    SimpleDocTemplate(str(output), title="DE10-Nano guitar FIR processing",
                      leftMargin=60, rightMargin=60).build(story)


def main():
    terminal_path = DOCS / "guitar-fir-terminal.png"
    waveform_path = DOCS / "guitar-fir-waveform.png"
    terminal(terminal_path)
    waveform(waveform_path)
    make_pdf(DOCS / "guitar-fir-report.pdf", terminal_path, waveform_path)


if __name__ == "__main__":
    main()
