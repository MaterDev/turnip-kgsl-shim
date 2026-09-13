# turnip-kgsl-shim

Pass-through Vulkan ICD for Termux/Android (bionic) that hides `VK_KHR_display` and friends from Mesa Turnip on KGSL devices, where enabling them makes `vkEnumeratePhysicalDevices` fail with `VK_ERROR_INITIALIZATION_FAILED`. Makes wgpu-native (and anything else that enables every advertised extension) see the Adreno GPU.

- Build + install: `./build.sh` → `~/.local/share/turnip-kgsl-shim/{libvulkan_turnip_shim.so,turnip_shim_icd.json}` (Termux clang, no root).
- Use: `VK_ICD_FILENAMES=~/.local/share/turnip-kgsl-shim/turnip_shim_icd.json <program>`.
- Undo: unset that variable (Chromium/the viewer keep using the real ICD directly). Remove: `rm -rf ~/.local/share/turnip-kgsl-shim`.
- Tests in `test/`: `display-ext-repro.c` (shows the Turnip bug), `wgpu-render.c` (offscreen render through wgpu-native, expects GPU-RENDER-OK). Compile with Termux clang as in build.sh.
- Keep it a shim: no driver behaviour changes beyond the extension filter unless documented in README.
- `tools/chromium-gpu` is the source of `~/.local/bin/chromium-gpu` (the Chromium wrapper the viewer's agent-browser config points at); `config/thor-gpu.env.example` is the template for `~/.config/thor-gpu/env`. Keep both in sync when editing the installed copies.
- Test scripts live in `~/.claude/skills/agent-browser/` (`gpu-probe.sh`, `gpu-piece-test.sh`, `gpu-soak.sh`); they run in the isolated `gputest` session with a watchdog.
