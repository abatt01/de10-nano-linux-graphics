#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
sha256sum -c SHA256SUMS
gcc -std=c11 -Wall -Wextra -Werror -O2 reference_test.c -o build/reference_test
build/reference_test
gcc -std=c11 -Wall -Wextra -Werror -O2 fir128_test.c -o build/fir128_test_host
if ! command -v iverilog >/dev/null || ! command -v vvp >/dev/null; then
    echo 'RTL simulation pending. Icarus Verilog (iverilog and vvp) is unavailable.' >&2
    exit 1
fi
python3 - <<'PY'
from pathlib import Path
from struct import iter_unpack
for name in ('mixed', 'expected'):
    pcm = Path(f'{name}.s16le').read_bytes()
    stem = 'input' if name == 'mixed' else name
    Path(f'build/{stem}.hex').write_text(''.join(f'{sample:04x}\n' for (sample,) in iter_unpack('<H', pcm)))
PY
iverilog -g2012 -s fir128_tb -o build/fir128_tb ip/fir128/fir128.v fir128_tb.sv
vvp build/fir128_tb
