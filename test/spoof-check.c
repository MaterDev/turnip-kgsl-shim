#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
static void go(const char* engine){
  VkApplicationInfo ai; memset(&ai,0,sizeof ai); ai.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.pEngineName=engine; ai.apiVersion=VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci; memset(&ci,0,sizeof ci); ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ci.pApplicationInfo=&ai;
  VkInstance in; if(vkCreateInstance(&ci,NULL,&in)){printf("%s: create failed\n",engine);return;}
  uint32_t n=1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(in,&n,&pd);
  VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd,&p);
  VkPhysicalDeviceProperties2 p2; memset(&p2,0,sizeof p2); p2.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; vkGetPhysicalDeviceProperties2(pd,&p2);
  printf("engine=%-6s  props: vendor=0x%04x device=0x%04x type=%d  props2: vendor=0x%04x type=%d  name=%s\n",engine,p.vendorID,p.deviceID,p.deviceType,p2.properties.vendorID,p2.properties.deviceType,p.deviceName);
  vkDestroyInstance(in,NULL);
}
int main(){ go("Dawn"); go("ANGLE"); go("Dawn"); return 0; }
