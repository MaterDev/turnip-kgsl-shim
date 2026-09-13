# Getting WebGPU onto a phone GPU that nobody wanted to give me

*Draft. First person. The story of running WebGPU on the real Adreno 740 inside a Termux-built
Chromium on an Android handheld — no root, no rebuilt browser — and every wall I hit on the way.*

## The setup

I do my creative-coding work on an AYN Thor: a dual-screen Android handheld with a Snapdragon 8
Gen 2 and an Adreno 740. The bottom screen is a terminal, the top screen is a browser. My whole
toolchain lives in Termux, and inside a proot Ubuntu I run Claude Code as an unprivileged user —
no root, no sudo, ever.

I have a little gallery app, Canvas Lab, full of self-contained graphics pieces: Canvas2D, SVG,
WebGL, and — the one that kept mocking me — WebGPU. WebGL2 already ran hardware-accelerated at
60fps in my "Thor Viewer" (a full-screen mirror of the browser I can watch and take over). But
every WebGPU piece either fell back to software or, worse, **froze the whole device**.

I wanted WebGPU pieces to run on the actual GPU, visible in the viewer, stably. The device clearly
*had* a capable GPU — native Android Chrome runs WebGPU on it fine. The problem was getting there
from inside my weird bionic-Chromium-in-a-mirror stack.

## Dead end #1: the glibc / KGSL wall

The obvious move: run a native WebGPU renderer in my glibc Ubuntu (proot) — `npm i webgpu` ships a
prebuilt Dawn `dawn.node` for linux-arm64 — and stream frames into a canvas.

It could not reach the GPU. At all.

The reason turned out to be decisive and permanent: **this Adreno is KGSL-only.** The GPU is
`/dev/kgsl-3d0`. There's a `/dev/dri/renderD128` and a `card0`, but those are the *display
controller* (`msm_drm`/`mdss`), not a render node for the GPU. Ubuntu's stock glibc Mesa Turnip is
built with only the DRM/msm backend — no KGSL backend compiled in — so it enumerates zero GPUs:
"Failed to detect any valid GPUs." The prebuilt glibc `dawn.node` is in the same boat, and it's a
glibc binary that won't even load under bionic Termux node anyway.

Only **Termux's bionic Turnip** is built with the KGSL backend. So whatever I did had to live in
bionic, and realistically had to go *through Chromium*, which already used that Turnip for WebGL.

## Dead end #2: flags, headless, and Xvfb

So: in-browser it is. I threw the entire flag kitchen sink at Chromium —
`--enable-unsafe-webgpu`, `--use-vulkan=native`, `--use-webgpu-adapter=default`,
`--enable-features=Vulkan,WebGPU,...`, `--enable-dawn-features=allow_unsafe_apis,...`.

And `chrome://gpu` teased me: Dawn Info *did* enumerate
`<Integrated GPU> Vulkan backend - Turnip Adreno (TM) 740`. The adapter was right there. But
`navigator.gpu.requestAdapter()` in the page still handed me SwiftShader (the CPU fallback). The
line that explained it: **"WebGPU interop: Disabled"** under headless. GPU compositing was off, so
the renderer only ever got the software adapter.

The documented fix for headless servers is to run Chromium *headed* under Xvfb. I did. GPU
compositing flipped on (WebGL went to full "Hardware accelerated"). WebGPU interop stayed
**Disabled**. This is a real Chromium 149 integration gap for Dawn↔compositor shared images on
Turnip, not a flag I could flip.

## The one that scared me: the crash-loop

Somewhere in the flag exploration I paired the `Vulkan` base compositing feature with the default
surface under `--ozone-platform=headless`. The GPU process **crash-looped** — respawning, pegging
CPU and GPU — and the **device physically overheated**. That was the moment this stopped being a
puzzle and started being a hardware-safety problem.

Everything after that ran through a watchdog: an isolated `gputest` browser session that never
touches my live viewer, with a monitor that kills the browser if load or CPU run away. If you try
this, build the watchdog *first*.

## Turning the problem around

I stopped asking "how do I turn on interop" and started reading Chromium's source for *why the
page never gets the hardware adapter*. In `webgpu_decoder_impl.cc`, the decoder's `CanUseAdapter`
accepts a native Vulkan adapter only if `SupportsExternalImages()` — **or** if the adapter is
exactly SwiftShader (`deviceType == CPU`, `vendorID == 0x1AE0`, `deviceID == 0xC0DE`), in which
case Chromium presents canvases through a manual readback path.

Here's the twist. Termux's Chromium is a *Linux* build, but its third-party code — including Dawn —
is compiled with `__ANDROID__` defined. On an Android Dawn build, `SupportsExternalImages()`
specifically wants `VK_ANDROID_external_memory_android_hardware_buffer`, which Termux's Turnip
doesn't expose. (It *does* expose the desktop-Linux external-memory extensions a Linux Dawn build
would have accepted — but the Android build doesn't look for those.) So real Turnip fails the test,
and the only adapter Chromium will accept is one that *is* SwiftShader.

That reframed the whole thing: **the fastest way to get my real GPU accepted was to make it look
like SwiftShader.**

## The breakthrough: a shim, a spoof, and a blocked dlopen

### First I had to find the GPU at all

Before any of that, a subtler bug: wgpu-native saw *no* GPU on this device even though `vulkaninfo`
worked. I traced it to Turnip advertising `VK_KHR_display` and its display-WSI companions
unconditionally, even on a KGSL device with no DRM master. Enable any of them at instance creation
and `vkEnumeratePhysicalDevices` returns `VK_ERROR_INITIALIZATION_FAILED` — the GPU vanishes.
Libraries like wgpu-native "enable everything the driver advertises," so they trip it. (Minimal
repro in the repo; upstream Mesa report drafted.)

So I wrote **turnip-kgsl-shim**: a ~200-line pass-through Vulkan ICD that forwards every call to
the real Turnip driver but filters those display extensions out of the advertised list. Nobody
enables what they can't see. With the shim, wgpu-native rendered WGSL offscreen on the Adreno and
read back correct pixels — `GPU-RENDER-OK`. The GPU was reachable.

### Then I made it lie to exactly one caller

I taught the same shim an opt-in trick: `TURNIP_SHIM_SPOOF_CPU_FOR=Dawn`. It watches
`vkCreateInstance` for instances whose engine name is `"Dawn"` (Dawn sets this), and for *those
instances only* reports the real Turnip GPU with SwiftShader's vendor/device IDs and `deviceType =
CPU`. ANGLE, `vulkaninfo`, wgpu-native — anything not calling itself "Dawn" — sees the true Turnip,
so my hardware WebGL is completely untouched. Now Chromium's decoder accepted the adapter, and
`chrome://gpu` finally said **"WebGPU interop: Hardware accelerated."**

### Then it crashed, and I found out why

Accepting the adapter wasn't enough — creating a device **segfaulted the GPU process**. That was my
original "WebGPU freezes" bug all along. I couldn't attach a debugger (proot has no ptrace/gdb), so
I wrote an `LD_PRELOAD` crash-backtrace helper that interposes `sigaction`, keeps its own signal
handler first, and frame-walks with `dladdr()`. It pinned the fault to
`libvk_swiftshader.so+0xc03aec` (fault address `0x14`) inside device creation.

The culprit: Chromium's own *bundled* SwiftShader is registered as a Vulkan ICD too, and Dawn's
Vulkan backend probes the SwiftShader ICD **before** the system loader (`{ICD::SwiftShader,
ICD::None}`). With both the bundled SwiftShader and my disguised Turnip looking like "SwiftShader,"
Dawn grabbed the bundled one — and it crashes on this device.

Fix: a second tiny `LD_PRELOAD` library, `libno-dlopen.so`, that makes `dlopen()` fail for any path
containing `libvk_swiftshader`. With the bundled one unloadable, my disguised Turnip is the *only*
"SwiftShader" Dawn can find. (Detail that bit me: I had to scope the preload to the actual `chrome`
binary — the launcher is a shell script that calls glibc tools, and injecting a bionic `.so` into
those breaks them. The wrapper `exec`s `env LD_PRELOAD=... chrome ...` directly.)

### Then I had to actually see it

The adapter now ran WGSL on the GPU — but a `webgpu`-context canvas came out **blank** in the
viewer, because Chromium can't composite a WebGPU canvas from an adapter it thinks is SwiftShader
here. So the pieces don't use a `webgpu` canvas. They render offscreen into a `GPUTexture`, copy it
to a buffer, read the pixels back, and blit them into a plain `2d` canvas — which composites and
screencasts perfectly. `createPresenter()` in the app hides all of that behind `begin()` /
`present()`.

## Where it landed

- Adapter: **Turnip Adreno (TM) 740**, `maxTextureDimension2D = 16384`, `shader-f16` — hardware
  limits, not SwiftShader's.
- A 4x4 render+readback smoke test returns the exact expected pixel; a 20x Mandelbrot at 1024x1024
  runs in **76 ms** (~265 fps-equivalent — software would be single digits).
- Canvas presentation: **~140 fps in a raw render+readback loop at 768x432**, **60 fps fullscreen at 1280x633**.
- A multi-round WebGL↔WebGPU soak: **0 GPU crashes**, battery holding at 38.0 °C.

Three isolated, reversible runtime layers — an ICD shim, a device-identity spoof, and a blocked
`dlopen` — plus a readback presenter in the app. No patched Chromium, no patched Mesa, no root.
Unset three env vars and it's the stock browser again.

## Limitations & honest caveats

- **`adapter.isFallbackAdapter` is `true` to JavaScript.** By construction the adapter claims to be
  software, so any code that trusts that flag will wrongly think it's on a CPU rasterizer. My app
  works around it by *timing* a probe (real GPU: milliseconds; CPU: seconds) and only then trusting
  it — and falling back to an "open in real Chrome" card if the probe is slow, so a genuinely
  software adapter can never freeze the device.
- **One Dawn fast-path is disabled for "SwiftShader".** Because Dawn believes it's on SwiftShader,
  it turns off `VulkanUseExtendedDynamicState` (and possibly other adapter-specific fast paths).
  It's correct, just not maximally fast.
- **Readback isn't free.** Presenting through copy-to-buffer + map + blit costs real time and
  scales with resolution. It's comfortably above frame budget at these sizes, but it's the price of
  not having compositor interop.
- **It is a hack that rides on implementation details.** It depends on Dawn setting
  `pEngineName == "Dawn"`, on Chromium's decoder special-casing exactly those SwiftShader IDs, and
  on the bundled SwiftShader being a separately-loaded ICD. **A Chromium upgrade could break any of
  these** — treat the pinned Chromium as part of the setup, and re-verify after any update.
- **Tested on exactly one device/driver combo:** Adreno 740, Mesa 26.2.2 Turnip, Chromium 149,
  Android 13.

## What I'd do for native speed

The readback path is fine for watching and iterating in the viewer, but if I wanted true
native-speed WebGPU:

1. **Drive the device's real Android Chrome over adb + CDP.** Native Adreno WebGPU, native
   compositing, native speed — and Claude could still control it. The cost is wireless debugging
   setup (adb pairing over Wi-Fi), which is fragile on a handheld whose network changes.
2. **A glibc Turnip-KGSL build in proot + `dawn.node`.** There's prior art for glibc Mesa Turnip
   with a KGSL backend for Android containers (lfdevs/mesa-for-android-container,
   Grima04/mesa-turnip-kgsl). With that, the prebuilt glibc `dawn.node` could reach the GPU and I'd
   run a native WGSL render service — no browser identity games at all. It's a real build (hours,
   thermals), so I've deferred it.

For now, the shim-plus-readback approach gets real WebGPU pieces running on the real GPU, in the
viewer, safely — which was the whole point.

---

*Code: `MaterDev/turnip-kgsl-shim` (the shim, the tools, the tests) and Canvas Lab (the pieces and
the `gpu.js` presenter). Full mechanism in `docs/HOW-IT-WORKS.md`; step-by-step in
`docs/REPRODUCE.md`.*
