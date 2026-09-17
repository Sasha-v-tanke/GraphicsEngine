include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/utils/context.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/utils/target_options.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/utils/dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/utils/sources.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/utils/modules.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/utils/tests.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/utils/benchmarks.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/utils/samples.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/utils/recurse.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/utils/external.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/utils/package_api.cmake")
