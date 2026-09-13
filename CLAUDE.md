# turnip-kgsl-shim

Pass-through Vulkan ICD for Termux/Android (bionic) that hides `VK_KHR_display` and friends from Mesa Turnip on KGSL devices, where enabling them makes `vkEnumeratePhysicalDevices` fail with `VK_ERROR_INITIALIZATION_FAILED`. Makes wgpu-native (and anything else that enables every advertised extension) see the Adreno GPU.

- Build + install: `./build.sh` → `.so` to `$PREFIX/lib/libvulkan_turnip_shim.so` (bionic linker only dlopens ICDs from trusted paths) + manifest `~/.local/share/turnip-kgsl-shim/turnip_shim_icd.json` (Termux clang, no root).
- Use: `VK_ICD_FILENAMES=~/.local/share/turnip-kgsl-shim/turnip_shim_icd.json <program>`.
- Undo: unset `VK_ICD_FILENAMES` (real ICD used directly). Remove: `rm -f $PREFIX/lib/libvulkan_turnip_shim.so; rm -rf ~/.local/share/turnip-kgsl-shim`.
- Tests in `test/`: `display-ext-repro.c` (shows the Turnip bug), `wgpu-render.c` (offscreen render through wgpu-native, expects GPU-RENDER-OK). Compile with Termux clang as in build.sh.
- Keep it a shim: no driver behaviour changes beyond the extension filter unless documented in README.
- `tools/chromium-gpu` is the source of `~/.local/bin/chromium-gpu` (the Chromium wrapper the viewer's agent-browser config points at); `config/thor-gpu.env.example` is the template for `~/.config/thor-gpu/env`. Keep both in sync when editing the installed copies.
- Test scripts live in `~/.claude/skills/agent-browser/` (`gpu-probe.sh`, `gpu-piece-test.sh`, `gpu-soak.sh`); they run in the isolated `gputest` session with a watchdog.

## Docs & the full WebGPU chain
The shim is one of four layers that together get WebGPU onto the Adreno inside Termux Chromium: (1) this ICD hides `VK_KHR_display`; (2) `TURNIP_SHIM_SPOOF_CPU_FOR=Dawn` disguises the GPU as SwiftShader for Chromium's decoder; (3) `tools/no-dlopen.c` (LD_PRELOAD) blocks Chromium's crashy bundled `libvk_swiftshader.so`; (4) the app renders offscreen and blits to a 2D canvas. `tools/segv-backtrace.c` was the crash-triage tool. Full explanation in `docs/HOW-IT-WORKS.md`, reproduction in `docs/REPRODUCE.md`, narrative in `docs/BLOG-DRAFT.md`, upstream bug in `docs/mesa-issue-draft.md`.
