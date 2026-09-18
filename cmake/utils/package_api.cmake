include_guard(GLOBAL)

macro(PACKAGE_ROOT name)
    get_property(
        packageRootSet
        GLOBAL
        PROPERTY GRAPHICS_ENGINE_PACKAGE_ROOT
        SET
    )

    if (packageRootSet)
        message(FATAL_ERROR
            "PACKAGE_ROOT: package root already defined"
        )
    endif ()

    set(
        packageRootTarget
        "GraphicsEngine${name}"
    )

    if (NOT TARGET "${packageRootTarget}")
        message(FATAL_ERROR
            "PACKAGE_ROOT: module '${name}' does not exist"
        )
    endif ()

    set_property(
        GLOBAL
        PROPERTY GRAPHICS_ENGINE_PACKAGE_ROOT
        "${packageRootTarget}"
    )

    add_library(
        GraphicsEnginePackage
        INTERFACE
    )

    add_library(
        GraphicsEngine::GraphicsEngine
        ALIAS
        GraphicsEnginePackage
    )

    set_target_properties(
        GraphicsEnginePackage
        PROPERTIES
        EXPORT_NAME GraphicsEngine
    )

    target_link_libraries(
        GraphicsEnginePackage
        INTERFACE
        ${packageRootTarget}
    )
endmacro()

macro(PACKAGE_COMPONENT name)
    set_property(
        GLOBAL
        APPEND
        PROPERTY GRAPHICS_ENGINE_PACKAGE_COMPONENTS
        "${name}"
    )
endmacro()

macro(PACKAGE_DEPENDS target)
    if (NOT TARGET "${target}")
        message(FATAL_ERROR
            "PACKAGE_DEPENDS: target '${target}' does not exist"
        )
    endif ()

    get_target_property(
        packageDependencyCount
        "${target}"
        GRAPHICS_ENGINE_PACKAGE_DEPENDENCY_COUNT
    )

    if (NOT packageDependencyCount)
        set(packageDependencyCount 0)
    endif ()

    math(
        EXPR
        packageDependencyCount
        "${packageDependencyCount} + 1"
    )

    set_property(
        TARGET "${target}"
        PROPERTY
        GRAPHICS_ENGINE_PACKAGE_DEPENDENCY_COUNT
        "${packageDependencyCount}"
    )

    set_property(
        TARGET "${target}"
        PROPERTY
        "GRAPHICS_ENGINE_PACKAGE_DEPENDENCY_${packageDependencyCount}"
        ${ARGN}
    )
endmacro()
