#include <gst/gst.h>
#include <gst/vulkan/vulkan.h>
#include <cstdio>

int main() {
    gst_init(nullptr, nullptr);
    GError* error = nullptr;
    GstPlugin* plugin = gst_plugin_load_file("gst-plugins/gstvulkan.dll", &error);
    if (!plugin) {
        std::fprintf(stderr, "Vulkan plugin load failed: %s\n",
            error ? error->message : "unknown");
        if (error) g_error_free(error);
        return 1;
    }
    GstElement* element = gst_element_factory_make("vulkanh264dec", nullptr);
    GstVulkanInstance* instance = gst_vulkan_instance_new();
    if (!element || !instance || !gst_vulkan_instance_open(instance, &error)) {
        std::fprintf(stderr, "Vulkan context setup failed (%s): %s\n",
            !element ? "element" : !instance ? "instance" : "instance open",
            error ? error->message : "unknown");
        if (error) g_error_free(error);
        return 1;
    }
    GstVulkanDevice* device = gst_vulkan_device_new_with_index(instance, 0);
    if (!device || !gst_vulkan_device_open(device, &error)) {
        std::fprintf(stderr, "Vulkan device setup failed: %s\n",
            error ? error->message : "unknown");
        if (error) g_error_free(error);
        return 1;
    }

    GstContext* deviceContext = gst_context_new(
        GST_VULKAN_DEVICE_CONTEXT_TYPE_STR, TRUE);
    gst_context_set_vulkan_device(deviceContext, device);
    gst_element_set_context(element, deviceContext);
    gst_context_unref(deviceContext);
    GstVulkanDevice* foundDevice = nullptr;
    bool ok = gst_vulkan_device_run_context_query(element, &foundDevice) &&
        foundDevice == device;
    if (foundDevice) gst_object_unref(foundDevice);

    GArray* families = gst_vulkan_device_queue_family_indices(device);
    GstVulkanQueue* queue = families && families->len
        ? gst_vulkan_device_get_queue(device,
            g_array_index(families, guint32, 0), 0) : nullptr;
    if (families) g_array_unref(families);
    if (queue) {
        GstContext* queueContext = gst_context_new(
            GST_VULKAN_QUEUE_CONTEXT_TYPE_STR, TRUE);
        gst_context_set_vulkan_queue(queueContext, queue);
        gst_element_set_context(element, queueContext);
        gst_context_unref(queueContext);
        GstVulkanQueue* foundQueue = nullptr;
        ok = ok && gst_vulkan_queue_run_context_query(element, &foundQueue) &&
            foundQueue && foundQueue->queue == queue->queue;
        if (foundQueue) gst_object_unref(foundQueue);
        gst_object_unref(queue);
    } else {
        ok = false;
    }

    gst_object_unref(device);
    gst_object_unref(instance);
    gst_object_unref(element);
    gst_object_unref(plugin);
    std::fprintf(stderr, "Unlinked decoder context lookup: %s\n",
        ok ? "passed" : "failed");
    return ok ? 0 : 1;
}
