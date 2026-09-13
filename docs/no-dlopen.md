# no-dlopen (LD_PRELOAD helper)

`tools/no-dlopen.c` builds `libno-dlopen.so`, a tiny bionic `LD_PRELOAD` library that makes `dlopen()` fail for any library path containing one of the substrings in `NO_DLOPEN_MATCH` (comma-separated), and makes the following `dlerror()` return a real message (callers such as Dawn's `DynamicLib` build a `std::string` from it; a NULL there crashes).

Why it exists: Termux Chromium's bundled SwiftShader (`libvk_swiftshader.so`) segfaults on the AYN Thor whenever a WebGPU device is created on it, and Dawn probes that ICD before the system Vulkan loader. With the shim presenting the Adreno GPU to Dawn as "SwiftShader", the bundled one must be kept out of the process so the real GPU is the only candidate.

Use (only for the chrome binary, never for glibc tools — see `docs/segv-backtrace.md`):

```
THOR_PRELOAD=$PREFIX/lib/libno-dlopen.so NO_DLOPEN_MATCH=libvk_swiftshader NO_DLOPEN_DEBUG=1
```

`~/.local/bin/chromium-gpu` applies this when `THOR_SHIM=1` is set in `~/.config/thor-gpu/env`. Build: `clang -O1 -shared -fPIC tools/no-dlopen.c -ldl -o $PREFIX/lib/libno-dlopen.so`.
