include_guard(GLOBAL)

include(CMakePackageConfigHelpers)


# =============================================================================
# Package graph
# =============================================================================

function(
    _GRAPHICS_ENGINE_COLLECT_PACKAGE_GRAPH
    rootTarget
    collectStaticPrivateDependencies
    output
)
    set(pendingTargets
        ${rootTarget}
    )

    set(collectedTargets)

    while (pendingTargets)
        list(
            POP_FRONT
            pendingTargets
            currentTarget
        )

        list(
            FIND
            collectedTargets
            "${currentTarget}"
            targetIndex
        )

        if (NOT targetIndex EQUAL -1)
            continue()
        endif ()

        if (NOT TARGET "${currentTarget}")
            message(FATAL_ERROR
                "Package dependency '${currentTarget}' is not a CMake target"
            )
        endif ()

        list(APPEND
            collectedTargets
            "${currentTarget}"
        )

        get_target_property(
            publicDependencies
            "${currentTarget}"
            GRAPHICS_ENGINE_PUBLIC_DEPENDENCIES
        )

        if (publicDependencies)
            list(APPEND
                pendingTargets
                ${publicDependencies}
            )
        endif ()

        if (collectStaticPrivateDependencies)
            get_target_property(
                targetType
                "${currentTarget}"
                TYPE
            )
        endif ()

        if (
            collectStaticPrivateDependencies
            AND targetType STREQUAL "STATIC_LIBRARY"
        )
            get_target_property(
                privateDependencies
                "${currentTarget}"
                GRAPHICS_ENGINE_PRIVATE_DEPENDENCIES
            )

            if (privateDependencies)
                list(APPEND
                    pendingTargets
                    ${privateDependencies}
                )
            endif ()
        endif ()
    endwhile ()

    set(
        ${output}
        "${collectedTargets}"
        PARENT_SCOPE
    )
endfunction()


# =============================================================================
# Package root
# =============================================================================

get_property(
    packageRoot
    GLOBAL
    PROPERTY GRAPHICS_ENGINE_PACKAGE_ROOT
)

if (NOT packageRoot)
    message(FATAL_ERROR
        "GraphicsEngine package root is not defined"
    )
endif ()


# =============================================================================
# API graph
# =============================================================================

_GRAPHICS_ENGINE_COLLECT_PACKAGE_GRAPH(
    "${packageRoot}"
    FALSE
    graphicsEngineApiTargets
)


# =============================================================================
# Link graph
# =============================================================================

_GRAPHICS_ENGINE_COLLECT_PACKAGE_GRAPH(
    "${packageRoot}"
    TRUE
    graphicsEngineLinkTargets
)


# =============================================================================
# Install targets
# =============================================================================

set(
    graphicsEngineInstallTargets
    GraphicsEnginePackage
)

foreach (target IN LISTS graphicsEngineLinkTargets)
    get_target_property(
        imported
        "${target}"
        IMPORTED
    )

    if (imported)
        message(FATAL_ERROR
            "Package dependency '${target}' is an imported target. "
            "External dependencies must use a GraphicsEngine wrapper target."
        )
    endif ()

    get_target_property(
        module
        "${target}"
        GRAPHICS_ENGINE_MODULE
    )

    get_target_property(
        exportable
        "${target}"
        GRAPHICS_ENGINE_EXPORTABLE
    )

    if (
        NOT module
        AND NOT exportable
    )
        message(FATAL_ERROR
            "Target '${target}' is required by GraphicsEngine package "
            "but is not exportable"
        )
    endif ()

    list(APPEND
        graphicsEngineInstallTargets
        "${target}"
    )
endforeach ()

list(
    REMOVE_DUPLICATES
    graphicsEngineInstallTargets
)


# =============================================================================
# Package dependencies
# =============================================================================

set(graphicsEnginePackageDependencies)

foreach (target IN LISTS graphicsEngineLinkTargets)
    get_target_property(
        packageDependencyCount
        "${target}"
        GRAPHICS_ENGINE_PACKAGE_DEPENDENCY_COUNT
    )

    if (NOT packageDependencyCount)
        continue()
    endif ()

    foreach (dependencyIndex RANGE 1 "${packageDependencyCount}")
        get_target_property(
            packageDependency
            "${target}"
            "GRAPHICS_ENGINE_PACKAGE_DEPENDENCY_${dependencyIndex}"
        )

        if (packageDependency)
            list(
                JOIN
                packageDependency
                " "
                packageDependencyArguments
            )

            list(APPEND
                graphicsEnginePackageDependencies
                "${packageDependencyArguments}"
            )
        endif ()
    endforeach ()
endforeach ()

list(
    REMOVE_DUPLICATES
    graphicsEnginePackageDependencies
)

set(GRAPHICS_ENGINE_FIND_DEPENDENCIES)

foreach (
    dependency
    IN LISTS graphicsEnginePackageDependencies
)
    string(APPEND
        GRAPHICS_ENGINE_FIND_DEPENDENCIES
        "find_dependency(${dependency})\n"
    )
endforeach ()


# =============================================================================
# Components
# =============================================================================

get_property(
    GRAPHICS_ENGINE_PACKAGE_COMPONENTS
    GLOBAL
    PROPERTY GRAPHICS_ENGINE_PACKAGE_COMPONENTS
)

if (GRAPHICS_ENGINE_PACKAGE_COMPONENTS)
    list(
        REMOVE_DUPLICATES
        GRAPHICS_ENGINE_PACKAGE_COMPONENTS
    )
endif ()


# =============================================================================
# Headers
# =============================================================================

set(graphicsEngineApiHeaders)

foreach (target IN LISTS graphicsEngineApiTargets)
    get_target_property(
        module
        "${target}"
        GRAPHICS_ENGINE_MODULE
    )

    if (NOT module)
        continue()
    endif ()

    get_target_property(
        apiHeaders
        "${target}"
        GRAPHICS_ENGINE_API_HEADERS
    )

    if (apiHeaders)
        list(APPEND
            graphicsEngineApiHeaders
            ${apiHeaders}
        )
    endif ()
endforeach ()

list(
    REMOVE_DUPLICATES
    graphicsEngineApiHeaders
)

foreach (header IN LISTS graphicsEngineApiHeaders)
    file(
        RELATIVE_PATH
        relativeHeader
        "${PROJECT_SOURCE_DIR}"
        "${header}"
    )

    if (
        relativeHeader STREQUAL ".."
        OR relativeHeader MATCHES "^\\.\\./"
    )
        message(FATAL_ERROR
            "API header '${header}' is outside GraphicsEngine source tree"
        )
    endif ()

    get_filename_component(
        relativeDirectory
        "${relativeHeader}"
        DIRECTORY
    )

    set(
        headerDestination
        "${CMAKE_INSTALL_INCLUDEDIR}/GraphicsEngine"
    )

    if (relativeDirectory)
        string(APPEND
            headerDestination
            "/${relativeDirectory}"
        )
    endif ()

    install(
        FILES
        "${header}"
        DESTINATION
        "${headerDestination}"
    )
endforeach ()


# =============================================================================
# Targets
# =============================================================================

install(
    TARGETS
    ${graphicsEngineInstallTargets}
    EXPORT
    GraphicsEngineTargets
    RUNTIME DESTINATION
    ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION
    ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION
    ${CMAKE_INSTALL_LIBDIR}
)

set(
    graphicsEnginePackageDirectory
    "${CMAKE_INSTALL_LIBDIR}/cmake/GraphicsEngine"
)

install(
    EXPORT
    GraphicsEngineTargets
    FILE
    GraphicsEngineTargets.cmake
    NAMESPACE
    GraphicsEngine::
    DESTINATION
    "${graphicsEnginePackageDirectory}"
)


# =============================================================================
# Package config
# =============================================================================

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/GraphicsEngineConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/GraphicsEngineConfig.cmake"
    INSTALL_DESTINATION
    "${graphicsEnginePackageDirectory}"
)

write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/GraphicsEngineConfigVersion.cmake"
    VERSION
    "${PROJECT_VERSION}"
    COMPATIBILITY
    ExactVersion
)

install(
    FILES
    "${PROJECT_BINARY_DIR}/GraphicsEngineConfig.cmake"
    "${PROJECT_BINARY_DIR}/GraphicsEngineConfigVersion.cmake"
    DESTINATION
    "${graphicsEnginePackageDirectory}"
)
