include_external(glfw)
include_external(glm)
include_external(tiny_obj)
include_external(stb_image)

if (GRAPHICS_ENGINE_BUILD_VULKAN)
    include_external(vulkan)
endif ()
