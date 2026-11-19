#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=${QUARTUS_ROOTDIR:-$HOME/intelFPGA_lite/22.1std/quartus}
vendor=${GHRD_DIR:-../cd/Demonstrations/SoC_FPGA/DE10_NANO_SoC_GHRD}
image=${1:-fir128}
case "$image" in
    fir128) build=$PWD/build; component=ip/fir128; overlay=fir128.dts ;;
    array_sum) build=$PWD/build/array_sum; component=../ghrd/ip/array_sum; overlay=../sw/array_sum.dts ;;
    *) echo 'Usage: build.sh [fir128|array_sum]' >&2; exit 2 ;;
esac
mkdir -p "$build/ip/$image"
cp "$vendor"/{DE10_NANO_SoC_GHRD.qpf,DE10_NANO_SoC_GHRD.qsf,DE10_NANO_SoC_GHRD.v,DE10_NANO_SOC_GHRD.sdc} "$build/"
cp -r "$vendor"/ip/{altsource_probe,debounce,edge_detect} "$build/ip/"
cp "$component"/* "$build/ip/$image/"
# Normalize the System CD's line endings before applying the 22.1 migration.
tr -d '\r' < "$vendor/soc_system.qsys" > "$build/soc_system.qsys"
patch --batch --forward --fuzz=0 "$build/soc_system.qsys" ghrd-22.1.patch
if [[ $image == fir128 ]]; then
    sed -i 's/array_sum/fir128/g' "$build/soc_system.qsys"
fi
sed -i 's/LAST_QUARTUS_VERSION 16.0.2/LAST_QUARTUS_VERSION "22.1std.0 Lite Edition"/' "$build/DE10_NANO_SoC_GHRD.qsf"
dtc -@ -I dts -O dtb -o "$build/$image.dtbo" "$overlay"
cd "$build"
"$root/sopc_builder/bin/qsys-generate" soc_system.qsys --synthesis=VERILOG
"$root/bin/quartus_sh" --flow compile DE10_NANO_SoC_GHRD
"$root/bin/quartus_cpf" -c -o bitstream_compression=on output_files/DE10_NANO_SoC_GHRD.sof "$image.rbf"
sha256sum "$image.rbf" "$image.dtbo" > firmware.sha256
cat firmware.sha256
