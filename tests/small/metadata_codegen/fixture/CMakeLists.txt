# Shared by the source build and the installed-package consumer.
if (TARGET GraphicsEngineCommon)
    set(metadataDependency GraphicsEngineCommon)
else ()
    set(metadataDependency GraphicsEngine::GraphicsEngine)
endif ()

add_library(GraphicsEngineMetadataGameplay STATIC gameplay.cpp)
add_library(GraphicsEngineMetadataAssets STATIC assets.cpp)
foreach (metadataTarget IN ITEMS GraphicsEngineMetadataGameplay GraphicsEngineMetadataAssets)
    target_compile_features(${metadataTarget} PUBLIC cxx_std_23)
    target_include_directories(${metadataTarget} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${metadataTarget} PUBLIC ${metadataDependency})
endforeach ()
GRAPHICS_ENGINE_GENERATE_METADATA(
    TARGET GraphicsEngineMetadataGameplay
    FUNCTION NMetadataFixture::RegisterGameplay
    SCHEMAS gameplay.meta.json
)
GRAPHICS_ENGINE_GENERATE_METADATA(
    TARGET GraphicsEngineMetadataAssets
    FUNCTION NMetadataFixture::RegisterAssets
    SCHEMAS assets.meta.json
)

add_executable(GraphicsEngineMetadataConsumer main.cpp)
target_link_libraries(GraphicsEngineMetadataConsumer PRIVATE GraphicsEngineMetadataGameplay GraphicsEngineMetadataAssets)
add_test(NAME GraphicsEngineMetadataConsumerRun COMMAND GraphicsEngineMetadataConsumer)
set_tests_properties(GraphicsEngineMetadataConsumerRun PROPERTIES LABELS "small;cpp;metadata")
