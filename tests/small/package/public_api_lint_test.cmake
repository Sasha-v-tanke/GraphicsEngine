if (NOT DEFINED GRAPHICS_ENGINE_SOURCE_DIR)
    message(FATAL_ERROR "GRAPHICS_ENGINE_SOURCE_DIR is required")
endif ()

if (NOT DEFINED GRAPHICS_ENGINE_BINARY_DIR)
    message(FATAL_ERROR "GRAPHICS_ENGINE_BINARY_DIR is required")
endif ()

set(fixtureRoot "${GRAPHICS_ENGINE_BINARY_DIR}/public-api-lint-fixtures")
file(REMOVE_RECURSE "${fixtureRoot}")
file(MAKE_DIRECTORY "${fixtureRoot}")

function(_GRAPHICS_ENGINE_REJECTS_HEADER name contents)
    set(fixtureDirectory "${fixtureRoot}/${name}")
    set(includeDirectory "${fixtureDirectory}/include")
    file(MAKE_DIRECTORY "${includeDirectory}/GraphicsEngine/fixture")
    file(WRITE "${fixtureDirectory}/headers.cmake"
        "set(GRAPHICS_ENGINE_PUBLIC_HEADERS \"GraphicsEngine/fixture/public.h\")\n")
    file(WRITE "${includeDirectory}/GraphicsEngine/fixture/public.h" "${contents}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DGRAPHICS_ENGINE_PUBLIC_HEADER_MANIFEST=${fixtureDirectory}/headers.cmake"
            "-DGRAPHICS_ENGINE_INCLUDE_DIRECTORY=${includeDirectory}"
            -P "${GRAPHICS_ENGINE_SOURCE_DIR}/cmake/public_api_lint.cmake"
        RESULT_VARIABLE result
        OUTPUT_QUIET
        ERROR_QUIET
    )

    if (result EQUAL 0)
        message(FATAL_ERROR "Public API lint accepted fixture '${name}'")
    endif ()
endfunction()

_GRAPHICS_ENGINE_REJECTS_HEADER(vk_device "struct X { VkDevice device; };\n")
_GRAPHICS_ENGINE_REJECTS_HEADER(vma_allocation "struct X { VmaAllocation allocation; };\n")
_GRAPHICS_ENGINE_REJECTS_HEADER(glfw_window "struct X { GLFWwindow* window; };\n")
_GRAPHICS_ENGINE_REJECTS_HEADER(imgui_context "struct X { ImGuiContext* context; };\n")
_GRAPHICS_ENGINE_REJECTS_HEADER(vulkan_include "#include <vulkan/vulkan.h>\n")

set(missingDirectory "${fixtureRoot}/missing-header")
file(MAKE_DIRECTORY "${missingDirectory}/include/GraphicsEngine/fixture")
file(WRITE "${missingDirectory}/headers.cmake"
    "set(GRAPHICS_ENGINE_PUBLIC_HEADERS \"GraphicsEngine/fixture/missing.h\")\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DGRAPHICS_ENGINE_PUBLIC_HEADER_MANIFEST=${missingDirectory}/headers.cmake"
        "-DGRAPHICS_ENGINE_INCLUDE_DIRECTORY=${missingDirectory}/include"
        -P "${GRAPHICS_ENGINE_SOURCE_DIR}/cmake/public_api_lint.cmake"
    RESULT_VARIABLE missingResult
    OUTPUT_QUIET
    ERROR_QUIET
)
if (missingResult EQUAL 0)
    message(FATAL_ERROR "Public API lint accepted a missing header")
endif ()

set(hiddenDirectory "${fixtureRoot}/hidden-dependency")
file(MAKE_DIRECTORY "${hiddenDirectory}/include/GraphicsEngine/fixture")
file(WRITE "${hiddenDirectory}/include/GraphicsEngine/fixture/public.h"
    "#include <GraphicsEngine/fixture/hidden.h>\n")
file(WRITE "${hiddenDirectory}/probe.cpp"
    "#include <GraphicsEngine/fixture/public.h>\n")
file(WRITE "${hiddenDirectory}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.31)
project(GraphicsEngineHiddenPublicDependency LANGUAGES CXX)
add_library(probe OBJECT probe.cpp)
target_include_directories(probe PRIVATE "${PUBLIC_HEADER_INCLUDE_DIRECTORY}")
]=])
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${hiddenDirectory}" -B "${hiddenDirectory}/build"
        "-DPUBLIC_HEADER_INCLUDE_DIRECTORY=${hiddenDirectory}/include"
    RESULT_VARIABLE configureResult
    OUTPUT_QUIET
    ERROR_QUIET
)
if (NOT configureResult EQUAL 0)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${hiddenDirectory}/build"
        RESULT_VARIABLE buildResult
        OUTPUT_QUIET
        ERROR_QUIET
    )
else ()
    set(buildResult 1)
endif ()
if (buildResult EQUAL 0)
    message(FATAL_ERROR "Isolated public-header compilation accepted a hidden dependency")
endif ()
