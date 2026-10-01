# Vulkan Backend

`VulkanBackend` owns Vulkan loader and instance bootstrap for GraphicsEngine.

This layer is backend-internal. It may use Vulkan native types, but higher-level
Window, Application, Engine and future Graphics public contracts must not depend
on Vulkan headers through this module.

## Instance

`VulkanInstance` is the first Vulkan lifetime object:

- initializes the Volk loader through one code path;
- checks the requested Vulkan instance API version before creation;
- resolves required and optional instance extensions from the current loader;
- enables `VK_KHR_portability_enumeration` when requested for MoltenVK-style
  portability enumeration;
- enables `VK_LAYER_KHRONOS_validation` when requested and available;
- wires `VK_EXT_debug_utils` when available in development configuration;
- owns `VkInstance` and debug messenger destruction through RAII.

The object is transactional: constructor failure publishes no partial instance,
and already-created Vulkan objects are destroyed before the exception escapes.

This module does not create a physical device, logical device, surface,
swapchain, GPU resources, queues or command buffers.

## Dispatch

Global Vulkan entry points are loaded with `volkInitialize()`. After successful
instance creation, `volkLoadInstanceOnly()` loads instance-level dispatch.
Device-level dispatch belongs to a later logical-device layer.
