#!/usr/bin/env python3
"""Generate the fixed clip, unified coefficient data and integer oracle."""
import hashlib
import math
from pathlib import Path
import struct

root = Path(__file__).resolve().parent
n = 16384
cutoff = 6000 / 48000
weights = []
for k in range(128):
    x = k - 63.5
    sinc = math.sin(2 * math.pi * cutoff * x) / (math.pi * x)
    weights.append(sinc * (0.54 - 0.46 * math.cos(2 * math.pi * k / 127)))
coeff = [round(w / sum(weights) * 32768) for w in weights]
samples = [
    round(14000 * math.sin(2 * math.pi * 1500 * i / 48000)
          + 11000 * math.sin(2 * math.pi * 12000 * i / 48000))
    for i in range(n)
]

def store_pcm(path, values):
    path.write_bytes(struct.pack(f"<{len(values)}h", *values))

def oracle(samples):
    padded = [0] * 127 + samples
    output = []
    for i in range(len(samples)):
        total = sum(c * x for c, x in zip(coeff, reversed(padded[i:i + 128])))
        rounded = ((abs(total) + 16384) // 32768) * (-1 if total < 0 else 1)
        output.append(max(-32768, min(32767, rounded)))
    return output

store_pcm(root / "mixed.s16le", samples)
(root / "coeff.inc").write_text("".join(f"{c},\n" for c in coeff))
(root / "ip/fir128/coeff.hex").write_text("".join(f"{c & 65535:04x}\n" for c in coeff))
store_pcm(root / "expected.s16le", oracle(samples))
for name in ("mixed.s16le", "coeff.inc", "ip/fir128/coeff.hex", "expected.s16le"):
    print(hashlib.sha256((root / name).read_bytes()).hexdigest(), "", name)
