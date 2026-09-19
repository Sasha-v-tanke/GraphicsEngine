include_guard(GLOBAL)

macro(MODULE name)
    set(
        moduleTarget
        "GraphicsEngine${name}"
    )

    if (TARGET "${moduleTarget}")
        message(FATAL_ERROR
            "MODULE: target '${moduleTarget}' already exists"
        )
    endif ()

    add_library(
        ${moduleTarget}
        STATIC
    )

    _GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS(
        ${moduleTarget}
    )

    add_library(
        GraphicsEngine::${name}
        ALIAS
        ${moduleTarget}
    )

    target_compile_features(
        ${moduleTarget}
        PUBLIC
        cxx_std_23
    )

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "MODULE"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_MODULE
        "${name}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${moduleTarget}"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()

    target_include_directories(
        ${GRAPHICS_ENGINE_CURRENT_TARGET}
        PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/GraphicsEngine>
    )

    set_target_properties(
        ${moduleTarget}
        PROPERTIES
        EXPORT_NAME "_${name}"
        GRAPHICS_ENGINE_MODULE TRUE
    )
endmacro()


macro(SUBMODULE)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "SUBMODULE"
    )

    if (NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "MODULE")
        message(FATAL_ERROR
            "SUBMODULE: current context is not a production module"
        )
    endif ()

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()

macro(PRIVATE_SUBMODULE)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "PRIVATE_SUBMODULE"
    )

    if (NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "MODULE")
        message(FATAL_ERROR
            "PRIVATE_SUBMODULE: current context is not a production module"
        )
    endif ()

    set(
        savedContext
        "${GRAPHICS_ENGINE_CURRENT_CONTEXT}"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "PRIVATE_SUBMODULE"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "${savedContext}"
    )
endmacro()
