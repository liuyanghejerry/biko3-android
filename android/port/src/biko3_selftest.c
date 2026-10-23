/* Android self-test: proves the ported host stack on the device/emulator before
 * the (AArch64) guest is booted. Covers the guest TLS slot mechanism, the guest
 * image header the loader verifies, plain file I/O and a real Vulkan
 * instance/device/surface/swapchain present through the Biko3 bridge. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include "Biko3Vulkan.h"
#include "dm_tsd.h"

typedef void (*report_fn)(const char *line);

static void reportf(report_fn report, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void reportf(report_fn report, const char *format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    report(line);
}

/* ---------- guest TLS slots ---------- */
static pthread_key_t tls_keys[DM_TSD_N];
static int tls_ok;
static int64_t tls_thread_offset;
static uint64_t tls_thread_value;

static void *tls_thread(void *argument) {
    (void)argument;
    /* Each thread gets its own slots; the value must not leak to the main
     * thread. The TP-relative offset is expected to match on Android; under
     * the x86 emulator's ARM translation it does not, which is reported. */
    dm_tsd_setspecific(tls_keys[DM_TSD_KTLR], (void *)(uintptr_t)0x9999);
    tls_thread_offset = dm_tsd_thread_offset();
    tls_thread_value = (uint64_t)(uintptr_t)dm_tsd_getspecific(tls_keys[DM_TSD_KTLR]);
    return NULL;
}

static int selftest_tls(report_fn report) {
    for (int i = 0; i < DM_TSD_N; i++) {
        if (pthread_key_create(&tls_keys[i], NULL)) {
            reportf(report, "tls: pthread_key_create failed");
            return 0;
        }
        dm_tsd_register(i, tls_keys[i]);
    }
    dm_tsd_attach();
    const uintptr_t values[DM_TSD_N] = {0x1111, 0x2222, 0x3333, 0x4444};
    for (int i = 0; i < DM_TSD_N; i++) {
        if (dm_tsd_setspecific(tls_keys[i], (void *)values[i])) {
            reportf(report, "tls: dm_tsd_setspecific(%d) failed", i);
            return 0;
        }
    }
    for (int i = 0; i < DM_TSD_N; i++) {
        if ((uintptr_t)dm_tsd_getspecific(tls_keys[i]) != values[i]) {
            reportf(report, "tls: dm_tsd_getspecific(%d) mismatch", i);
            return 0;
        }
        if (dm_tsd_read_guest(i) != values[i]) {
            reportf(report, "tls: slot %d through the thread pointer holds %#llx, want %#llx",
                    i, (unsigned long long)dm_tsd_read_guest(i), (unsigned long long)values[i]);
            return 0;
        }
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, tls_thread, NULL) == 0) {
        pthread_join(thread, NULL);
        if (tls_thread_value != 0x9999) {
            reportf(report, "tls: second thread read %#llx from its own slot",
                    (unsigned long long)tls_thread_value);
            return 0;
        }
        if (dm_tsd_getspecific(tls_keys[DM_TSD_KTLR]) != (void *)values[DM_TSD_KTLR]) {
            reportf(report, "tls: the second thread leaked into the main slots");
            return 0;
        }
        if (tls_thread_offset != dm_tsd_thread_offset()) {
            reportf(report, "tls: slots work per thread, but the TP offset differs "
                    "across threads (%lld vs %lld); this platform needs the "
                    "per-thread fallback", (long long)tls_thread_offset,
                    (long long)dm_tsd_thread_offset());
        } else {
            reportf(report, "tls: 4 guest slots at TP%+lld, stable across threads",
                    (long long)dm_tsd_thread_offset());
        }
    } else {
        reportf(report, "tls: 4 guest slots at TP%+lld", (long long)dm_tsd_thread_offset());
    }
    tls_ok = 1;
    return 1;
}

/* ---------- guest image identity ---------- */
static int selftest_image(const char *guest_dir, report_fn report) {
    char path[1024];
    snprintf(path, sizeof path, "%s/main.patched.bin", guest_dir);
    FILE *file = fopen(path, "rb");
    if (!file) {
        reportf(report, "image: %s is missing", path);
        return 0;
    }
    unsigned char header[0x80];
    size_t got = fread(header, 1, sizeof header, file);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    if (got != sizeof header || memcmp(header + 0x10, "NRO0", 4)) {
        reportf(report, "image: not an NRO header");
        return 0;
    }
    char build[41];
    for (int i = 0; i < 20; i++) snprintf(build + i * 2, 3, "%02x", header[0x40 + i]);
    reportf(report, "image: %ld bytes, NRO0 header, build id %s", size, build);
    return 1;
}

/* ---------- files ---------- */
static int selftest_files(const char *dir, report_fn report) {
    char path[1024];
    snprintf(path, sizeof path, "%s/selftest.txt", dir);
    FILE *file = fopen(path, "wb");
    if (!file) {
        reportf(report, "files: cannot create %s", path);
        return 0;
    }
    const char payload[] = "biko3 android port";
    if (fwrite(payload, 1, sizeof payload, file) != sizeof payload) {
        fclose(file);
        reportf(report, "files: short write");
        return 0;
    }
    fclose(file);
    char readback[sizeof payload] = {0};
    file = fopen(path, "rb");
    if (!file || fread(readback, 1, sizeof payload, file) != sizeof payload || memcmp(readback, payload, sizeof payload)) {
        if (file) fclose(file);
        reportf(report, "files: read-back mismatch");
        return 0;
    }
    fclose(file);
    unlink(path);
    reportf(report, "files: write/read/unlink ok in %s", dir);
    return 1;
}

/* ---------- Vulkan through the bridge ---------- */
typedef struct {
    PFN_vkCreateInstance create_instance;
    PFN_vkCreateDevice create_device;
    PFN_vkGetInstanceProcAddr instance_proc;
    PFN_vkGetDeviceProcAddr device_proc;
    PFN_vkDestroyDevice destroy_device;
} bridge_t;

static int selftest_vulkan(report_fn report, int frames) {
    bridge_t bridge = {
        (PFN_vkCreateInstance)biko3_vk_resolve("vkCreateInstance"),
        (PFN_vkCreateDevice)biko3_vk_resolve("vkCreateDevice"),
        (PFN_vkGetInstanceProcAddr)biko3_vk_resolve("vkGetInstanceProcAddr"),
        (PFN_vkGetDeviceProcAddr)biko3_vk_resolve("vkGetDeviceProcAddr"),
        (PFN_vkDestroyDevice)biko3_vk_resolve("vkDestroyDevice"),
    };
    if (!bridge.create_instance || !bridge.create_device) {
        reportf(report, "vulkan: bridge is incomplete");
        return 0;
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Biko3 Android self-test",
        .apiVersion = VK_API_VERSION_1_1,
    };
    const char *instance_extensions[] = { "VK_KHR_surface", "VK_KHR_android_surface" };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = instance_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkImageView views[8] = {0};
    VkFramebuffer framebuffers[8] = {0};
    VkCommandBuffer commands[8] = {0};
    uint32_t image_count = 0;
    VkImage images[8] = {0};
    VkResult result = VK_SUCCESS;

    if ((result = bridge.create_instance(&instance_info, NULL, &instance)) != VK_SUCCESS) {
        reportf(report, "vulkan: vkCreateInstance -> %d", (int)result);
        return 0;
    }
    uint32_t devices = 0;
    if (vkEnumeratePhysicalDevices(instance, &devices, NULL) != VK_SUCCESS || !devices) {
        reportf(report, "vulkan: no physical device");
        goto done;
    }
    VkPhysicalDevice list[8];
    if (devices > 8) devices = 8;
    vkEnumeratePhysicalDevices(instance, &devices, list);
    physical = list[0];
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    reportf(report, "vulkan: device %s (API %u.%u.%u)", properties.deviceName,
            VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
            VK_API_VERSION_PATCH(properties.apiVersion));

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, NULL);
    VkQueueFamilyProperties queue_properties[16];
    if (families > 16) families = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, queue_properties);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < families; i++)
        if ((queue_properties[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { family = i; break; }
    if (family == UINT32_MAX) {
        reportf(report, "vulkan: no graphics+compute queue family");
        goto done;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = 1, .ppEnabledExtensionNames = device_extensions,
    };
    if ((result = bridge.create_device(physical, &device_info, NULL, &device)) != VK_SUCCESS) {
        reportf(report, "vulkan: vkCreateDevice -> %d", (int)result);
        goto done;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    PFN_vkCreateAndroidSurfaceKHR create_surface =
        (PFN_vkCreateAndroidSurfaceKHR)bridge.instance_proc(instance, "vkCreateAndroidSurfaceKHR");
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surface_support =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)bridge.instance_proc(instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surface_caps =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)bridge.instance_proc(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surface_formats =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)bridge.instance_proc(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    PFN_vkCreateSwapchainKHR create_swapchain =
        (PFN_vkCreateSwapchainKHR)bridge.device_proc(device, "vkCreateSwapchainKHR");
    PFN_vkGetSwapchainImagesKHR get_images =
        (PFN_vkGetSwapchainImagesKHR)bridge.device_proc(device, "vkGetSwapchainImagesKHR");
    PFN_vkAcquireNextImageKHR acquire =
        (PFN_vkAcquireNextImageKHR)bridge.device_proc(device, "vkAcquireNextImageKHR");
    PFN_vkQueuePresentKHR present =
        (PFN_vkQueuePresentKHR)bridge.device_proc(device, "vkQueuePresentKHR");
    if (!create_surface || !surface_support || !surface_caps || !surface_formats ||
        !create_swapchain || !get_images || !acquire || !present) {
        reportf(report, "vulkan: surface/swapchain entry points missing");
        goto done;
    }
    /* The bridge's NN surface adapter uses the window the UI handed over. */
    typedef struct { VkStructureType sType; const void *pNext; VkFlags flags; void *window; } ViSurface;
    typedef VkResult (*CreateViSurface)(VkInstance, const void *, const VkAllocationCallbacks *, VkSurfaceKHR *);
    CreateViSurface create_vi = (CreateViSurface)bridge.instance_proc(instance, "vkCreateViSurfaceNN");
    if (create_vi) {
        ViSurface info = { (VkStructureType)1000062000, NULL, 0, NULL };
        if ((result = create_vi(instance, (const void *)&info, NULL, &surface)) != VK_SUCCESS) {
            reportf(report, "vulkan: vkCreateViSurfaceNN -> %d", (int)result);
            goto done;
        }
    } else {
        VkAndroidSurfaceCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
        if ((result = create_surface(instance, &info, NULL, &surface)) != VK_SUCCESS) {
            reportf(report, "vulkan: vkCreateAndroidSurfaceKHR -> %d", (int)result);
            goto done;
        }
    }
    VkBool32 supported = VK_FALSE;
    surface_support(physical, family, surface, &supported);
    if (!supported) {
        reportf(report, "vulkan: queue family does not support presentation");
        goto done;
    }
    VkSurfaceCapabilitiesKHR caps;
    if ((result = surface_caps(physical, surface, &caps)) != VK_SUCCESS) {
        reportf(report, "vulkan: surface capabilities -> %d", (int)result);
        goto done;
    }
    uint32_t format_count = 0;
    surface_formats(physical, surface, &format_count, NULL);
    if (!format_count) {
        reportf(report, "vulkan: no surface format");
        goto done;
    }
    VkSurfaceFormatKHR formats[16];
    if (format_count > 16) format_count = 16;
    surface_formats(physical, surface, &format_count, formats);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) { extent.width = 1280; extent.height = 720; }
    VkSwapchainCreateInfoKHR swapchain_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface, .minImageCount = caps.minImageCount + 1,
        .imageFormat = formats[0].format, .imageColorSpace = formats[0].colorSpace,
        .imageExtent = extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
    };
    if (swapchain_info.minImageCount > caps.maxImageCount && caps.maxImageCount)
        swapchain_info.minImageCount = caps.maxImageCount;
    if ((result = create_swapchain(device, &swapchain_info, NULL, &swapchain)) != VK_SUCCESS) {
        reportf(report, "vulkan: vkCreateSwapchainKHR -> %d (%ux%u)", (int)result, extent.width, extent.height);
        goto done;
    }
    get_images(device, swapchain, &image_count, NULL);
    if (!image_count || image_count > 8) {
        reportf(report, "vulkan: unexpected swapchain image count %u", image_count);
        goto done;
    }
    get_images(device, swapchain, &image_count, images);
    reportf(report, "vulkan: swapchain %ux%u, %u images, format %d", extent.width, extent.height,
            image_count, (int)formats[0].format);

    VkAttachmentDescription attachment = {
        .format = formats[0].format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference,
    };
    VkRenderPassCreateInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
        .subpassCount = 1, .pSubpasses = &subpass,
    };
    if (vkCreateRenderPass(device, &pass_info, NULL, &pass) != VK_SUCCESS) {
        reportf(report, "vulkan: vkCreateRenderPass failed");
        goto done;
    }
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    if (vkCreateCommandPool(device, &pool_info, NULL, &pool) != VK_SUCCESS) {
        reportf(report, "vulkan: vkCreateCommandPool failed");
        goto done;
    }
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = formats[0].format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        if (vkCreateImageView(device, &view_info, NULL, &views[i]) != VK_SUCCESS) {
            reportf(report, "vulkan: vkCreateImageView failed");
            goto done;
        }
        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = pass, .attachmentCount = 1, .pAttachments = &views[i],
            .width = extent.width, .height = extent.height, .layers = 1,
        };
        if (vkCreateFramebuffer(device, &framebuffer_info, NULL, &framebuffers[i]) != VK_SUCCESS) {
            reportf(report, "vulkan: vkCreateFramebuffer failed");
            goto done;
        }
    }
    VkCommandBufferAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = image_count,
    };
    if (vkAllocateCommandBuffers(device, &allocate_info, commands) != VK_SUCCESS) {
        reportf(report, "vulkan: vkAllocateCommandBuffers failed");
        goto done;
    }
    for (uint32_t i = 0; i < image_count; i++) {
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(commands[i], &begin);
        VkClearValue clear = { .color = { { 0.06f, 0.09f, 0.16f, 1.0f } } };
        VkRenderPassBeginInfo pass_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass, .framebuffer = framebuffers[i],
            .renderArea = { { 0, 0 }, extent }, .clearValueCount = 1, .pClearValues = &clear,
        };
        vkCmdBeginRenderPass(commands[i], &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(commands[i]);
        vkEndCommandBuffer(commands[i]);
    }
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(device, &fence_info, NULL, &fence) != VK_SUCCESS) {
        reportf(report, "vulkan: vkCreateFence failed");
        goto done;
    }
    for (int frame = 0; frame < frames; frame++) {
        uint32_t index = 0;
        if ((result = acquire(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &index)) != VK_SUCCESS) {
            reportf(report, "vulkan: acquire -> %d", (int)result);
            goto done;
        }
        if (index >= image_count) index = 0;
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 0, .pWaitSemaphores = NULL, .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1, .pCommandBuffers = &commands[index],
        };
        vkResetFences(device, 1, &fence);
        if ((result = vkQueueSubmit(queue, 1, &submit, fence)) != VK_SUCCESS) {
            reportf(report, "vulkan: submit -> %d", (int)result);
            goto done;
        }
        vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000ull);
        VkPresentInfoKHR present_info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &index,
        };
        if ((result = present(queue, &present_info)) != VK_SUCCESS) {
            reportf(report, "vulkan: present -> %d", (int)result);
            goto done;
        }
    }
    vkDeviceWaitIdle(device);
    reportf(report, "vulkan: %d frames presented through the bridge", frames);
    result = VK_SUCCESS;

done:
    if (device) {
        if (fence) vkDestroyFence(device, fence, NULL);
        if (pool) vkDestroyCommandPool(device, pool, NULL);
        for (uint32_t i = 0; i < image_count; i++) {
            if (framebuffers[i]) vkDestroyFramebuffer(device, framebuffers[i], NULL);
            if (views[i]) vkDestroyImageView(device, views[i], NULL);
        }
        if (pass) vkDestroyRenderPass(device, pass, NULL);
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, NULL);
        if (surface) vkDestroySurfaceKHR(instance, surface, NULL);
        if (bridge.destroy_device) bridge.destroy_device(device, NULL);
        else vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    return result == VK_SUCCESS;
}

int biko3_selftest_run(const char *guest_dir, const char *data_dir, report_fn report) {
    int ok = 1;
    reportf(report, "self-test: starting");
    ok &= selftest_tls(report);
    ok &= selftest_image(guest_dir, report);
    ok &= selftest_files(data_dir, report);
    ok &= selftest_vulkan(report, 8);
    if (tls_ok) reportf(report, "self-test: %s", ok ? "passed" : "failed");
    return ok;
}
