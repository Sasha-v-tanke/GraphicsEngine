include_guard(GLOBAL)

# An opt-in build step, independent of runtime and the future Asset Pipeline cooker.
function(GRAPHICS_ENGINE_GENERATE_METADATA)
    cmake_parse_arguments(PARSE_ARGV 0 metadata "" "TARGET;FUNCTION" "SCHEMAS")
    if (metadata_UNPARSED_ARGUMENTS OR metadata_KEYWORDS_MISSING_VALUES
        OR NOT metadata_TARGET OR NOT metadata_FUNCTION OR NOT metadata_SCHEMAS)
        message(FATAL_ERROR "GRAPHICS_ENGINE_GENERATE_METADATA requires TARGET, FUNCTION and SCHEMAS")
    endif ()
    if (NOT TARGET "${metadata_TARGET}")
        message(FATAL_ERROR "Metadata target '${metadata_TARGET}' does not exist")
    endif ()
    get_target_property(imported "${metadata_TARGET}" IMPORTED)
    get_target_property(alias "${metadata_TARGET}" ALIASED_TARGET)
    get_target_property(type "${metadata_TARGET}" TYPE)
    get_target_property(configured "${metadata_TARGET}" GRAPHICS_ENGINE_METADATA_CONFIGURED)
    if (imported OR alias OR configured OR NOT type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
        message(FATAL_ERROR "Metadata requires an unconfigured, concrete local library/executable target")
    endif ()
    if (NOT metadata_TARGET MATCHES "^[A-Za-z_][A-Za-z_0-9.-]*$")
        message(FATAL_ERROR "Metadata target must have a portable identifier")
    endif ()

    # No Python/tool discovery happens until a caller explicitly opts into generation.
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    set(generator "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/metadata_codegen.py")
    if (NOT EXISTS "${generator}")
        set(generator "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/metadata_codegen/generate.py")
    endif ()
    set(schemaArguments)
    set(schemas)
    foreach (schema IN LISTS metadata_SCHEMAS)
        cmake_path(ABSOLUTE_PATH schema BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE OUTPUT_VARIABLE absolute)
        list(APPEND schemas "${absolute}")
        list(APPEND schemaArguments --schema "${absolute}")
    endforeach ()
    set(outputDirectory "${CMAKE_CURRENT_BINARY_DIR}/metadata/${metadata_TARGET}")
    set(header "${outputDirectory}/${metadata_TARGET}.gen.h")
    set(source "${outputDirectory}/${metadata_TARGET}.gen.cpp")
    set(manifest "${outputDirectory}/${metadata_TARGET}.manifest.json")
    set(stamp "${outputDirectory}/generation.stamp")
    add_custom_command(
        OUTPUT "${stamp}" "${header}" "${source}" "${manifest}"
        COMMAND "${Python3_EXECUTABLE}" "${generator}" ${schemaArguments}
            --function "${metadata_FUNCTION}" --header "${header}" --source "${source}" --manifest "${manifest}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${generator}" ${schemas}
        VERBATIM
        COMMENT "Generating metadata for ${metadata_TARGET}"
    )
    add_custom_target("${metadata_TARGET}Metadata" DEPENDS "${stamp}")
    add_dependencies("${metadata_TARGET}" "${metadata_TARGET}Metadata")
    set_source_files_properties("${source}" PROPERTIES GENERATED TRUE)
    target_sources("${metadata_TARGET}" PRIVATE "${source}")
    target_include_directories("${metadata_TARGET}" PUBLIC "$<BUILD_INTERFACE:${outputDirectory}>")
    set_property(TARGET "${metadata_TARGET}" PROPERTY GRAPHICS_ENGINE_METADATA_CONFIGURED TRUE)
    set_property(TARGET "${metadata_TARGET}" PROPERTY GRAPHICS_ENGINE_METADATA_MANIFEST "${manifest}")
endfunction()
