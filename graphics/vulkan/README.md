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

## Swapchain

`VulkanSwapchain` owns one `VkSwapchainKHR` for one `VkSurfaceKHR` and the
image views created for its images. Swapchain images themselves remain owned by
Vulkan WSI and are exposed only as indexed handles plus their matching view.

Swapchain planning is separated from object creation. The plan chooses:

- preferred `VK_FORMAT_B8G8R8A8_SRGB` with `SRGB_NONLINEAR` color space when
  available, otherwise the first reported surface format;
- preferred mailbox present mode when available, otherwise mandatory FIFO;
- the surface-fixed extent when Vulkan provides one, otherwise the current
  framebuffer size clamped to surface limits;
- one more image than `minImageCount`, capped by `maxImageCount`;
- exclusive image sharing for a shared graphics/present family, or concurrent
  sharing for separate families.

A zero-width or zero-height framebuffer is treated as suspended presentation.
That state is not fatal and creates no `VkSwapchainKHR`; callers recreate the
presentation chain when the framebuffer becomes non-zero again.

Acquire and present wrappers translate WSI results into explicit backend states:
acquired/presented, suboptimal, out-of-date, timeout/not-ready or suspended.
They do not record rendering commands or perform layout transitions; the future
command/submission path owns those responsibilities.

## Descriptor Manager

`VulkanDescriptorManager` is backend-only descriptor infrastructure. It owns the
descriptor pool, layout-specific set allocation and an immutable cache for
material bindings resolved by the Vulkan backend. Public `Material` continues to
store only backend-independent `ResourceIdentity` values and ranges; it never
stores `VkDescriptorSet`, `VkDescriptorSetLayout` or native resource handles.

Descriptor cache keys are built from the material binding layout plus logical
resource identities and buffer ranges. The concrete `VkBuffer`, `VkImageView`
and `VkSampler` values are supplied only by backend resolution immediately before
descriptor acquisition. A repeated compatible request returns the cached set
instead of updating or freeing an in-flight descriptor set. New cache entries are
allocated once and written once, so the current policy is safe for persistent
material bindings and can be extended to frame/worker arenas later.

Telemetry reports descriptor allocations, cache hits and misses. It is intended
for backend diagnostics and future frame-resource tuning, not for public
renderer API decisions.

## Dispatch

Global Vulkan entry points are loaded with `volkInitialize()`. After successful
instance creation, `volkLoadInstanceOnly()` loads instance-level dispatch.
After successful logical-device creation, `volkLoadDevice()` loads device-level
dispatch.
