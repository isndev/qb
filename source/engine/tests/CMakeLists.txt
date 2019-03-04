
add_subdirectory(system)
if (${CUBE_PREFIX_UPPER}_BUILD_BENCHMARK)
    add_subdirectory(benchmark)
endif()