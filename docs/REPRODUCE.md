# Reproduce / teaching guide: WebGPU on a KGSL-only Adreno in Termux Chromium

A numbered, copy-pasteable walkthrough of the whole result, built so each layer can be verified on
its own before the next is stacked on top. Read `HOW-IT-WORKS.md` first for *why* each step exists.

> **Safety.** One earlier experiment crash-looped the GPU process and overheated the device. Every
> browser step here runs through the watchdog-protected harness scripts in an **isolated**
> `gputest` session — never the user's live `thor` session. Do not pair Chromium's `Vulkan` base
> feature with the default surface under `--ozone-platform=headless`; that is what overheated the
> device. Keep an eye on `cat /sys/class/power_supply/battery/temp`.

---

## 0. Prerequisites

**Device class this applies to:** a KGSL-only Adreno (GPU reached via `/dev/kgsl-3d0`, with
`/dev/dri/renderD128` being the *display* controller, not a GPU render node), running Android +
Termux, no root. Confirmed on Adreno 740 / Snapdragon 8 Gen 2 / Android 13.

**Confirm the GPU access facts first:**

```sh
ls -l /dev/kgsl-3d0                 # should exist (the GPU)
cat /sys/class/drm/card0/device/uevent 2>/dev/null | grep -i driver   # msm_drm/mdss = display only
```

**Termux packages (bionic userland):**

```sh
pkg install clang mesa-vulkan-icd-freedreno vulkan-tools x11-repo chromium
vulkaninfo --summary | grep -i 'deviceName\|apiVersion'   # expect "Turnip Adreno (TM) 740", Vulkan 1.4
```

- Chromium here is **149.0.7827.155** (a Linux/ozone build, but its Dawn is compiled `__ANDROID__`).
- Mesa Turnip is **26.2.2**; ICD manifest `$PREFIX/share/vulkan/icd.d/freedreno_icd.aarch64.json`,
  `library_path` → `libvulkan_freedreno.so`.

**wgpu-native (for the Layer 1 native test only):** download the bionic release asset
`wgpu-android-aarch64-release.zip` from gfx-rs/wgpu-native (v29 used here) and unzip it to
`~/.local/share/wgpu-native` (headers under `include/`, `libwgpu_native.a`/`.so`).

**This repo:** clone `MaterDev/turnip-kgsl-shim` into your workspace.

---

## 1. Build and install the shim (Layer 1 + Layer 2 live in one ICD)

```sh
cd turnip-kgsl-shim
./build.sh
```

This compiles `src/shim.c` with Termux clang and installs:

- `$PREFIX/lib/libvulkan_turnip_shim.so` — the ICD. **Must** be in `$PREFIX/lib` (bionic's linker
  only lets the Vulkan loader `dlopen` a driver from a trusted path; `$HOME` is not one).
- `~/.local/share/turnip-kgsl-shim/turnip_shim_icd.json` — the manifest (points at the `.so`,
  copies `api_version` from Termux's real freedreno manifest).

---

## 2. Verify the Turnip/KGSL bug and that Layer 1 fixes it

**Show the bug** (enabling `VK_KHR_display` breaks enumeration):

```sh
$PREFIX/bin/clang -O2 test/display-ext-repro.c -lvulkan -o /tmp/repro
/tmp/repro
# WITHOUT the shim you get:  ...: enumerate=-3 count=0  on every line
```

**Show the shim hides the extensions** (run the same binary through the shim ICD):

```sh
VK_ICD_FILENAMES=$HOME/.local/share/turnip-kgsl-shim/turnip_shim_icd.json vulkaninfo --summary
# still reports Turnip Adreno (TM) 740 — driver behaviour otherwise unchanged
```

**Prove native WebGPU renders on the GPU through the shim** (Layer 1, no browser):

```sh
W=$HOME/.local/share/wgpu-native
$PREFIX/bin/clang -O2 -I"$W/include" test/wgpu-render.c "$W"/libwgpu_native.* -o /tmp/wgpu-render
VK_ICD_FILENAMES=$HOME/.local/share/turnip-kgsl-shim/turnip_shim_icd.json /tmp/wgpu-render
# expect: adapter: ... , then "RESULT: GPU-RENDER-OK"
```

---

## 3. Verify the Dawn spoof is selective (Layer 2)

```sh
$PREFIX/bin/clang -O2 test/spoof-check.c -lvulkan -o /tmp/spoof
VK_ICD_FILENAMES=$HOME/.local/share/turnip-kgsl-shim/turnip_shim_icd.json \
  TURNIP_SHIM_SPOOF_CPU_FOR=Dawn /tmp/spoof
# engine=Dawn   -> vendor=0x1ae0 device=0xc0de type=4 (CPU)          <- spoofed
# engine=ANGLE  -> real Turnip vendor/device, type=1 (INTEGRATED_GPU) <- unchanged
```

Only instances whose `pEngineName == "Dawn"` are disguised; everything else (ANGLE/WebGL,
`vulkaninfo`, wgpu-native) sees the true Turnip identity. Set `TURNIP_SHIM_DEBUG=1` to log each
filter/spoof decision to stderr.

---

## 4. Build the dlopen blocker (Layer 3)

```sh
$PREFIX/bin/clang -O1 -shared -fPIC tools/no-dlopen.c -ldl -o $PREFIX/lib/libno-dlopen.so
```

(Optional diagnostic — the crash-backtrace helper used to find the SwiftShader segfault:)

```sh
$PREFIX/bin/clang -O2 -shared -fPIC -fvisibility=default tools/segv-backtrace.c -ldl \
  -o $PREFIX/lib/libsegv-backtrace.so
```

---

## 5. Install the Chromium wrapper and the env switch (Layers 1–3 wired together)

The wrapper `~/.local/bin/chromium-gpu` is the source-controlled `tools/chromium-gpu`; the env
template is `config/thor-gpu.env.example` (a.k.a. `env.webgpu`). Install both:

```sh
install -m755 tools/chromium-gpu ~/.local/bin/chromium-gpu
mkdir -p ~/.config/thor-gpu
cp config/thor-gpu.env.example ~/.config/thor-gpu/env
```

The env file turns everything on:

```sh
THOR_SHIM=1                                          # route Vulkan through the shim + spoof Dawn
THOR_WEBGPU=1                                        # enable Chromium WebGPU flags
THOR_PRELOAD=/data/data/com.termux/files/usr/lib/libno-dlopen.so
NO_DLOPEN_MATCH=libvk_swiftshader                    # keep the crashy bundled SwiftShader out
```

When `THOR_SHIM=1`, the wrapper sets `VK_ICD_FILENAMES` to the shim manifest and
`TURNIP_SHIM_SPOOF_CPU_FOR=Dawn`, and `exec`s the real `chrome` binary with `LD_PRELOAD` scoped to
that one process (never the glibc helper tools the launcher script calls). WebGPU is only enabled
when the shim is active — otherwise Chromium's own SwiftShader WebGPU (which crashes here) stays
off.

---

## 6. Verify the whole chain in the browser (isolated session)

The harness scripts live in `~/.claude/skills/agent-browser/`. They run the `gputest` session
through `chromium-gpu`, time-box every call, and a watchdog kills the browser if load/CPU run away.
Start Canvas Lab first (`cd canvas-lab && node server.mjs` → `http://127.0.0.1:4860/`).

**a. Adapter + smoke + perf + `chrome://gpu` dump** — `gpu-probe.sh`:

```sh
~/.claude/skills/agent-browser/gpu-probe.sh webgpu-shim
cat ~/.cache/gpu-probe/webgpu-shim.log
```

Expect in the log:
- `adapter(default):` a JSON blob with `maxTex:16384`, `f16:true` (hardware limits; note
  `fallback:true` is *expected* — see caveats).
- `smoke: pixel=255,0,128,255 (want 255,0,128,255)` — exact.
- `perf: 20x mandelbrot 1024x1024 in ~76ms (~265 fps-equivalent; ... GPU >100)`.
- `chrome://gpu`: "WebGPU interop: Hardware accelerated", Dawn Info listing the Turnip adapter, 0
  crashes.

**b. A real piece renders and is visible** — `gpu-piece-test.sh`:

```sh
~/.claude/skills/agent-browser/gpu-piece-test.sh "WebGPU Gradient" 10
```

Expect the piece's console lines (`[webgpu-gradient] ready ...`, `~N fps @ WxH`), a screenshot in
`~/.cache/gpu-probe/`, `fallback card? no (stage active)`, `gpu crashes: 0`.

**c. Stability / thermals under switching** — `gpu-soak.sh`:

```sh
~/.claude/skills/agent-browser/gpu-soak.sh "hello-liquid" "WebGPU Gradient" 6 5
```

Expect `gpu crashes: 0`, temperature holding (shipped run: battery 38.0 °C), no error spam.

---

## 7. Switch the user's live viewer over (optional, last)

Only after the isolated checks pass. Point agent-browser at the wrapper and keep a backup of the
known-good WebGL-only config:

```sh
cp ~/.agent-browser/config.json ~/.agent-browser/config.json.webgl-only
# set "executablePath" in ~/.agent-browser/config.json to ~/.local/bin/chromium-gpu
```

The env at `~/.config/thor-gpu/env` is what makes the wrapper do the WebGPU thing.

---

## Undo everything (fully reversible, nothing system-wide changed)

```sh
# 1. Turn WebGPU back off: revert the viewer to the plain known-good Chromium
rm -f ~/.config/thor-gpu/env                     # wrapper falls back to plain Turnip WebGL-only
cp ~/.agent-browser/config.json.webgl-only ~/.agent-browser/config.json   # if you switched it

# 2. Remove the shim (real ICD is used directly again)
rm -f $PREFIX/lib/libvulkan_turnip_shim.so
rm -rf ~/.local/share/turnip-kgsl-shim

# 3. Remove the preload helpers
rm -f $PREFIX/lib/libno-dlopen.so $PREFIX/lib/libsegv-backtrace.so

# 4. (optional) remove the wrapper
rm -f ~/.local/bin/chromium-gpu
```

None of this patches Mesa, Chromium, Dawn, or SwiftShader on disk; it is all selected at runtime
via `VK_ICD_FILENAMES`, `LD_PRELOAD`, and env vars. Unsetting them restores the stock behaviour.

## Troubleshooting

- **`vkEnumeratePhysicalDevices` still fails (-3):** you are not going through the shim manifest;
  check `VK_ICD_FILENAMES`.
- **Page still gets SwiftShader:** `TURNIP_SHIM_SPOOF_CPU_FOR=Dawn` not set, or Chromium/Dawn
  changed the `pEngineName`/ID special-case (run `spoof-check.c`; check `chrome://gpu` Dawn Info).
- **GPU process crashes on device creation:** the bundled `libvk_swiftshader.so` is still loading;
  confirm `THOR_PRELOAD`/`NO_DLOPEN_MATCH` and that the wrapper `exec`s the `chrome` binary
  directly (grep the GPU log for `no-dlopen. denied`).
- **Canvas blank but compute works:** the piece is drawing to a `webgpu` context instead of using
  `createPresenter` (Layer 4).
