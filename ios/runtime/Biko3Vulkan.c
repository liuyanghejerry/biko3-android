#include "Biko3Vulkan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

/* NN is a guest platform extension; its sole window field is deliberately never
 * dereferenced. Every non-platform Vulkan handle is a real driver handle
 * (MoltenVK on Apple platforms, libvulkan on Android). */
#define NN_SURFACE "VK_NN_vi_surface"
#if defined(__ANDROID__)
#define PLATFORM_SURFACE_EXTENSION VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
#else
#define PLATFORM_SURFACE_EXTENSION VK_EXT_METAL_SURFACE_EXTENSION_NAME
#endif
typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *window; } GuestViSurfaceInfo;
static _Atomic(const void *) metal_layer;
void biko3_vk_set_metal_layer(const void *layer) { atomic_store(&metal_layer, layer); }

/* There is one renderer/device in this game's analyzed create path. The state
 * lock never crosses Vulkan calls; lifecycle serializes idle/destruction. */
static pthread_mutex_t graphics_state = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t graphics_lifecycle = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t graphics_changed = PTHREAD_COND_INITIALIZER;
static VkDevice active_device;
static unsigned active_queue_calls;
static int graphics_suspended;
static int device_destroying;

static void enter_queue(void) {
    pthread_mutex_lock(&graphics_state);
    while (graphics_suspended || device_destroying)
        pthread_cond_wait(&graphics_changed, &graphics_state);
    ++active_queue_calls;
    pthread_mutex_unlock(&graphics_state);
}
static void leave_queue(void) {
    pthread_mutex_lock(&graphics_state);
    --active_queue_calls;
    pthread_cond_broadcast(&graphics_changed);
    pthread_mutex_unlock(&graphics_state);
}
void biko3_vk_suspend(int suspended) {
    pthread_mutex_lock(&graphics_lifecycle);
    pthread_mutex_lock(&graphics_state);
    graphics_suspended = !!suspended;
    VkDevice device = active_device;
    if (suspended) {
        while (active_queue_calls) pthread_cond_wait(&graphics_changed, &graphics_state);
    } else pthread_cond_broadcast(&graphics_changed);
    pthread_mutex_unlock(&graphics_state);
    if (suspended && device) {
        VkResult result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS) fprintf(stderr, "Biko3 background device idle: %d\n", result);
    }
    pthread_mutex_unlock(&graphics_lifecycle);
}
static VkResult queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    enter_queue();
    VkResult result = vkQueueSubmit(queue, count, submits, fence);
    leave_queue(); return result;
}
#if defined(__ANDROID__)
/* One-shot readback of a presented swapchain image. A screenshot shows the
 * compositor's output; this writes the guest's own picture so its orientation
 * and aspect can be checked directly. Enabled by BIKO3_DUMP_FRAME=<path>. */
static VkDevice g_frame_device;
static VkQueue g_frame_queue;
static uint32_t g_frame_family;
static uint32_t g_frame_memory_type;
static uint32_t g_swap_width, g_swap_height;
static int g_frame_presents;

static void dump_frame(VkSwapchainKHR swapchain, uint32_t index) {
    const char *path = getenv("BIKO3_DUMP_FRAME");
    if (!path || !g_frame_device || !g_frame_queue || !g_swap_width) return;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(g_frame_device, swapchain, &count, NULL);
    if (!count) return;
    VkImage *images = calloc(count, sizeof(*images));
    if (!images) return;
    vkGetSwapchainImagesKHR(g_frame_device, swapchain, &count, images);
    VkImage image = index < count ? images[index] : images[0];
    free(images);
    VkDeviceSize bytes = (VkDeviceSize)g_swap_width * g_swap_height * 4;
    VkBufferCreateInfo buffer_info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(g_frame_device, &buffer_info, NULL, &buffer) != VK_SUCCESS) return;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(g_frame_device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = g_frame_memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(g_frame_device, &allocation, NULL, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(g_frame_device, buffer, NULL); return;
    }
    vkBindBufferMemory(g_frame_device, buffer, memory, 0);
    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool_info.queueFamilyIndex = g_frame_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(g_frame_device, &pool_info, NULL, &pool) != VK_SUCCESS) {
        vkFreeMemory(g_frame_device, memory, NULL); vkDestroyBuffer(g_frame_device, buffer, NULL); return;
    }
    VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(g_frame_device, &command_info, &command);
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(command, &begin);
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy region = { 0 };
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = g_swap_width;
    region.imageExtent.height = g_swap_height;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    vkEndCommandBuffer(command);
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(g_frame_device, &fence_info, NULL, &fence);
    vkQueueSubmit(g_frame_queue, 1, &submit, fence);
    vkWaitForFences(g_frame_device, 1, &fence, VK_TRUE, 2000000000ull);
    void *mapped = NULL;
    if (vkMapMemory(g_frame_device, memory, 0, bytes, 0, &mapped) == VK_SUCCESS) {
        FILE *out = fopen(path, "wb");
        if (out) {
            fprintf(out, "P6\n%u %u\n255\n", g_swap_width, g_swap_height);
            const unsigned char *pixels = mapped;
            for (VkDeviceSize i = 0; i + 4 <= bytes; i += 4) fwrite(pixels + i, 1, 3, out);
            fclose(out);
            fprintf(stderr, "[biko3] frame dumped: %s (%ux%u)\n", path, g_swap_width, g_swap_height);
        }
        vkUnmapMemory(g_frame_device, memory);
    }
    vkDestroyFence(g_frame_device, fence, NULL);
    vkDestroyCommandPool(g_frame_device, pool, NULL);
    vkDestroyBuffer(g_frame_device, buffer, NULL);
    vkFreeMemory(g_frame_device, memory, NULL);
}
#endif

#if defined(__ANDROID__)
/* The Android present path does not block on vblank, so the guest would run far
 * above the display rate (300+ fps on a phone, wasting power). Pace the present
 * calls after they are queued; BIKO3_FPS=0 disables the cap. */
static void pace_present(void) {
    static struct timespec last;
    static int target = -1;
    if (target < 0) {
        const char *configured = getenv("BIKO3_FPS");
        target = configured ? atoi(configured) : 60;
        if (target < 0) target = 0;
    }
    if (target <= 0) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec | last.tv_nsec) {
        long long elapsed = (now.tv_sec - last.tv_sec) * 1000000000LL +
                            (now.tv_nsec - last.tv_nsec);
        long long period = 1000000000LL / target;
        if (elapsed < period) {
            struct timespec delay = { (time_t)((period - elapsed) / 1000000000LL),
                                      (long)((period - elapsed) % 1000000000LL) };
            nanosleep(&delay, NULL);
            clock_gettime(CLOCK_MONOTONIC, &now);
        }
    }
    last = now;
}
#endif

static VkResult queue_present(VkQueue queue, const VkPresentInfoKHR *present) {
#if defined(__ANDROID__)
    if (getenv("BIKO3_DUMP_FRAME") && ++g_frame_presents == 300 && present &&
        present->swapchainCount && present->pSwapchains && present->pImageIndices) {
        dump_frame(present->pSwapchains[0], present->pImageIndices[0]);
    }
#endif
    enter_queue();
    VkResult result = vkQueuePresentKHR(queue, present);
    leave_queue();
#if defined(__ANDROID__)
    pace_present();
#endif
    return result;
}
static VkResult device_wait_idle(VkDevice device) {
    enter_queue();
    VkResult result = vkDeviceWaitIdle(device);
    leave_queue(); return result;
}
static void destroy_device(VkDevice device, const VkAllocationCallbacks *alloc) {
    pthread_mutex_lock(&graphics_lifecycle);
    pthread_mutex_lock(&graphics_state);
    device_destroying = 1;
    while (active_queue_calls) pthread_cond_wait(&graphics_changed, &graphics_state);
    if (active_device == device) active_device = VK_NULL_HANDLE;
    pthread_mutex_unlock(&graphics_state);
    vkDestroyDevice(device, alloc);
    pthread_mutex_lock(&graphics_state);
    device_destroying = 0;
    pthread_cond_broadcast(&graphics_changed);
    pthread_mutex_unlock(&graphics_state);
    pthread_mutex_unlock(&graphics_lifecycle);
}

static PFN_vkVoidFunction queue_override(const char *name) {
    if (!strcmp(name, "vkQueueSubmit")) return (PFN_vkVoidFunction)queue_submit;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)queue_present;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)destroy_device;
    if (!strcmp(name, "vkDeviceWaitIdle")) return (PFN_vkVoidFunction)device_wait_idle;
    return NULL;
}

static int has_extension(const VkExtensionProperties *p, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; ++i) if (!strcmp(p[i].extensionName, name)) return 1;
    return 0;
}
typedef VkResult (*ExtensionQuery)(void *, uint32_t *, VkExtensionProperties *);
static VkResult query_instance(void *context, uint32_t *count, VkExtensionProperties *properties) {
    return vkEnumerateInstanceExtensionProperties((const char *)context, count, properties);
}
static VkResult query_device(void *context, uint32_t *count, VkExtensionProperties *properties) {
    return vkEnumerateDeviceExtensionProperties((VkPhysicalDevice)context, NULL, count, properties);
}
/* Re-query count after VK_INCOMPLETE. Never trust a count that grew past the
 * allocated capacity; the alias entry uses one separately allocated element. */
static VkResult extension_snapshot(ExtensionQuery query, void *context,
                                   uint32_t *count, VkExtensionProperties **properties) {
    *properties = NULL; *count = 0;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        uint32_t capacity = 0;
        VkResult result = query(context, &capacity, NULL);
        if (result != VK_SUCCESS) return result;
        if (capacity == UINT32_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkExtensionProperties *entries = calloc((size_t)capacity + 1, sizeof(*entries));
        if (!entries) return VK_ERROR_OUT_OF_HOST_MEMORY;
        uint32_t used = capacity;
        result = query(context, &used, entries);
        if (result == VK_SUCCESS && used <= capacity) {
            *properties = entries; *count = used; return VK_SUCCESS;
        }
        free(entries);
        if (result != VK_INCOMPLETE) return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}
static VkResult instance_extensions(const char *layer, uint32_t *count, VkExtensionProperties *out) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 0;
    VkExtensionProperties *all = NULL;
    VkResult r = extension_snapshot(query_instance, (void *)layer, &n, &all);
    if (r != VK_SUCCESS) return r;
    /* Advertise NN only when backed by the actual Metal surface extension. */
    if (has_extension(all, n, PLATFORM_SURFACE_EXTENSION) && !has_extension(all, n, NN_SURFACE)) {
        strcpy(all[n].extensionName, NN_SURFACE); all[n].specVersion = 1; ++n;
    }
    if (out) { uint32_t used = *count < n ? *count : n;
        memcpy(out, all, used * sizeof(*all)); *count = used;
        r = used < n ? VK_INCOMPLETE : VK_SUCCESS;
    } else *count = n;
    free(all); return r;
}

static VkResult create_instance(const VkInstanceCreateInfo *in, const VkAllocationCallbacks *alloc, VkInstance *out) {
    if (!in || !out) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 0;
    VkExtensionProperties *available = NULL;
    VkResult r = extension_snapshot(query_instance, NULL, &n, &available);
    if (r != VK_SUCCESS) return r;
    const char **names = calloc((size_t)in->enabledExtensionCount + 2, sizeof(*names));
    if (!available || !names) { free(available); free(names); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkInstanceCreateInfo info = *in;
    uint32_t used = 0;
    int portability = 0;
    for (uint32_t i = 0; i < in->enabledExtensionCount; ++i) {
        const char *name = in->ppEnabledExtensionNames[i];
        if (!strcmp(name, NN_SURFACE)) name = PLATFORM_SURFACE_EXTENSION;
        if (!strcmp(name, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) portability = 1;
        int duplicate = 0;
        for (uint32_t j = 0; j < used; ++j) if (!strcmp(names[j], name)) duplicate = 1;
        if (!duplicate) names[used++] = name;
    }
    if (has_extension(available, n, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        if (!portability) names[used++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    info.enabledExtensionCount = used; info.ppEnabledExtensionNames = names;
    r = vkCreateInstance(&info, alloc, out);
    free(names); free(available); return r;
}

static VkResult create_vi_surface(VkInstance instance, const GuestViSurfaceInfo *in,
                                  const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    const void *window = atomic_load(&metal_layer);
    if (!in || !window || in->sType != (VkStructureType)1000062000 || in->pNext || in->flags)
        return VK_ERROR_INITIALIZATION_FAILED;
#if defined(__ANDROID__)
    PFN_vkCreateAndroidSurfaceKHR create = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR");
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidSurfaceCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .pNext = NULL, .flags = 0, .window = (ANativeWindow *)window };
    return create(instance, &info, alloc, out);
#else
    PFN_vkCreateMetalSurfaceEXT create = (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT");
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMetalSurfaceCreateInfoEXT info = { .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
        .pLayer = (const CAMetalLayer *)window };
    return create(instance, &info, alloc, out);
#endif
}

#if defined(__ANDROID__)
/* Surface capability logging: the game rejects a swapchain it considers
 * unsupported, and the exact limits differ between drivers. */
static VkResult logged_surface_caps(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                    VkSurfaceCapabilitiesKHR *caps) {
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, caps);
    if (result == VK_SUCCESS) {
        /* gfxstream-backed Android drivers commonly advertise only
         * VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR. An opaque RGBX window is opaque
         * anyway, so report OPAQUE as well and map it back at creation. */
        caps->supportedCompositeAlpha |= VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        fprintf(stderr, "Biko3 surface caps: extent %ux%u (min %ux%u max %ux%u), images %u..%u, "
                "usage %#x, transform %#x, alpha %#x\n",
                caps->currentExtent.width, caps->currentExtent.height,
                caps->minImageExtent.width, caps->minImageExtent.height,
                caps->maxImageExtent.width, caps->maxImageExtent.height,
                caps->minImageCount, caps->maxImageCount,
                (unsigned)caps->supportedUsageFlags, (unsigned)caps->supportedTransforms,
                (unsigned)caps->currentTransform,
                (unsigned)caps->supportedCompositeAlpha);
    } else {
        fprintf(stderr, "Biko3 surface caps: result %d\n", (int)result);
    }
    return result;
}
#endif

#if defined(__ANDROID__)
/* The Android presentation engine treats an RGBX window as opaque; translate
 * the OPAQUE composite alpha the game asks for to the advertised INHERIT. */
static VkResult bridge_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *in,
                                        const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    VkSwapchainCreateInfoKHR info = *in;
    if (info.compositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkResult result = vkCreateSwapchainKHR(device, &info, alloc, out);
#if defined(__ANDROID__)
    if (result == VK_SUCCESS) { g_swap_width = info.imageExtent.width; g_swap_height = info.imageExtent.height; }
#endif
    fprintf(stderr, "Biko3 vkCreateSwapchainKHR: %ux%u, %u images, format %d, mode %d -> %d\n",
            info.imageExtent.width, info.imageExtent.height, info.minImageCount,
            (int)info.imageFormat, (int)info.presentMode, (int)result);
    return result;
}
#endif

static VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *in,
                             const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (!in || !out) return VK_ERROR_INITIALIZATION_FAILED;
    /* Replaces the original Mesa-NVK vendor restriction with real requirements.
     * Original features/pNext are preserved and validated by vkCreateDevice. */
    uint32_t queues = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &queues, NULL);
    VkQueueFamilyProperties *families = calloc(queues ? queues : 1, sizeof(*families));
    if (!families) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queues, families);
    int graphics_compute = 0;
    for (uint32_t i = 0; i < in->queueCreateInfoCount; ++i) {
        uint32_t q = in->pQueueCreateInfos[i].queueFamilyIndex;
        if (q < queues && in->pQueueCreateInfos[i].queueCount &&
            (families[q].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) graphics_compute = 1;
    }
    free(families);
    if (!graphics_compute) return VK_ERROR_FEATURE_NOT_PRESENT;
    fprintf(stderr, "Biko3 vkCreateDevice: %u queue create infos, %u extensions, features %p, pNext %p\n",
            in->queueCreateInfoCount, in->enabledExtensionCount, (const void *)in->pEnabledFeatures, in->pNext);
    for (uint32_t i = 0; i < in->enabledExtensionCount; ++i)
        fprintf(stderr, "Biko3   extension: %s\n", in->ppEnabledExtensionNames[i]);
#if defined(__ANDROID__)
    for (const VkBaseInStructure *next = (const VkBaseInStructure *)in->pNext; next; next = next->pNext)
        fprintf(stderr, "Biko3   pNext sType: %d\n", (int)next->sType);
    if (in->pEnabledFeatures) {
        const VkPhysicalDeviceFeatures *f = in->pEnabledFeatures;
#define FEATURE(name) if (f->name) fprintf(stderr, "Biko3   feature: %s\n", #name)
        FEATURE(robustBufferAccess); FEATURE(fullDrawIndexUint32); FEATURE(imageCubeArray);
        FEATURE(independentBlend); FEATURE(geometryShader); FEATURE(tessellationShader);
        FEATURE(sampleRateShading); FEATURE(dualSrcBlend); FEATURE(logicOp);
        FEATURE(multiDrawIndirect); FEATURE(drawIndirectFirstInstance); FEATURE(depthClamp);
        FEATURE(depthBiasClamp); FEATURE(fillModeNonSolid); FEATURE(depthBounds);
        FEATURE(wideLines); FEATURE(largePoints); FEATURE(alphaToOne); FEATURE(multiViewport);
        FEATURE(samplerAnisotropy); FEATURE(textureCompressionETC2); FEATURE(textureCompressionASTC_LDR);
        FEATURE(textureCompressionBC); FEATURE(occlusionQueryPrecise); FEATURE(pipelineStatisticsQuery);
        FEATURE(vertexPipelineStoresAndAtomics); FEATURE(fragmentStoresAndAtomics);
        FEATURE(shaderTessellationAndGeometryPointSize); FEATURE(shaderImageGatherExtended);
        FEATURE(shaderStorageImageExtendedFormats); FEATURE(shaderStorageImageMultisample);
        FEATURE(shaderStorageImageReadWithoutFormat); FEATURE(shaderStorageImageWriteWithoutFormat);
        FEATURE(shaderUniformBufferArrayDynamicIndexing); FEATURE(shaderSampledImageArrayDynamicIndexing);
        FEATURE(shaderStorageBufferArrayDynamicIndexing); FEATURE(shaderStorageImageArrayDynamicIndexing);
        FEATURE(shaderClipDistance); FEATURE(shaderCullDistance); FEATURE(shaderFloat64);
        FEATURE(shaderInt64); FEATURE(shaderInt16); FEATURE(shaderResourceResidency);
        FEATURE(shaderResourceMinLod); FEATURE(sparseBinding); FEATURE(sparseResidencyBuffer);
        FEATURE(sparseResidencyImage2D); FEATURE(sparseResidencyImage3D); FEATURE(sparseResidency2Samples);
        FEATURE(sparseResidency4Samples); FEATURE(sparseResidency8Samples); FEATURE(sparseResidency16Samples);
        FEATURE(sparseResidencyAliased); FEATURE(variableMultisampleRate); FEATURE(inheritedQueries);
#undef FEATURE
    }
#endif
    uint32_t n = 0;
    VkExtensionProperties *available = NULL;
    VkResult r = extension_snapshot(query_device, (void *)physical, &n, &available);
    if (r != VK_SUCCESS) return r;
    const char **names = calloc((size_t)in->enabledExtensionCount + 1, sizeof(*names));
    if (!available || !names) { free(available); free(names); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkDeviceCreateInfo info = *in;
    uint32_t used = in->enabledExtensionCount;
    int portability = 0;
    for (uint32_t i = 0; i < used; ++i) {
        names[i] = in->ppEnabledExtensionNames[i];
        if (!strcmp(names[i], "VK_KHR_portability_subset")) portability = 1;
    }
    if (!portability && has_extension(available, n, "VK_KHR_portability_subset")) names[used++] = "VK_KHR_portability_subset";
    info.enabledExtensionCount = used; info.ppEnabledExtensionNames = names;
    r = vkCreateDevice(physical, &info, alloc, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&graphics_lifecycle);
        pthread_mutex_lock(&graphics_state);
        int occupied = active_device != VK_NULL_HANDLE;
        if (!occupied) active_device = *out;
        pthread_mutex_unlock(&graphics_state);
        pthread_mutex_unlock(&graphics_lifecycle);
        if (occupied) {
            vkDestroyDevice(*out, alloc); *out = VK_NULL_HANDLE;
            r = VK_ERROR_TOO_MANY_OBJECTS;
        } else {
#if defined(__ANDROID__)
            g_frame_device = *out;
            VkPhysicalDeviceMemoryProperties memory_properties;
            vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
            for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
                VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
                if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    g_frame_memory_type = i;
                    break;
                }
            }
            for (uint32_t i = 0; i < in->queueCreateInfoCount; i++) {
                if (in->pQueueCreateInfos[i].queueCount) {
                    g_frame_family = in->pQueueCreateInfos[i].queueFamilyIndex;
                    vkGetDeviceQueue(g_frame_device, g_frame_family, 0, &g_frame_queue);
                    break;
                }
            }
#endif
        }
    }
    fprintf(stderr, "Biko3 Vulkan device creation: %d (real graphics + compute queue)\n", r);
    free(names); free(available); return r;
}

PFN_vkVoidFunction biko3_vk_get_instance_proc_addr(VkInstance instance, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)biko3_vk_get_instance_proc_addr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)biko3_vk_get_device_proc_addr;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)instance_extensions;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)create_instance;
    if (instance && !strcmp(name, "vkCreateViSurfaceNN")) return (PFN_vkVoidFunction)create_vi_surface;
    if (instance && !strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)create_device;
#if defined(__ANDROID__)
    if (instance && !strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return (PFN_vkVoidFunction)logged_surface_caps;
    if (instance && !strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)bridge_create_swapchain;
#endif
    if (instance) {
        PFN_vkVoidFunction override = queue_override(name);
        if (override && vkGetInstanceProcAddr(instance, name)) return override;
    }
    return vkGetInstanceProcAddr(instance, name);
}
PFN_vkVoidFunction biko3_vk_get_device_proc_addr(VkDevice device, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)biko3_vk_get_device_proc_addr;
    if (device) {
        PFN_vkVoidFunction override = queue_override(name);
        if (override && vkGetDeviceProcAddr(device, name)) return override;
    }
    return vkGetDeviceProcAddr(device, name);
}

/* Addresses are linked, rather than queried with a null Vulkan instance: most
 * Vulkan commands are intentionally unavailable from null-instance lookup. */
PFN_vkVoidFunction biko3_vk_resolve(const char *name) {
    if (!name) return NULL;
    PFN_vkVoidFunction override = queue_override(name);
    if (override) return override;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)create_instance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)create_device;
    if (!strcmp(name, "vkCreateViSurfaceNN")) return (PFN_vkVoidFunction)create_vi_surface;
#if defined(__ANDROID__)
    if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return (PFN_vkVoidFunction)logged_surface_caps;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)bridge_create_swapchain;
#endif
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)instance_extensions;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)biko3_vk_get_instance_proc_addr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)biko3_vk_get_device_proc_addr;
#define BIKO3_VK_EXPORT(fn) if (!strcmp(name, #fn)) return (PFN_vkVoidFunction)fn;
#include "Biko3VulkanExports.inc"
#undef BIKO3_VK_EXPORT
    return NULL;
}

/* The cast required by vkGet*ProcAddr must not conceal an ABI mismatch. */
#define BIKO3_CHECK_API(function, type) \
    _Static_assert(_Generic(&(function), type: 1, default: 0), #function " Vulkan ABI mismatch")
BIKO3_CHECK_API(create_instance, PFN_vkCreateInstance);
BIKO3_CHECK_API(create_device, PFN_vkCreateDevice);
BIKO3_CHECK_API(destroy_device, PFN_vkDestroyDevice);
BIKO3_CHECK_API(instance_extensions, PFN_vkEnumerateInstanceExtensionProperties);
BIKO3_CHECK_API(queue_submit, PFN_vkQueueSubmit);
BIKO3_CHECK_API(queue_present, PFN_vkQueuePresentKHR);
BIKO3_CHECK_API(device_wait_idle, PFN_vkDeviceWaitIdle);
#undef BIKO3_CHECK_API
