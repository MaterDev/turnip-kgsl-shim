#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
static void probe(const char* label, const char** exts, uint32_t n){
  VkInstanceCreateInfo ici; memset(&ici,0,sizeof ici); ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.enabledExtensionCount=n; ici.ppEnabledExtensionNames=exts;
  VkInstance inst; VkResult r=vkCreateInstance(&ici,NULL,&inst); if(r){printf("%s: create=%d\n",label,r);return;}
  uint32_t c=0; r=vkEnumeratePhysicalDevices(inst,&c,NULL); printf("%s: enumerate=%d count=%u\n",label,r,c); vkDestroyInstance(inst,NULL); }
int main(){
  const char* a[]={"VK_KHR_surface","VK_KHR_display"}; probe("surface+display",a,2);
  const char* b[]={"VK_KHR_surface","VK_KHR_display","VK_KHR_get_display_properties2"}; probe("+display_props2",b,3);
  const char* c[]={"VK_KHR_surface","VK_KHR_display","VK_EXT_direct_mode_display"}; probe("+direct_mode",c,3);
  const char* d[]={"VK_KHR_surface","VK_KHR_display","VK_EXT_acquire_drm_display"}; probe("+acquire_drm",d,3);
  const char* e[]={"VK_KHR_surface","VK_KHR_display","VK_EXT_direct_mode_display","VK_EXT_acquire_drm_display","VK_KHR_get_display_properties2","VK_EXT_swapchain_colorspace","VK_KHR_get_physical_device_properties2"}; probe("full-wgpu-drm-set",e,7);
  return 0; }
