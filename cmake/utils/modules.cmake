include_guard(GLOBAL)

function(_GRAPHICS_ENGINE_PREPARE_BUILD_INCLUDE_TREE)
    get_property(
        prepared
        GLOBAL
        PROPERTY GRAPHICS_ENGINE_BUILD_INCLUDE_TREE_PREPARED
    )

    if (prepared)
        return()
    endif ()

    set(
        buildIncludeRoot
        "${GraphicsEngine_BINARY_DIR}/include"
    )

    set(
        buildIncludeNamespace
        "${buildIncludeRoot}/GraphicsEngine"
    )

    file(
        MAKE_DIRECTORY
        "${buildIncludeRoot}"
    )

    if (
        EXISTS "${buildIncludeNamespace}"
        OR IS_SYMLINK "${buildIncludeNamespace}"
    )
        file(
            REMOVE_RECURSE
            "${buildIncludeNamespace}"
        )
    endif ()

    cmake_policy(PUSH)

    if (POLICY CMP0205)
        cmake_policy(SET CMP0205 NEW)
    endif ()

    file(
        CREATE_LINK
        "${GraphicsEngine_SOURCE_DIR}"
        "${buildIncludeNamespace}"
        SYMBOLIC
        COPY_ON_ERROR
    )

    cmake_policy(POP)

    set_property(
        GLOBAL
        PROPERTY GRAPHICS_ENGINE_BUILD_INCLUDE_TREE_PREPARED TRUE
    )
endfunction()

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

    file(
        REAL_PATH
        "${CMAKE_CURRENT_LIST_DIR}"
        moduleDirectory
    )

    set_property(
        TARGET ${moduleTarget}
        PROPERTY
        GRAPHICS_ENGINE_MODULE_DIRECTORY
        "${moduleDirectory}"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
    _GRAPHICS_ENGINE_PREPARE_BUILD_INCLUDE_TREE()

    target_include_directories(
        ${GRAPHICS_ENGINE_CURRENT_TARGET}
        PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${GraphicsEngine_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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


macro(API)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "API"
    )

    if (NOT GRAPHICS_ENGINE_CURRENT_CONTEXT STREQUAL "MODULE")
        message(FATAL_ERROR
            "API: current context is not a production module"
        )
    endif ()

    get_target_property(
        moduleDirectory
        ${GRAPHICS_ENGINE_CURRENT_TARGET}
        GRAPHICS_ENGINE_MODULE_DIRECTORY
    )

    file(
        REAL_PATH
        "${CMAKE_CURRENT_LIST_DIR}"
        currentListDirectory
    )

    if (NOT currentListDirectory STREQUAL moduleDirectory)
        message(FATAL_ERROR
            "API: may only be declared in the root module entry file"
        )
    endif ()

    foreach (file IN ITEMS ${ARGN})
        if (IS_ABSOLUTE "${file}")
            message(FATAL_ERROR
                "API: '${file}' must be relative to the module directory"
            )
        endif ()

        file(
            REAL_PATH
            "${moduleDirectory}/${file}"
            header
        )

        if (NOT EXISTS "${header}")
            message(FATAL_ERROR
                "API: header '${file}' does not exist"
            )
        endif ()

        file(
            RELATIVE_PATH
            relativeHeader
            "${moduleDirectory}"
            "${header}"
        )

        if (
            relativeHeader STREQUAL ".."
            OR relativeHeader MATCHES "^\\.\\./"
        )
            message(FATAL_ERROR
                "API: header '${file}' is outside the module directory"
            )
        endif ()

        get_target_property(
            apiHeaders
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            GRAPHICS_ENGINE_API_HEADERS
        )

        if (apiHeaders)
            list(
                FIND
                apiHeaders
                "${header}"
                headerIndex
            )

            if (NOT headerIndex EQUAL -1)
                message(FATAL_ERROR
                    "API: header '${file}' is already declared"
                )
            endif ()
        endif ()

        set_property(
            TARGET ${GRAPHICS_ENGINE_CURRENT_TARGET}
            APPEND
            PROPERTY GRAPHICS_ENGINE_API_HEADERS
            "${header}"
        )
    endforeach ()
endmacro()
