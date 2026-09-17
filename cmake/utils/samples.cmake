include_guard(GLOBAL)

macro(SAMPLE name)
    set(sampleTarget "GraphicsEngineSample${name}")

    add_executable(
        ${sampleTarget}
    )

    _GRAPHICS_ENGINE_APPLY_PROJECT_OPTIONS(
        ${sampleTarget}
    )

    target_compile_features(
        ${sampleTarget}
        PRIVATE
        cxx_std_23
    )

    set(
        GRAPHICS_ENGINE_CURRENT_CONTEXT
        "SAMPLE"
    )

    set(
        GRAPHICS_ENGINE_CURRENT_TARGET
        "${sampleTarget}"
    )

    _GRAPHICS_ENGINE_ADD_LOCAL_HEADERS()
endmacro()
