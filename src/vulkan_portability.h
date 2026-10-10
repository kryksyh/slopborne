#ifndef BB_VULKAN_PORTABILITY_H
#define BB_VULKAN_PORTABILITY_H
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* MoltenVK devices are hidden by the loader unless enumeration is opted in. */
static inline void bb_vulkan_portability_instance(VkInstanceCreateInfo *info) {
#ifdef __APPLE__
    uint32_t count=0;
    if (vkEnumerateInstanceExtensionProperties(NULL,&count,NULL)!=VK_SUCCESS) return;
    VkExtensionProperties *properties=(VkExtensionProperties *)calloc(count ? count : 1,sizeof(*properties));
    if (!properties) return;
    if (vkEnumerateInstanceExtensionProperties(NULL,&count,properties)==VK_SUCCESS) {
        static const char *extensions[]={VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
        for (uint32_t i=0;i<count;++i) {
            if (!strcmp(properties[i].extensionName,extensions[0])) {
                info->flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
                info->enabledExtensionCount=1;
                info->ppEnabledExtensionNames=extensions;
                break;
            }
        }
    }
    free(properties);
#else
    (void)info;
#endif
}

static inline void bb_vulkan_portability_device(VkPhysicalDevice physical, VkDeviceCreateInfo *info) {
    uint32_t count=0;
    if (vkEnumerateDeviceExtensionProperties(physical,NULL,&count,NULL)!=VK_SUCCESS) return;
    VkExtensionProperties *properties=(VkExtensionProperties *)calloc(count ? count : 1,sizeof(*properties));
    if (!properties) return;
    if (vkEnumerateDeviceExtensionProperties(physical,NULL,&count,properties)==VK_SUCCESS) {
        static const char *extensions[]={"VK_KHR_portability_subset"};
        for (uint32_t i=0;i<count;++i) {
            if (!strcmp(properties[i].extensionName,extensions[0])) {
                info->enabledExtensionCount=1;
                info->ppEnabledExtensionNames=extensions;
                break;
            }
        }
    }
    free(properties);
}
#endif
