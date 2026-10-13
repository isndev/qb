cmake_minimum_required(VERSION 3.24)
project(qb_embedded_codec_export LANGUAGES C CXX)

if (NOT DEFINED QB_EMBED_SOURCE OR NOT IS_DIRECTORY "${QB_EMBED_SOURCE}")
    message(FATAL_ERROR "QB_EMBED_SOURCE must name the qb source checkout")
endif ()

set(QB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(QB_INSTALL OFF CACHE BOOL "" FORCE)
set(QB_WITH_QUIC OFF CACHE STRING "" FORCE)
add_subdirectory("${QB_EMBED_SOURCE}" qb)

# The parent calls qb_add_executable after qb's add_subdirectory has returned.
# The same REQUIRES decision must describe the compiled codecs in both scopes.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/codec-probe.cpp" "int main() { return 0; }\n")
qb_add_executable(NAME codec-zstd-probe
        SOURCES "${CMAKE_CURRENT_BINARY_DIR}/codec-probe.cpp" REQUIRES zstd)
qb_add_executable(NAME codec-brotli-probe
        SOURCES "${CMAKE_CURRENT_BINARY_DIR}/codec-probe.cpp" REQUIRES brotli)

foreach (_codec IN ITEMS ZSTD BROTLI)
    string(TOLOWER "${_codec}" _name)
    if (QB_WITH_${_codec})
        if (NOT QB_HAS_${_codec} OR NOT TARGET codec-${_name}-probe)
            message(FATAL_ERROR "enabled ${_name} was omitted from the parent scope")
        endif ()
    elseif (QB_HAS_${_codec} OR TARGET codec-${_name}-probe)
        message(FATAL_ERROR "disabled ${_name} leaked into the parent scope")
    endif ()
endforeach ()
