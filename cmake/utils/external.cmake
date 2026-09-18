include_guard(GLOBAL)

function(include_external Name)
    include(
        "${CMAKE_CURRENT_LIST_DIR}/${Name}/${Name}.cmake"
    )
endfunction()
