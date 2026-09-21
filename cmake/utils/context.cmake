include_guard(GLOBAL)

macro(_GRAPHICS_ENGINE_REQUIRE_CONTEXT commandName)
    if (NOT DEFINED GRAPHICS_ENGINE_CURRENT_TARGET)
        message(FATAL_ERROR
            "${commandName}: no active module context"
        )
    endif ()
endmacro()


macro(_GRAPHICS_ENGINE_ADD_LOCAL_HEADERS)
    _GRAPHICS_ENGINE_REQUIRE_CONTEXT(
        "_GRAPHICS_ENGINE_ADD_LOCAL_HEADERS"
    )

    file(GLOB localHeaders CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/*.h"
    )

    if (localHeaders)
        target_sources(
            ${GRAPHICS_ENGINE_CURRENT_TARGET}
            PRIVATE
            ${localHeaders}
        )
    endif ()
endmacro()
