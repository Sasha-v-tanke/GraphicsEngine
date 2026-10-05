# Vulkan Backend

`VulkanBackend` owns Vulkan loader, instance bootstrap, physical-device selection and logical-device
bootstrap for GraphicsEngine.

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

## Logical Device

`VulkanDevice` creates and owns one `VkDevice` for a selected physical device.
It enables only the queue families required by graphics and presentation; when
both roles use the same family, only one queue create request is emitted.

The logical-device layer enables the required Vulkan 1.2/1.3 features from the
selected physical-device snapshot:

- timeline semaphore;
- Synchronization2;
- Dynamic Rendering.

After successful device creation, device-level Volk dispatch is loaded through
`volkLoadDevice()`. The graphics and present queue handles are retrieved once
and exposed only as `VulkanLockedQueue`, so backend code holds the queue mutex
while using the raw `VkQueue`. Shared graphics/present queues therefore have one
host-synchronization boundary.

`VulkanDevice` publishes immutable `GraphicsCapabilities` from the selected
device snapshot. The current public capability surface maps Vulkan presentation
support, timeline semaphore support and the backend frame-in-flight policy.

## Swapchain Presentation

`VulkanSwapchain` owns the single presentation chain for one surface. Swapchain
selection is split into a CPU-testable config step and the runtime Vulkan object
creation step.

The selection policy is deterministic:

- prefer `VK_FORMAT_B8G8R8A8_SRGB` with `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`,
  otherwise use the first surface format;
- prefer `VK_PRESENT_MODE_MAILBOX_KHR`, otherwise fall back to FIFO when
  available;
- use the surface fixed extent when provided, otherwise clamp the framebuffer
  size to surface bounds;
- request one more image than the surface minimum, clamped to the surface
  maximum when it is finite;
- use exclusive image sharing for a shared graphics/present family and
  concurrent sharing when the families differ.

A zero framebuffer extent is a suspended presentation state. In that state no
`VkSwapchainKHR` is created and acquire/present calls are rejected with the
project error model instead of treating minimization as a fatal backend failure.
`SwapchainImageIndex` values come from `vkAcquireNextImageKHR` and are unrelated
to Engine `FrameExecutionSlot` indices.

## Dispatch

Global Vulkan entry points are loaded with `volkInitialize()`. After successful
instance creation, `volkLoadInstanceOnly()` loads instance-level dispatch.
After successful logical-device creation, `volkLoadDevice()` loads device-level
dispatch.
