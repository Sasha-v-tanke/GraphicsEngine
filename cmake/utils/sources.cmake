include_guard(GLOBAL)

macro(SOURCES)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "SOURCES"
    )

    foreach (file IN ITEMS ${ARGN})
        target_sources(
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/${file}"
        )
    endforeach ()
endmacro()
