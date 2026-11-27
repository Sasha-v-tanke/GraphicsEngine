if (NOT DEFINED GRAPHICS_ENGINE_PUBLIC_HEADER_MANIFEST)
    message(FATAL_ERROR "GRAPHICS_ENGINE_PUBLIC_HEADER_MANIFEST is required")
endif ()

if (NOT DEFINED GRAPHICS_ENGINE_INCLUDE_DIRECTORY)
    message(FATAL_ERROR "GRAPHICS_ENGINE_INCLUDE_DIRECTORY is required")
endif ()

include("${GRAPHICS_ENGINE_PUBLIC_HEADER_MANIFEST}")

if (NOT GRAPHICS_ENGINE_PUBLIC_HEADERS)
    message(FATAL_ERROR "Public header allowlist is empty")
endif ()

file(GLOB_RECURSE installedHeaders
    RELATIVE "${GRAPHICS_ENGINE_INCLUDE_DIRECTORY}"
    "${GRAPHICS_ENGINE_INCLUDE_DIRECTORY}/*.h")
list(SORT installedHeaders)
list(SORT GRAPHICS_ENGINE_PUBLIC_HEADERS)

if (NOT installedHeaders STREQUAL GRAPHICS_ENGINE_PUBLIC_HEADERS)
    message(FATAL_ERROR
        "Installed public headers do not match the CMake API allowlist\n"
        "Expected: ${GRAPHICS_ENGINE_PUBLIC_HEADERS}\n"
        "Actual: ${installedHeaders}")
endif ()

foreach (header IN LISTS GRAPHICS_ENGINE_PUBLIC_HEADERS)
    if (IS_ABSOLUTE "${header}" OR header MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "Invalid public header path '${header}'")
    endif ()

    set(path "${GRAPHICS_ENGINE_INCLUDE_DIRECTORY}/${header}")
    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "Public header '${header}' is missing")
    endif ()

    # EXISTS is insufficient on case-insensitive filesystems: compare every component
    # with the spelling returned by the filesystem.
    set(directory "${GRAPHICS_ENGINE_INCLUDE_DIRECTORY}")
    string(REPLACE "/" ";" components "${header}")
    foreach (component IN LISTS components)
        file(GLOB children RELATIVE "${directory}" "${directory}/*")
        list(FIND children "${component}" componentIndex)
        if (componentIndex EQUAL -1)
            message(FATAL_ERROR "Public header path has incorrect case: '${header}'")
        endif ()
        set(directory "${directory}/${component}")
    endforeach ()

    file(READ "${path}" content)
    if (content MATCHES "#[ \t]*include[ \t]*[<\"](vulkan/|vulkan\\.h|GLFW/|vk_mem_alloc\\.h|imgui)"
        OR content MATCHES "\\b(Vk[A-Za-z0-9_]*|Vma[A-Za-z0-9_]*|ImGui[A-Za-z0-9_]*|GLFWwindow)\\b")
        message(FATAL_ERROR "Public header '${header}' exposes a backend-native type or include")
    endif ()
endforeach ()
