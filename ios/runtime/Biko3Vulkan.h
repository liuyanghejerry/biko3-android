#pragma once
#if defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR 1
#else
#define VK_USE_PLATFORM_METAL_EXT 1
#endif
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The UI retains the presentation surface until the guest renderer has
 * stopped: a CAMetalLayer on Apple platforms, an ANativeWindow on Android. */
void biko3_vk_set_metal_layer(const void *layer);
/* Call from serialized foreground/background notifications, outside guest API
 * callbacks. Suspend returns after the tracked Vulkan device becomes idle. */
void biko3_vk_suspend(int suspended);
PFN_vkVoidFunction biko3_vk_resolve(const char *name);
PFN_vkVoidFunction biko3_vk_get_instance_proc_addr(VkInstance instance, const char *name);
PFN_vkVoidFunction biko3_vk_get_device_proc_addr(VkDevice device, const char *name);
#ifdef __cplusplus
}
#endif
