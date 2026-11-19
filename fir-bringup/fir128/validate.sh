#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ $# != 0 && $# != 2 ]]; then
    echo 'Usage: validate.sh [input.s16le expected.s16le]' >&2
    exit 2
fi
mkdir -p build
sha256sum -c SHA256SUMS
gcc -std=c11 -Wall -Wextra -Werror -O2 reference_test.c -o build/reference_test
build/reference_test
gcc -std=c11 -Wall -Wextra -Werror -O2 fir128_test.c -o build/fir128_test_host
if ! command -v iverilog >/dev/null || ! command -v vvp >/dev/null; then
    echo 'RTL simulation pending. Icarus Verilog (iverilog and vvp) is unavailable.' >&2
    exit 1
fi
python3 - "${1:-mixed.s16le}" "${2:-expected.s16le}" <<'PY'
from pathlib import Path
from struct import iter_unpack
from sys import argv
for stem, filename in zip(('input', 'expected'), argv[1:]):
    pcm = Path(filename).read_bytes()
    if len(pcm) != 32768:
        raise SystemExit(f'{filename}: expected exactly 32768 bytes')
    Path(f'build/{stem}.hex').write_text(''.join(f'{sample:04x}\n' for (sample,) in iter_unpack('<H', pcm)))
PY
iverilog -g2012 -s fir128_tb -o build/fir128_tb ip/fir128/fir128.v fir128_tb.sv
vvp build/fir128_tb
