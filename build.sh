#!/bin/sh
# Builds the shim with Termux clang and installs it + an ICD manifest under ~/.local/share/turnip-kgsl-shim.
# Use it by pointing the Vulkan loader at the manifest:  VK_ICD_FILENAMES=~/.local/share/turnip-kgsl-shim/turnip_shim_icd.json
set -e
T=/data/data/com.termux/files/usr
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="${TURNIP_SHIM_PREFIX:-$HOME/.local/share/turnip-kgsl-shim}"
mkdir -p "$OUT"
"$T/bin/clang" -O2 -shared -fPIC -fvisibility=default -I"$T/include" "$HERE/src/shim.c" -ldl -o "$OUT/libvulkan_turnip_shim.so"
API=$(sed -n 's/.*"api_version": *"\([^"]*\)".*/\1/p' "$T/share/vulkan/icd.d/freedreno_icd.aarch64.json")
cat > "$OUT/turnip_shim_icd.json" <<JSON
{ "file_format_version": "1.0.1", "ICD": { "library_path": "$OUT/libvulkan_turnip_shim.so", "api_version": "${API:-1.4.0}" } }
JSON
echo "installed: $OUT/libvulkan_turnip_shim.so"
echo "manifest:  $OUT/turnip_shim_icd.json"
