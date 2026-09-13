/* turnip-kgsl-shim: a pass-through Vulkan ICD that hides selected instance extensions.
 *
 * Why: on Android/KGSL devices (Termux + Mesa Turnip), enabling VK_KHR_display (or its
 * companions) makes vkEnumeratePhysicalDevices fail with VK_ERROR_INITIALIZATION_FAILED,
 * because Turnip's display WSI needs a DRM master that KGSL devices don't have. Libraries
 * such as wgpu enable every display extension the driver advertises, so they see no GPU.
 * This shim forwards everything to the real ICD but filters the advertised extension list,
 * so well-behaved apps simply never request the broken ones.
 *
 * Env: TURNIP_SHIM_REAL  path to the real ICD .so (default: Termux libvulkan_freedreno.so)
 *      TURNIP_SHIM_HIDE  comma-separated extension names to hide (default: display family)
 *      TURNIP_SHIM_DEBUG =1 to log to stderr
 *      TURNIP_SHIM_SPOOF_CPU_FOR  engine name (VkApplicationInfo.pEngineName, e.g. "Dawn"). For Vulkan
 *                 instances created by that engine ONLY, report the GPU as a CPU device with
 *                 SwiftShader's vendor/device IDs (0x1AE0/0xC0DE). Chromium's WebGPU decoder accepts
 *                 such an adapter unconditionally and presents canvases through its manual
 *                 readback path, so WebGPU work runs on the real GPU even though Chromium (built
 *                 with Android defines) can't import external textures from this driver.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_REAL "/data/data/com.termux/files/usr/lib/libvulkan_freedreno.so"
#define DEFAULT_HIDE "VK_KHR_display,VK_KHR_get_display_properties2,VK_EXT_direct_mode_display," \
                     "VK_EXT_acquire_drm_display,VK_EXT_acquire_xlib_display,VK_EXT_display_surface_counter"

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_gpa)(VkInstance, const char *);

static void *g_real;
static PFN_gpa g_gipa, g_gpdpa;
static PFN_negotiate g_negotiate;
static char *g_hide;
static const char *g_spoof_engine;               /* NULL = off (set from TURNIP_SHIM_SPOOF_CPU_FOR) */
static int g_debug;

static void logf_(const char *fmt, const char *a) { if (g_debug) fprintf(stderr, "[turnip-shim] "); if (g_debug) fprintf(stderr, fmt, a); }

static int load_real(void) {
    if (g_real) return 1;
    const char *e = getenv("TURNIP_SHIM_DEBUG"); g_debug = e && *e && *e != '0';
    const char *path = getenv("TURNIP_SHIM_REAL"); if (!path || !*path) path = DEFAULT_REAL;
    const char *hide = getenv("TURNIP_SHIM_HIDE"); if (!hide) hide = DEFAULT_HIDE;
    g_hide = strdup(hide);
    const char *sp = getenv("TURNIP_SHIM_SPOOF_CPU_FOR"); if (sp && *sp) g_spoof_engine = strdup(sp);
    g_real = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_real) { fprintf(stderr, "[turnip-shim] dlopen %s failed: %s\n", path, dlerror()); return 0; }
    g_gipa = (PFN_gpa)dlsym(g_real, "vk_icdGetInstanceProcAddr");
    g_gpdpa = (PFN_gpa)dlsym(g_real, "vk_icdGetPhysicalDeviceProcAddr");
    g_negotiate = (PFN_negotiate)dlsym(g_real, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (!g_gipa) { fprintf(stderr, "[turnip-shim] real ICD lacks vk_icdGetInstanceProcAddr\n"); return 0; }
    logf_("loaded real ICD %s\n", path);
    return 1;
}

static int is_hidden(const char *name) {
    size_t n = strlen(name); const char *p = g_hide;
    while (p && *p) {
        const char *q = strchr(p, ','); size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len == n && !strncmp(p, name, n)) return 1;
        p = q ? q + 1 : NULL;
    }
    return 0;
}

static VkResult VKAPI_CALL shim_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *out) {
    PFN_vkEnumerateInstanceExtensionProperties f = (PFN_vkEnumerateInstanceExtensionProperties)g_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!f) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 0; VkResult r = f(layer, &n, NULL); if (r != VK_SUCCESS) return r;
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof *all);
    r = f(layer, &n, all); if (r != VK_SUCCESS && r != VK_INCOMPLETE) { free(all); return r; }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (is_hidden(all[i].extensionName)) { logf_("hiding %s\n", all[i].extensionName); continue; }
        all[kept++] = all[i];
    }
    if (!out) { *count = kept; free(all); return VK_SUCCESS; }
    uint32_t w = *count < kept ? *count : kept;
    memcpy(out, all, w * sizeof *out); *count = w; free(all);
    return w < kept ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- optional per-engine "pretend to be SwiftShader" spoof ---------------------------------- */
#define MAX_INST 16
#define MAX_PD 64

static VkInstance g_spoof_inst[MAX_INST];        /* instances created by the spoofed engine */
static VkPhysicalDevice g_spoof_pd[MAX_PD];      /* physical devices enumerated from them */
typedef VkResult (VKAPI_PTR *PFN_ci)(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
typedef void (VKAPI_PTR *PFN_di)(VkInstance, const VkAllocationCallbacks *);
typedef VkResult (VKAPI_PTR *PFN_epd)(VkInstance, uint32_t *, VkPhysicalDevice *);
typedef void (VKAPI_PTR *PFN_gpdp)(VkPhysicalDevice, VkPhysicalDeviceProperties *);
typedef void (VKAPI_PTR *PFN_gpdp2)(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
static PFN_ci g_real_ci; static PFN_di g_real_di; static PFN_epd g_real_epd; static PFN_gpdp g_real_gpdp; static PFN_gpdp2 g_real_gpdp2, g_real_gpdp2khr;

static int inst_spoofed(VkInstance i){ for(int k=0;k<MAX_INST;k++) if(g_spoof_inst[k]==i) return 1; return 0; }
static int pd_spoofed(VkPhysicalDevice p){ for(int k=0;k<MAX_PD;k++) if(g_spoof_pd[k]==p) return 1; return 0; }
static void spoof_props(VkPhysicalDeviceProperties *p){
    p->vendorID = 0x1AE0; p->deviceID = 0xC0DE; p->deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    logf_("spoofing %s as SwiftShader/CPU\n", p->deviceName);
}
static VkResult VKAPI_CALL shim_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out){
    VkResult r = g_real_ci(ci, a, out);
    if (r == VK_SUCCESS && g_spoof_engine && ci->pApplicationInfo && ci->pApplicationInfo->pEngineName &&
        !strcmp(ci->pApplicationInfo->pEngineName, g_spoof_engine)) {
        for (int k=0;k<MAX_INST;k++) if(!g_spoof_inst[k]){ g_spoof_inst[k]=*out; break; }
        logf_("instance from engine %s will be spoofed\n", g_spoof_engine);
    }
    return r;
}
static void VKAPI_CALL shim_DestroyInstance(VkInstance i, const VkAllocationCallbacks *a){
    for (int k=0;k<MAX_INST;k++) if(g_spoof_inst[k]==i) g_spoof_inst[k]=NULL;
    g_real_di(i, a);
}
static VkResult VKAPI_CALL shim_EnumeratePhysicalDevices(VkInstance i, uint32_t *n, VkPhysicalDevice *pds){
    VkResult r = g_real_epd(i, n, pds);
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && pds && inst_spoofed(i))
        for (uint32_t x=0; x<*n; x++) { if (pd_spoofed(pds[x])) continue; for (int k=0;k<MAX_PD;k++) if(!g_spoof_pd[k]){ g_spoof_pd[k]=pds[x]; break; } }
    return r;
}
static void VKAPI_CALL shim_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p){
    g_real_gpdp(pd, p); if (pd_spoofed(pd)) spoof_props(p);
}
static void VKAPI_CALL shim_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p){
    g_real_gpdp2(pd, p); if (pd_spoofed(pd)) spoof_props(&p->properties);
}
static void VKAPI_CALL shim_GetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p){
    g_real_gpdp2khr(pd, p); if (pd_spoofed(pd)) spoof_props(&p->properties);
}
/* returns a wrapper for the spoof-relevant entry points, caching the real pointer; NULL otherwise */
static PFN_vkVoidFunction spoof_hook(VkInstance inst, const char *name){
    if (!g_spoof_engine) return NULL;
    PFN_vkVoidFunction real = g_gipa(inst, name); if (!real) return NULL;
    if (!strcmp(name,"vkCreateInstance"))                 { g_real_ci=(PFN_ci)real;        return (PFN_vkVoidFunction)shim_CreateInstance; }
    if (!strcmp(name,"vkDestroyInstance"))                { g_real_di=(PFN_di)real;        return (PFN_vkVoidFunction)shim_DestroyInstance; }
    if (!strcmp(name,"vkEnumeratePhysicalDevices"))       { g_real_epd=(PFN_epd)real;      return (PFN_vkVoidFunction)shim_EnumeratePhysicalDevices; }
    if (!strcmp(name,"vkGetPhysicalDeviceProperties"))    { g_real_gpdp=(PFN_gpdp)real;    return (PFN_vkVoidFunction)shim_GetPhysicalDeviceProperties; }
    if (!strcmp(name,"vkGetPhysicalDeviceProperties2"))   { g_real_gpdp2=(PFN_gpdp2)real;  return (PFN_vkVoidFunction)shim_GetPhysicalDeviceProperties2; }
    if (!strcmp(name,"vkGetPhysicalDeviceProperties2KHR")){ g_real_gpdp2khr=(PFN_gpdp2)real; return (PFN_vkVoidFunction)shim_GetPhysicalDeviceProperties2KHR; }
    return NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *v) {
    if (!load_real()) return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (g_negotiate) return g_negotiate(v);
    if (*v > 4) *v = 4; return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance inst, const char *name) {
    if (!load_real() || !name) return NULL;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)shim_EnumerateInstanceExtensionProperties;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    { PFN_vkVoidFunction h = spoof_hook(inst, name); if (h) return h; }
    return g_gipa(inst, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance inst, const char *name) {
    if (!load_real() || !g_gpdpa) return NULL;
    { PFN_vkVoidFunction h = spoof_hook(inst, name); if (h) return h; }
    return g_gpdpa(inst, name);
}
/* legacy exports for very old loader interface versions */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i, const char *n) { return vk_icdGetInstanceProcAddr(i, n); }
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *l, uint32_t *c, VkExtensionProperties *p) { if (!load_real()) return VK_ERROR_INITIALIZATION_FAILED; return shim_EnumerateInstanceExtensionProperties(l, c, p); }
