#!/bin/sh
# Builds the shim with Termux clang. The .so is installed into Termux's lib dir (the bionic linker
# only lets the Vulkan loader dlopen ICDs from trusted paths; $HOME is not one), the manifest goes
# under ~/.local/share/turnip-kgsl-shim.
#   use:  VK_ICD_FILENAMES=$HOME/.local/share/turnip-kgsl-shim/turnip_shim_icd.json <program>
#   undo: rm $PREFIX/lib/libvulkan_turnip_shim.so ~/.local/share/turnip-kgsl-shim -r
set -e
T=/data/data/com.termux/files/usr
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="${TURNIP_SHIM_PREFIX:-$HOME/.local/share/turnip-kgsl-shim}"
LIB="$T/lib/libvulkan_turnip_shim.so"
mkdir -p "$OUT"
"$T/bin/clang" -O2 -shared -fPIC -fvisibility=default -I"$T/include" "$HERE/src/shim.c" -ldl -o "$LIB"
API=$(sed -n 's/.*"api_version": *"\([^"]*\)".*/\1/p' "$T/share/vulkan/icd.d/freedreno_icd.aarch64.json")
cat > "$OUT/turnip_shim_icd.json" <<JSON
{ "file_format_version": "1.0.1", "ICD": { "library_path": "$LIB", "api_version": "${API:-1.4.0}" } }
JSON
rm -f "$OUT/libvulkan_turnip_shim.so"
echo "installed: $LIB"
echo "manifest:  $OUT/turnip_shim_icd.json"
