include_guard(GLOBAL)

macro(_GRAPHICS_ENGINE_RESOLVE_DEPENDENCY output dependency)
    if ("${dependency}" MATCHES "::")
        set(
            ${output}
            "${dependency}"
        )
    elseif (TARGET "${dependency}")
        set(
            ${output}
            "${dependency}"
        )
    else ()
        set(
            ${output}
            "GraphicsEngine::${dependency}"
        )
    endif ()
endmacro()

macro(_GRAPHICS_ENGINE_CANONICAL_TARGET output target)
    if (TARGET "${target}")
        get_target_property(
            aliasedTarget
            "${target}"
            ALIASED_TARGET
        )

        if (aliasedTarget)
            set(
                ${output}
                "${aliasedTarget}"
            )
        else ()
            set(
                ${output}
                "${target}"
            )
        endif ()
    else ()
        set(
            ${output}
            "${target}"
        )
    endif ()
endmacro()

macro(PUBLIC_DEPENDS)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "PUBLIC_DEPENDS"
    )

    foreach (dependency IN ITEMS ${ARGN})
        _GRAPHICS_ENGINE_RESOLVE_DEPENDENCY(
            resolvedDependency
            "${dependency}"
        )

        target_link_libraries(
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            PUBLIC
            ${resolvedDependency}
        )

        _GRAPHICS_ENGINE_CANONICAL_TARGET(
            packageDependency
            "${resolvedDependency}"
        )

        set_property(
            TARGET ${GRAPHICS_ENGINE_CURRENT_TARGET}
            APPEND
            PROPERTY GRAPHICS_ENGINE_PUBLIC_DEPENDENCIES
            ${packageDependency}
        )
    endforeach ()
endmacro()


macro(PRIVATE_DEPENDS)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "PRIVATE_DEPENDS"
    )

    foreach (dependency IN ITEMS ${ARGN})
        _GRAPHICS_ENGINE_RESOLVE_DEPENDENCY(
            resolvedDependency
            "${dependency}"
        )

        _GRAPHICS_ENGINE_CANONICAL_TARGET(
            packageDependency
            "${resolvedDependency}"
        )

        target_link_libraries(
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            PRIVATE
            "$<BUILD_LOCAL_INTERFACE:${resolvedDependency}>"
        )

        get_target_property(
            currentTargetType
            "${GRAPHICS_ENGINE_CURRENT_TARGET}"
            TYPE
        )

        if (currentTargetType STREQUAL "STATIC_LIBRARY")
            target_link_libraries(
                ${GRAPHICS_ENGINE_CURRENT_TARGET}
                INTERFACE
                "$<LINK_ONLY:${resolvedDependency}>"
            )
        endif ()

        set_property(
            TARGET ${GRAPHICS_ENGINE_CURRENT_TARGET}
            APPEND
            PROPERTY GRAPHICS_ENGINE_PRIVATE_DEPENDENCIES
            ${packageDependency}
        )
    endforeach ()
endmacro()
