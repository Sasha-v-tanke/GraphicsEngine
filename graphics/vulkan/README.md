# Vulkan Backend

`VulkanBackend` owns Vulkan loader, instance bootstrap and physical-device selection for GraphicsEngine.

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

`VulkanInstance` does not create a physical device, logical device, swapchain,
GPU resources, queues or command buffers.

## GLFW Surface Integration

`glfw_surface.h` is the only boundary that combines Vulkan native types with
GLFW window access. Public `Window` keeps no native-handle getter.

The integration:

- reports GLFW-required Vulkan instance extensions before instance creation;
- creates and destroys one `VkSurfaceKHR` through `VulkanSurface`;
- reaches the GLFW window through an internal Window engine contract only;
- requires the `Window`, `VulkanInstance` and GLFW runtime to outlive the
  surface object.

## Physical Device Selection

Physical-device selection first snapshots Vulkan properties into
`VulkanPhysicalDeviceCapabilities`, then evaluates that CPU-testable value type.
The runtime path only maps the selected snapshot back to its `VkPhysicalDevice`.
It does not create `VkDevice`.

The default Vulkan backend requires:

- Vulkan 1.3 or newer;
- `VK_KHR_swapchain`;
- timeline semaphore, Synchronization2 and Dynamic Rendering features;
- at least one graphics-capable queue and one presentation-capable queue;
- valid surface support with at least one swapchain format and present mode.

Suitable devices are scored deterministically. Device type is the primary
preference, a shared graphics/present queue family receives a smaller bonus, and
`maxImageDimension2D` is a final capability contribution. Equal scores are
resolved by stable device UUID, vendor/device IDs and name, so enumeration order
does not decide the winner.

Rejected candidates retain explicit reasons for every missing required
capability. If no candidate is suitable, initialization fails with diagnostics
for all candidates.

## Dispatch

Global Vulkan entry points are loaded with `volkInitialize()`. After successful
instance creation, `volkLoadInstanceOnly()` loads instance-level dispatch.
Device-level dispatch belongs to the logical-device layer.
