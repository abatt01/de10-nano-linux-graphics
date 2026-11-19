#!/usr/bin/env python3
"""Prepare the board's fixed-length mono PCM input or wrap output in WAV."""
import argparse
from pathlib import Path
import random
import struct
import wave

RATE = 48000
SAMPLES = 16384
BYTES = 2 * SAMPLES


def write_wav(path, pcm):
    with wave.open(str(path), "wb") as audio:
        audio.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        audio.writeframes(pcm)


def prepare(path, offset):
    with wave.open(str(path), "rb") as audio:
        if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) != (1, 2, RATE):
            raise ValueError("use a mono 16-bit PCM WAV at 48 kHz")
        if offset < 0 or offset + SAMPLES > audio.getnframes():
            raise ValueError(f"the selected section must contain {SAMPLES} frames")
        audio.setpos(offset)
        data = audio.readframes(SAMPLES)
    if len(data) != BYTES:
        raise ValueError("WAV data is truncated")
    return data


def demo():
    """Generate a repeatable E-minor pluck with a damped delay per string."""
    rng = random.Random(128)
    output = [0.0] * SAMPLES
    for i, frequency in enumerate((82.41, 110, 146.83, 196, 246.94, 329.63)):
        delay = round(RATE / frequency)
        string = [rng.uniform(-1, 1) for _ in range(delay)]
        for n in range(SAMPLES - i * 288):
            position = n % delay
            value = string[position]
            output[n + i * 288] += value
            string[position] = 0.497 * (value + string[(position + 1) % delay])
    scale = 25000 / max(map(abs, output))
    return struct.pack(f"<{SAMPLES}h", *(round(value * scale) for value in output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    extract = commands.add_parser("prepare")
    extract.add_argument("source", type=Path)
    extract.add_argument("output", type=Path)
    extract.add_argument("--offset", type=int, default=0, help="start frame at 48 kHz")
    for name in ("wrap", "demo"):
        command = commands.add_parser(name)
        if name == "wrap":
            command.add_argument("source", type=Path)
        command.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "prepare":
            args.output.write_bytes(prepare(args.source, args.offset))
        else:
            pcm = demo() if args.command == "demo" else args.source.read_bytes()
            if len(pcm) != BYTES:
                raise ValueError(f"expected exactly {BYTES} PCM bytes")
            write_wav(args.output, pcm)
    except (OSError, ValueError, wave.Error, EOFError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
