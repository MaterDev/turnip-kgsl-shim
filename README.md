# turnip-kgsl-shim

A pass-through Vulkan ICD (Installable Client Driver) for Termux on Android/bionic that works
around a Mesa Turnip bug on KGSL-only devices, plus an optional, experimental mode for getting
Chromium's WebGPU decoder to accept the real GPU.

## The bug

Mesa's Turnip driver advertises `VK_KHR_display` and its related instance extensions
(`VK_KHR_get_display_properties2`, `VK_EXT_direct_mode_display`, `VK_EXT_acquire_drm_display`,
`VK_EXT_acquire_xlib_display`, `VK_EXT_display_surface_counter`) unconditionally, even on devices
that only expose the GPU through KGSL (`/dev/kgsl-3d0`) and have no DRM render node / DRM master
for it. Display WSI needs a DRM master to initialize. On such a device, enabling any of these
extensions when creating a `VkInstance` makes `vkEnumeratePhysicalDevices` fail:

```
VK_ERROR_INITIALIZATION_FAILED (-3)
```

instead of returning the GPU. Apps that ask for exactly the extensions they need never trip this.
But libraries that enable *every* extension the driver advertises (wgpu-native does this) end up
enabling the broken display extensions and see zero physical devices — the GPU silently disappears.

**Repro:** `test/display-ext-repro.c`. It creates several `VkInstance`s with increasing subsets of
the display extension family and calls `vkEnumeratePhysicalDevices` on each:

```
surface+display: enumerate=-3 count=0
+display_props2: enumerate=-3 count=0
+direct_mode:    enumerate=-3 count=0
+acquire_drm:    enumerate=-3 count=0
full-wgpu-drm-set: enumerate=-3 count=0
```

Confirmed on Adreno 740, Mesa 26.2.2 (Turnip, `freedreno_icd.aarch64.json`), Android 13, Termux,
no root. See `docs/mesa-issue-draft.md` for the upstream report and a suggested fix direction.

## What the shim does

It's a real Vulkan ICD that Termux's Vulkan loader (`libvulkan.so`, LunarG loader) can load like
any other. Internally it just forwards every call to the real Turnip ICD (`libvulkan_freedreno.so`),
except:

- `vkEnumerateInstanceExtensionProperties` — filters the hidden extensions out of the list before
  returning it, so well-behaved callers that "enable everything advertised" never request the
  broken ones in the first place. This is the whole fix; the shim never has to know *why* an app
  asked for `VK_KHR_display` in a case where it does actually work — it just never gets offered it.
- Optionally, entry points involved in physical-device spoofing for one named engine (see below).

No other driver behavior changes. If you need to hide a different or additional extension, use
`TURNIP_SHIM_HIDE` rather than editing the default list, so the default stays documented and stable.

## Build / install / use / undo

Termux only (bionic, arm64, clang from `pkg install clang`). No root needed.

```sh
./build.sh
```

This compiles `src/shim.c` and installs the result to two places:

- **`$PREFIX/lib/libvulkan_turnip_shim.so`** (i.e. `/data/data/com.termux/files/usr/lib/`) — the
  ICD itself. It **must** live here, not under `$HOME`: bionic's dynamic linker restricts which
  directories the Vulkan loader is allowed to `dlopen()` a driver from (namespace/path
  restrictions on library loading), and `$HOME` is not a trusted location. `$PREFIX/lib` is where
  the real Turnip ICD (`libvulkan_freedreno.so`) already lives, so it's trusted.
- **`~/.local/share/turnip-kgsl-shim/turnip_shim_icd.json`** — the ICD manifest (Khronos loader
  JSON format) that points at the `.so` above and copies the API version out of Termux's real
  `freedreno_icd.aarch64.json` manifest. This one is just data, so it's fine under `$HOME`.

To override where the manifest/build artifacts go, set `TURNIP_SHIM_PREFIX` before running
`build.sh` (default `~/.local/share/turnip-kgsl-shim`); the `.so` install location is not
configurable, for the reason above.

**Use it** by pointing the Vulkan loader at the shim's manifest instead of letting it pick up the
system ICD automatically:

```sh
VK_ICD_FILENAMES=$HOME/.local/share/turnip-kgsl-shim/turnip_shim_icd.json <program>
```

Any Vulkan program works unmodified — `vulkaninfo`, `wgpu-native` test programs, Chromium, etc.

**Undo:** just don't set `VK_ICD_FILENAMES` (or unset it) — everything else falls back to the real
ICD directly, nothing about the system changed. To remove the files entirely:

```sh
rm -f $PREFIX/lib/libvulkan_turnip_shim.so
rm -rf ~/.local/share/turnip-kgsl-shim
```

## Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `TURNIP_SHIM_REAL` | `/data/data/com.termux/files/usr/lib/libvulkan_freedreno.so` | Path to the real ICD to wrap. |
| `TURNIP_SHIM_HIDE` | `VK_KHR_display,VK_KHR_get_display_properties2,VK_EXT_direct_mode_display,VK_EXT_acquire_drm_display,VK_EXT_acquire_xlib_display,VK_EXT_display_surface_counter` | Comma-separated instance extension names to hide from `vkEnumerateInstanceExtensionProperties`. |
| `TURNIP_SHIM_DEBUG` | unset | Set to `1` (or any non-`0` value) to log filtering/spoofing decisions to stderr. |
| `TURNIP_SHIM_SPOOF_CPU_FOR` | unset | Engine name (see below). Off by default. |
| `TURNIP_SHIM_PREFIX` (build.sh only) | `~/.local/share/turnip-kgsl-shim` | Where `build.sh` writes the manifest/build output. Does not affect the `.so` install path. |

## Optional: `TURNIP_SHIM_SPOOF_CPU_FOR` (experimental)

This mode exists for one specific, narrower problem than the display-extension bug above:
getting **Chromium's WebGPU decoder** to hand a page a hardware-backed adapter at all, on a device
where Chromium was built with `__ANDROID__` defined (Termux's Chromium is a Linux/ozone build but
compiles third-party code, including Dawn, as if it were Android).

Chromium's decoder (`CanUseAdapter` in `webgpu_decoder_impl.cc`) only accepts a native Vulkan
adapter if it supports importing external images
(`SupportsExternalImages()`), which on an `__ANDROID__` Dawn build specifically means
`VK_ANDROID_external_memory_android_hardware_buffer`. Termux's Turnip doesn't expose that
extension (it has the desktop-Linux external-memory extensions instead, which an Android-flavored
Dawn build doesn't check for). The only other adapter the decoder accepts unconditionally is one
that looks exactly like SwiftShader: `deviceType == CPU`, `vendorID == 0x1AE0`,
`deviceID == 0xC0DE`. Anything else is rejected and the page gets no GPU adapter.

With `TURNIP_SHIM_SPOOF_CPU_FOR=Dawn` set, the shim intercepts `vkCreateInstance` and remembers
which `VkInstance`s were created with `VkApplicationInfo.pEngineName == "Dawn"` (Dawn sets this).
For physical devices enumerated *from those instances only*, `vkGetPhysicalDeviceProperties[2]`
reports the real Turnip GPU but with SwiftShader's vendor/device IDs and `deviceType = CPU`.
Every other caller (ANGLE, `vulkaninfo`, wgpu-native, anything not identifying as engine `Dawn`)
sees the real, unmodified Turnip identity. Verify with `test/spoof-check.c`.

**This is experimental and a genuine hack**, not a clean fix:

- It relies on matching Dawn's current behavior of setting `pEngineName == "Dawn"`, and on
  Chromium's decoder continuing to special-case exactly those SwiftShader IDs — both are
  implementation details that can change across Chromium/Dawn versions.
- It does not by itself make WebGPU work end-to-end. As of this writing, Chromium's *bundled*
  SwiftShader (`libvk_swiftshader.so`) is also registered as a Vulkan ICD and also looks like
  "SwiftShader" to Dawn's ICD ordering (`{ICD::SwiftShader, ICD::None}`), so Dawn may pick the
  real bundled SwiftShader instead of the spoofed Turnip and crash (see
  `docs/segv-backtrace.md`), or may pick the spoofed Turnip — this needs the bundled SwiftShader
  ICD to be excluded/removed so the spoofed Turnip is the only "SwiftShader" Dawn can see.
- It's a device-identity spoof aimed at one consumer (Chromium/Dawn); it isn't a general-purpose
  Vulkan feature and could confuse any other app that happens to name its engine "Dawn".

Treat it as a stepping stone for the WebGPU-in-Chromium investigation, not something to depend on
for anything else. See `WEBGPU-BRIEF.md`-style investigation notes for the current status of that
effort (kept outside this repo).

## Limitations

- No thread safety: the spoof bookkeeping (`g_spoof_inst`, `g_spoof_pd`) uses plain fixed-size
  arrays (16 instances, 64 physical devices) with no locking. Fine for the single-instance,
  single-GPU case this was built for; a multi-threaded caller creating/destroying instances
  concurrently could race.
- Fixed capacity: more than 16 concurrently-live spoofed instances, or more than 64 physical
  devices enumerated from them, silently stop being tracked (no error, they just aren't spoofed).
  Not a concern on a one-GPU device.
- `logf_()` takes exactly one `%s`-style argument; it's not a real variadic logger. Fine for the
  current call sites, but don't add a differently-typed format specifier without updating it.
- The extension filter is a fixed list matched by exact name; it doesn't understand extension
  versions or dependencies.
- Only tested against Termux's Turnip (`freedreno_icd.aarch64.json`) on Adreno 740 / Mesa 26.2.2.

## How it works (ICD internals)

A Vulkan ICD is just a shared library the loader `dlopen()`s and talks to through a small,
fixed C interface described by a JSON manifest (`file_format_version`, `ICD.library_path`,
`ICD.api_version` — see `turnip_shim_icd.json` written by `build.sh`). The loader never touches
Vulkan calls directly against multiple drivers; every real ICD must implement:

- **`vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion)`** — the ICD interface version
  handshake. The shim forwards this to the real ICD's implementation (or clamps to version 4 if
  the real ICD is old enough not to export it).
- **`vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)`** — the loader's way of
  asking the ICD for every function pointer it needs, given either `NULL` (global/instance-creation
  functions) or a live `VkInstance` (instance and device functions). The shim intercepts exactly
  three names here — `vkEnumerateInstanceExtensionProperties` (filtering, see above),
  `vkGetInstanceProcAddr` (returns itself, so re-lookups keep going through the shim), and,
  only when spoofing is enabled, the handful of entry points needed for the CPU-device spoof
  (`vkCreateInstance`, `vkDestroyInstance`, `vkEnumeratePhysicalDevices`,
  `vkGetPhysicalDeviceProperties[2][KHR]`) — and forwards every other name straight to the real
  ICD's `vk_icdGetInstanceProcAddr`.
- **`vk_icdGetPhysicalDeviceProcAddr(VkInstance, const char *pName)`** — the loader's way of
  asking for device-level entry points resolved per-physical-device. Same pattern: spoof hook
  first, then forward.

Because the shim only intercepts a small, named set of entry points and passes every other call
straight through by function pointer, it behaves identically to the real driver for anything it
doesn't explicitly filter or spoof — that's what makes it safe to leave installed and select only
via `VK_ICD_FILENAMES` rather than needing to patch Mesa itself.
