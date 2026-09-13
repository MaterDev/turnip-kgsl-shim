# Draft upstream report: Turnip/KGSL — enabling display WSI extensions breaks `vkEnumeratePhysicalDevices`

Target: Mesa GitLab (`https://gitlab.freedesktop.org/mesa/mesa`), component Turnip (`src/freedreno/vulkan`).

---

## Summary

On a KGSL-only Adreno device (no DRM render node for the GPU, no DRM master available), enabling
`VK_KHR_display` — or any of its related instance extensions — when creating a `VkInstance` makes
`vkEnumeratePhysicalDevices()` return `VK_ERROR_INITIALIZATION_FAILED` instead of the GPU that is
otherwise enumerated fine. Turnip advertises these extensions unconditionally via
`vkEnumerateInstanceExtensionProperties`, even though it apparently cannot support them without a
DRM master. Any application or library that enables every extension the driver advertises (a
common and reasonable pattern — e.g. `wgpu-native`'s `InstanceExtras`/default extension handling)
will silently lose access to the GPU.

## Environment

- Device: Adreno 740 (Snapdragon 8 Gen 2), arm64
- OS: Android 13, no root
- Driver: Mesa 26.2.2, Turnip, installed via Termux package `mesa-vulkan-icd-freedreno`
- ICD manifest: `freedreno_icd.aarch64.json`, `library_path` → `libvulkan_freedreno.so`
- Loader: LunarG Vulkan loader (`libvulkan.so`, bundled with Termux)
- GPU access path: `/dev/kgsl-3d0` only. `/dev/dri/renderD128` exists on this device but is the
  display controller (`msm_drm`/`mdss`), not a render node for this GPU — there is no DRM render
  node or DRM master available for the GPU at all.
- `vulkaninfo` with no extra extensions enabled works fine and reports "Turnip Adreno (TM) 740",
  Vulkan 1.4 — this is specifically about enabling display WSI extensions.

## Repro

```c
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
static void probe(const char* label, const char** exts, uint32_t n){
  VkInstanceCreateInfo ici; memset(&ici,0,sizeof ici);
  ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.enabledExtensionCount=n; ici.ppEnabledExtensionNames=exts;
  VkInstance inst; VkResult r=vkCreateInstance(&ici,NULL,&inst);
  if(r){printf("%s: create=%d\n",label,r);return;}
  uint32_t c=0; r=vkEnumeratePhysicalDevices(inst,&c,NULL);
  printf("%s: enumerate=%d count=%u\n",label,r,c);
  vkDestroyInstance(inst,NULL);
}
int main(){
  const char* a[]={"VK_KHR_surface","VK_KHR_display"};
  probe("surface+display",a,2);
  const char* b[]={"VK_KHR_surface","VK_KHR_display","VK_KHR_get_display_properties2"};
  probe("+display_props2",b,3);
  const char* c[]={"VK_KHR_surface","VK_KHR_display","VK_EXT_direct_mode_display"};
  probe("+direct_mode",c,3);
  const char* d[]={"VK_KHR_surface","VK_KHR_display","VK_EXT_acquire_drm_display"};
  probe("+acquire_drm",d,3);
  const char* e[]={"VK_KHR_surface","VK_KHR_display","VK_EXT_direct_mode_display",
                   "VK_EXT_acquire_drm_display","VK_KHR_get_display_properties2",
                   "VK_EXT_swapchain_colorspace","VK_KHR_get_physical_device_properties2"};
  probe("full-wgpu-drm-set",e,7);
  return 0;
}
```

(Full source: `test/display-ext-repro.c` in `MaterDev/turnip-kgsl-shim`.)

Compile against the Termux Vulkan headers and link `-lvulkan`; run with no `VK_ICD_FILENAMES`
override so the loader picks up the system `freedreno_icd.aarch64.json`.

## Expected

`vkEnumeratePhysicalDevices` returns `VK_SUCCESS` and a count of 1 (the Adreno 740) in every case,
the same as when no display extensions are requested — enabling an instance extension that an
application never otherwise uses should not change enumeration results, and if the driver
genuinely cannot support display WSI in this configuration it should either not advertise it, or
fail only display-specific calls, not GPU enumeration itself.

## Actual

Every instance created with `VK_KHR_display` or a related extension enabled fails at
`vkEnumeratePhysicalDevices` with `VK_ERROR_INITIALIZATION_FAILED` (-3), count 0:

```
surface+display: enumerate=-3 count=0
+display_props2: enumerate=-3 count=0
+direct_mode:    enumerate=-3 count=0
+acquire_drm:    enumerate=-3 count=0
full-wgpu-drm-set: enumerate=-3 count=0
```

An instance created with neither `VK_KHR_display` nor its companions enabled enumerates the GPU
normally.

## Impact

`vkEnumerateInstanceExtensionProperties` still advertises these extensions as supported, so any
Vulkan consumer that follows the common "enable everything the driver reports" pattern (rather
than requesting only extensions it specifically needs) will enable one of them and then see zero
physical devices, with no indication of why. Observed in practice with `wgpu-native`'s Vulkan
backend, whose instance-extension setup does this by default — WebGPU applications built on it
report no adapter at all on this device, even though the GPU is otherwise fully usable through
Turnip (confirmed with a plain offscreen render/readback test that enables no display extensions).

## Suspected cause

Turnip's display WSI support likely assumes a DRM render node / DRM master is available to back
`VkDisplayKHR` objects (`VK_EXT_acquire_drm_display` deals with DRM master acquisition directly).
On this device there is no DRM node for the GPU (`kgsl` only), so instance-level initialization
tied to the display extensions appears to fail, and that failure seems to propagate to
`vkEnumeratePhysicalDevices` for the whole instance rather than being scoped to display-specific
entry points.

## Suggested fix directions

1. Don't advertise `VK_KHR_display`/companions from `vkEnumerateInstanceExtensionProperties` when
   no suitable DRM device/master is available for the GPU at all (KGSL-only configuration) — mirror
   however Turnip already decides it can't do window-system integration.
2. Alternatively, make display-extension initialization fail non-fatally and lazily: let
   `vkCreateInstance` succeed and `vkEnumeratePhysicalDevices` continue to enumerate the GPU
   normally, and only surface `VK_ERROR_INITIALIZATION_FAILED` from the display-specific entry
   points (`vkGetPhysicalDeviceDisplayPropertiesKHR`, etc.) when they're actually called.

Either would let extension-agnostic consumers (wgpu-native and similar) work without needing to
special-case KGSL devices themselves.

## Workaround in use

A userspace pass-through Vulkan ICD that filters `VK_KHR_display` and its companions out of
`vkEnumerateInstanceExtensionProperties` before the loader/application ever sees them:
`https://github.com/MaterDev/turnip-kgsl-shim`. Confirms the diagnosis: with the extensions hidden,
the same application enumerates and uses the GPU normally.

## Attachments to include when filing

- `vulkaninfo` output (baseline, extensions not enabled)
- Output of the repro program above
- `freedreno_icd.aarch64.json` (Mesa version / driver identification)
- `dmesg`/`logcat` around a failing `vkCreateInstance`+`vkEnumeratePhysicalDevices` call, if any
  kernel-side messages appear (none were observed in ad hoc testing so far)
