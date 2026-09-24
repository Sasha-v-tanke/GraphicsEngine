include_guard(GLOBAL)

macro(INIT_WINDOW_ENGINE engine)
    if (GRAPHICS_ENGINE_BUILD_${engine})
        target_compile_definitions(
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            PRIVATE
            GRAPHICS_ENGINE_WINDOW_HAS_${engine}
        )

        PRIVATE_DEPENDS(
            ${engine}
        )

        RECURSE(
            "${engine}"
        )
    endif ()
endmacro()
