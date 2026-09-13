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
static int g_debug;

static void logf_(const char *fmt, const char *a) { if (g_debug) fprintf(stderr, "[turnip-shim] "); if (g_debug) fprintf(stderr, fmt, a); }

static int load_real(void) {
    if (g_real) return 1;
    const char *e = getenv("TURNIP_SHIM_DEBUG"); g_debug = e && *e && *e != '0';
    const char *path = getenv("TURNIP_SHIM_REAL"); if (!path || !*path) path = DEFAULT_REAL;
    const char *hide = getenv("TURNIP_SHIM_HIDE"); if (!hide) hide = DEFAULT_HIDE;
    g_hide = strdup(hide);
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

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *v) {
    if (!load_real()) return VK_ERROR_INCOMPATIBLE_DRIVER;
    if (g_negotiate) return g_negotiate(v);
    if (*v > 4) *v = 4; return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance inst, const char *name) {
    if (!load_real() || !name) return NULL;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)shim_EnumerateInstanceExtensionProperties;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vk_icdGetInstanceProcAddr;
    return g_gipa(inst, name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance inst, const char *name) {
    if (!load_real() || !g_gpdpa) return NULL; return g_gpdpa(inst, name);
}
/* legacy exports for very old loader interface versions */
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i, const char *n) { return vk_icdGetInstanceProcAddr(i, n); }
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *l, uint32_t *c, VkExtensionProperties *p) { if (!load_real()) return VK_ERROR_INITIALIZATION_FAILED; return shim_EnumerateInstanceExtensionProperties(l, c, p); }
