include_guard(GLOBAL)

macro(_GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS target)
    target_link_libraries(
        ${target}
        PRIVATE
        "$<BUILD_INTERFACE:GraphicsEngine::Warnings>"
        "$<BUILD_INTERFACE:GraphicsEngine::Sanitizers>"
    )
endmacro()
