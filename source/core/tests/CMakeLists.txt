
add_subdirectory(system)
if (${QB_PREFIX_UPPER}_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()