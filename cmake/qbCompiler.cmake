#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# -----------------------------------------------------------------------------
# qb Framework - Compiler Configuration Module
#
# This file configures compiler flags, optimizations, and performance settings
# for the qb framework. It provides high-performance compilation settings
# tailored to different platforms and compilers.
# -----------------------------------------------------------------------------

if(QB_COMPILER_INCLUDED)
    return()
endif()
set(QB_COMPILER_INCLUDED TRUE)

# Include required modules
include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

# -----------------------------------------------------------------------------
# Compiler Detection and Information
# -----------------------------------------------------------------------------
qb_debug_message("Configuring compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# Set compiler-specific variables
set(QB_COMPILER_MSVC FALSE)
set(QB_COMPILER_GCC FALSE)
set(QB_COMPILER_CLANG FALSE)
set(QB_COMPILER_INTEL FALSE)

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    set(QB_COMPILER_MSVC TRUE)
    set(QB_COMPILER_NAME "MSVC")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(QB_COMPILER_GCC TRUE)
    set(QB_COMPILER_NAME "GCC")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(QB_COMPILER_CLANG TRUE)
    set(QB_COMPILER_NAME "Clang")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Intel")
    set(QB_COMPILER_INTEL TRUE)
    set(QB_COMPILER_NAME "Intel")
else()
    set(QB_COMPILER_NAME "Unknown")
    qb_warning_message("Unknown compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# -----------------------------------------------------------------------------
# Base Compiler Flags
# -----------------------------------------------------------------------------
# Initialize to an empty STRING (not a bare `set(VAR)`): these are re-published
# to CACHE INTERNAL below, and a bare unset would let the later `list(APPEND)`
# read through to the stale cache and accumulate flags across reconfigures. An
# empty-string normal variable shadows the cache so each configure rebuilds
# cleanly (same idiom as QB_SANITIZE_*/QB_COVERAGE_* opts).
set(QB_CXX_FLAGS_BASE "")
set(QB_CXX_FLAGS_DEBUG "")
set(QB_CXX_FLAGS_RELEASE "")
set(QB_CXX_FLAGS_RELWITHDEBINFO "")
set(QB_CXX_FLAGS_MINSIZEREL "")

# Common base flags for all compilers
if(QB_COMPILER_MSVC)
    # MSVC specific flags
    list(APPEND QB_CXX_FLAGS_BASE
        "/nologo"           # Suppress startup banner
        "/EHsc"             # Enable C++ exceptions
        "/GR"               # Enable RTTI
        "/permissive-"      # Disable permissive mode
        "/Zc:__cplusplus"   # Enable proper __cplusplus macro
        "/Zc:preprocessor"  # Conformant preprocessor (required for __VA_OPT__)
        "/utf-8"            # Use UTF-8 encoding
    )

    # Warning configuration
    list(APPEND QB_CXX_FLAGS_BASE
        "/W4"               # Warning level 4
        "/wd4251"           # Disable DLL interface warnings
        "/wd4275"           # Disable non-DLL interface warnings
        "/wd4996"           # Disable deprecated function warnings
    )
    
    # Debug flags
    list(APPEND QB_CXX_FLAGS_DEBUG
        "/Od"               # Disable optimization
        "/RTC1"             # Runtime checks
        "/MDd"              # Multi-threaded debug DLL
    )
    
    # Release flags
    list(APPEND QB_CXX_FLAGS_RELEASE
        "/O2"               # Maximize speed
        "/Ob2"              # Inline function expansion
        "/Ot"               # Favor fast code
        "/Gy"               # Enable function-level linking
        "/GL"               # Whole program optimization
        "/MD"               # Multi-threaded DLL
        "/DNDEBUG"          # Define NDEBUG
    )
    
    # RelWithDebInfo flags
    list(APPEND QB_CXX_FLAGS_RELWITHDEBINFO
        "/O2"               # Maximize speed
        "/Ob1"              # Inline function expansion
        "/Gy"               # Enable function-level linking
        "/Zi"               # Debug information
        "/MD"               # Multi-threaded DLL
        "/DNDEBUG"          # Define NDEBUG
    )
    
    # MinSizeRel flags
    list(APPEND QB_CXX_FLAGS_MINSIZEREL
        "/O1"               # Minimize size
        "/Ob1"              # Inline function expansion
        "/Gy"               # Enable function-level linking
        "/MD"               # Multi-threaded DLL
        "/DNDEBUG"          # Define NDEBUG
    )

elseif(QB_COMPILER_GCC OR QB_COMPILER_CLANG)
    # GCC and Clang common flags
    list(APPEND QB_CXX_FLAGS_BASE
        "-Wall"                 # Enable all warnings
        "-Wextra"               # Enable extra warnings
        "-Wpedantic"            # Enable pedantic warnings
        "-Wno-unused-parameter" # Disable unused parameter warnings
        "-fPIC"                 # Position independent code
    )
    
    # Platform-specific flags
    if(QB_PLATFORM_LINUX)
        list(APPEND QB_CXX_FLAGS_BASE "-pthread")
    endif()
    
    # Debug flags
    list(APPEND QB_CXX_FLAGS_DEBUG
        "-O0"               # No optimization
        "-g3"               # Full debug information
        "-fstack-protector-strong"  # Stack protection
    )
    
    # Release flags
    list(APPEND QB_CXX_FLAGS_RELEASE
        "-O3"               # Aggressive optimization
        "-DNDEBUG"          # Define NDEBUG
        "-fomit-frame-pointer"  # Omit frame pointer
        "-ffunction-sections"   # Function sections
        "-fdata-sections"       # Data sections
    )
    
    # RelWithDebInfo flags
    list(APPEND QB_CXX_FLAGS_RELWITHDEBINFO
        "-O2"               # Optimize for speed
        "-g"                # Debug information
        "-DNDEBUG"          # Define NDEBUG
        "-ffunction-sections"   # Function sections
        "-fdata-sections"       # Data sections
    )
    
    # MinSizeRel flags
    list(APPEND QB_CXX_FLAGS_MINSIZEREL
        "-Os"               # Optimize for size
        "-DNDEBUG"          # Define NDEBUG
        "-ffunction-sections"   # Function sections
        "-fdata-sections"       # Data sections
    )
    
    # Compiler-specific optimizations
    if(QB_COMPILER_GCC)
        # GCC version-specific flags
        if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "9.0")
            list(APPEND QB_CXX_FLAGS_BASE "-Wno-error=deprecated-copy")
        endif()
        
    elseif(QB_COMPILER_CLANG)
        # Clang specific flags
        list(APPEND QB_CXX_FLAGS_BASE
            "-Wno-unused-private-field"
            "-Wno-missing-braces"
        )
        
        # Clang version-specific flags
        if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "10.0")
            list(APPEND QB_CXX_FLAGS_BASE "-Wno-deprecated-copy")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# High-Performance Optimizations
# -----------------------------------------------------------------------------
if(QB_ENABLE_OPTIMIZATIONS)
    qb_debug_message("Enabling high-performance optimizations")
    
    if(QB_COMPILER_MSVC)
        # MSVC optimizations (/favor:speed is not a cl.exe switch; /Ot favors fast code)
        list(APPEND QB_CXX_FLAGS_RELEASE
            "/Ot"              # Favor fast code (speed over size within /O2 budget)
        )
        if(QB_ENABLE_FAST_MATH)
            list(APPEND QB_CXX_FLAGS_RELEASE "/fp:fast")
        endif()
        
        # ISA-specific intrinsics only when targeting the host CPU
        if(QB_ENABLE_NATIVE_ARCH AND QB_ARCH_64)
            list(APPEND QB_CXX_FLAGS_RELEASE "/arch:AVX2")
        endif()
        
    elseif(QB_COMPILER_GCC OR QB_COMPILER_CLANG)
        # GCC/Clang optimizations
        list(APPEND QB_CXX_FLAGS_RELEASE
            "-funroll-loops"        # Unroll loops
            "-ftree-vectorize"      # Tree vectorization
        )
        if(QB_ENABLE_FAST_MATH)
            list(APPEND QB_CXX_FLAGS_RELEASE
                "-ffast-math"           # Fast math (breaks IEEE-754)
                "-fno-signed-zeros"     # No signed zeros
                "-fno-trapping-math"    # No trapping math
            )
        endif()
        
        # Architecture-specific optimizations
        if(QB_ENABLE_NATIVE_ARCH)
            # Tune for the build-host CPU. Verify support before using it: older
            # Apple Clang rejects -march=native on arm64, where -mcpu=native is the
            # accepted spelling. Fall back gracefully so the build never breaks.
            check_cxx_compiler_flag("-march=native" QB_HAS_MARCH_NATIVE)
            if(QB_HAS_MARCH_NATIVE)
                list(APPEND QB_CXX_FLAGS_RELEASE "-march=native")
            else()
                check_cxx_compiler_flag("-mcpu=native" QB_HAS_MCPU_NATIVE)
                if(QB_HAS_MCPU_NATIVE)
                    list(APPEND QB_CXX_FLAGS_RELEASE "-mcpu=native")
                else()
                    qb_warning_message("QB_ENABLE_NATIVE_ARCH on, but neither -march=native nor -mcpu=native is supported; using compiler default target")
                endif()
            endif()
        elseif(QB_ARCH_ARM64 AND NOT APPLE)
            # Generic ARMv8 baseline for non-Apple ARM64 (e.g. Linux servers).
            # On Apple Silicon, do NOT force -march=armv8-a: the toolchain already
            # targets the native apple-mN CPU, and forcing the generic baseline
            # downgrades codegen (loses LSE atomics and other extensions).
            list(APPEND QB_CXX_FLAGS_RELEASE "-march=armv8-a")
        elseif(QB_ARCH_64 AND NOT QB_ARCH_ARM)
            # Portable x86-64 baseline (deliberately conservative for distributable
            # binaries). Use QB_ENABLE_NATIVE_ARCH=ON to target the host CPU.
            list(APPEND QB_CXX_FLAGS_RELEASE "-march=x86-64")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# Link Time Optimization (LTO)
# -----------------------------------------------------------------------------
if(QB_ENABLE_LTO)
    qb_debug_message("Enabling Link Time Optimization")
    
    if(QB_COMPILER_MSVC)
        # MSVC LTO is enabled with /GL flag (already added above)
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /LTCG")
        set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG")
        set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS_RELEASE} /LTCG")
        
    elseif(QB_COMPILER_GCC OR QB_COMPILER_CLANG)
        # GCC/Clang LTO
        check_cxx_compiler_flag("-flto" QB_HAS_LTO_FLAG)
        if(QB_HAS_LTO_FLAG)
            list(APPEND QB_CXX_FLAGS_RELEASE "-flto")
            set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -flto")
            set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} -flto")
            if(QB_COMPILER_GCC)
                list(APPEND QB_CXX_FLAGS_RELEASE "-fuse-linker-plugin")
            endif()
        else()
            qb_warning_message("LTO requested but not supported by compiler")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# Profiling Support
# -----------------------------------------------------------------------------
if(QB_WITH_PROFILING)
    qb_debug_message("Enabling profiling support")
    
    if(QB_COMPILER_GCC OR QB_COMPILER_CLANG)
        list(APPEND QB_CXX_FLAGS_BASE
            "-pg"               # Enable profiling
            "-fno-omit-frame-pointer"  # Keep frame pointer for profiling
        )
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pg")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -pg")
    endif()
endif()

# -----------------------------------------------------------------------------
# Sanitizers (QB_SANITIZE)
# -----------------------------------------------------------------------------
# QB_SANITIZE is a comma-separated list (e.g. "address,undefined", "thread", "memory",
# "leak"). The flags are applied to EVERY qb / qbm / test target and their link step
# via qb_apply_compiler_flags() / qb_apply_linker_flags() — not just to Debug — so the
# whole instrumented set is consistent regardless of CMAKE_BUILD_TYPE.
set(QB_SANITIZE_COMPILE_OPTS "")
set(QB_SANITIZE_LINK_OPTS "")
if(QB_SANITIZE)
    if(QB_WITH_PROFILING)
        qb_warning_message("QB_SANITIZE='${QB_SANITIZE}' is incompatible with QB_WITH_PROFILING (tcmalloc/gperftools intercept the same hooks); disable one")
    endif()

    if(QB_COMPILER_GCC OR QB_COMPILER_CLANG)
        list(APPEND QB_SANITIZE_COMPILE_OPTS
            "-fsanitize=${QB_SANITIZE}"
            "-fno-omit-frame-pointer"   # readable stack traces
            "-fno-sanitize-recover=all" # abort on first error (CI-friendly)
            "-g"                        # symbolized reports
        )
        list(APPEND QB_SANITIZE_LINK_OPTS "-fsanitize=${QB_SANITIZE}")
        qb_status_message("Sanitizers enabled: ${QB_SANITIZE}")
    elseif(QB_COMPILER_MSVC)
        # MSVC only ships AddressSanitizer.
        if(QB_SANITIZE MATCHES "address")
            list(APPEND QB_SANITIZE_COMPILE_OPTS "/fsanitize=address")
            qb_status_message("Sanitizers enabled (MSVC): address")
        else()
            qb_warning_message("MSVC only supports /fsanitize=address; ignoring QB_SANITIZE='${QB_SANITIZE}'")
        endif()
    endif()
endif()

# -----------------------------------------------------------------------------
# Coverage instrumentation
# -----------------------------------------------------------------------------
# Coverage must be applied per target. Setting global CMAKE_*_FLAGS after the
# source tree has already created targets does not retrofit existing targets.
set(QB_COVERAGE_COMPILE_OPTS "")
set(QB_COVERAGE_LINK_OPTS "")
if(QB_BUILD_COVERAGE)
    if(WIN32)
        qb_warning_message("QB_BUILD_COVERAGE is not supported on Windows")
    elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        qb_warning_message("QB_BUILD_COVERAGE is intended for Debug builds")
    elseif(QB_COMPILER_CLANG)
        # clang: LLVM source-based coverage (precise region/line mapping, native
        # `llvm-cov` reporting). Report via llvm-profdata + llvm-cov (see the root
        # CMakeLists coverage section). This is the right path on macOS, where gcc's
        # gcov cannot read clang .gcno.
        list(APPEND QB_COVERAGE_COMPILE_OPTS
            "-g"
            "-O0"
            "-fprofile-instr-generate"
            "-fcoverage-mapping"
        )
        list(APPEND QB_COVERAGE_LINK_OPTS "-fprofile-instr-generate")
        set(QB_COVERAGE_KIND "llvm" CACHE INTERNAL "coverage instrumentation kind")
        qb_status_message("Coverage instrumentation enabled (LLVM source-based)")
    elseif(QB_COMPILER_GCC)
        # gcc: gcov-style instrumentation (the reference toolchain; matches CI).
        list(APPEND QB_COVERAGE_COMPILE_OPTS
            "-g"
            "-O0"
            "-fprofile-arcs"
            "-ftest-coverage"
        )
        list(APPEND QB_COVERAGE_LINK_OPTS "--coverage")
        set(QB_COVERAGE_KIND "gcov" CACHE INTERNAL "coverage instrumentation kind")
        qb_status_message("Coverage instrumentation enabled (gcov)")
    endif()
endif()

# -----------------------------------------------------------------------------
# Thread Support
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)
if(NOT CMAKE_USE_PTHREADS_INIT AND NOT WIN32)
    qb_error_message("pthreads not found but required for qb framework")
endif()

# -----------------------------------------------------------------------------
# Helper Functions
# -----------------------------------------------------------------------------
function(qb_apply_compiler_flags target)
    # Apply base flags
    target_compile_options(${target} PRIVATE ${QB_CXX_FLAGS_BASE})
    
    # Apply configuration-specific flags
    if(QB_CXX_FLAGS_DEBUG)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:${QB_CXX_FLAGS_DEBUG}>)
    endif()
    if(QB_CXX_FLAGS_RELEASE)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:${QB_CXX_FLAGS_RELEASE}>)
    endif()
    if(QB_CXX_FLAGS_RELWITHDEBINFO)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo>:${QB_CXX_FLAGS_RELWITHDEBINFO}>)
    endif()
    if(QB_CXX_FLAGS_MINSIZEREL)
        target_compile_options(${target} PRIVATE $<$<CONFIG:MinSizeRel>:${QB_CXX_FLAGS_MINSIZEREL}>)
    endif()

    # Sanitizer flags last so -fno-omit-frame-pointer wins over Release's
    # -fomit-frame-pointer on the command line.
    if(QB_SANITIZE_COMPILE_OPTS)
        target_compile_options(${target} PRIVATE ${QB_SANITIZE_COMPILE_OPTS})
    endif()

    if(QB_COVERAGE_COMPILE_OPTS)
        target_compile_options(${target} PRIVATE ${QB_COVERAGE_COMPILE_OPTS})
    endif()

    # Link threads if needed
    if(CMAKE_USE_PTHREADS_INIT)
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endif()
endfunction()

function(qb_apply_linker_flags target)
    # Sanitizer runtime must be on the link line of every instrumented target.
    if(QB_SANITIZE_LINK_OPTS)
        target_link_options(${target} PRIVATE ${QB_SANITIZE_LINK_OPTS})
    endif()

    if(QB_COVERAGE_LINK_OPTS)
        target_link_options(${target} PRIVATE ${QB_COVERAGE_LINK_OPTS})
    endif()

    if(QB_COMPILER_MSVC)
        # MSVC linker optimizations
        target_link_options(${target} PRIVATE 
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:RelWithDebInfo>:/OPT:REF>
            $<$<CONFIG:MinSizeRel>:/OPT:REF>
        )
        
    elseif(QB_PLATFORM_MACOS AND (QB_COMPILER_GCC OR QB_COMPILER_CLANG))
        # macOS/Apple ld64 linker optimizations
        target_link_options(${target} PRIVATE 
            $<$<CONFIG:Release>:-Wl,-dead_strip>
            $<$<CONFIG:RelWithDebInfo>:-Wl,-dead_strip>
            $<$<CONFIG:MinSizeRel>:-Wl,-dead_strip>
        )
        
        # Strip symbols in release builds (macOS style)
        target_link_options(${target} PRIVATE 
            $<$<CONFIG:Release>:-Wl,-x>
            $<$<CONFIG:MinSizeRel>:-Wl,-x>
        )
        
    elseif(QB_PLATFORM_LINUX AND (QB_COMPILER_GCC OR QB_COMPILER_CLANG))
        # Linux/GNU ld linker optimizations
        target_link_options(${target} PRIVATE 
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:RelWithDebInfo>:-Wl,--gc-sections>
            $<$<CONFIG:MinSizeRel>:-Wl,--gc-sections>
        )
        
        # Strip debug symbols in release builds (GNU style)
        target_link_options(${target} PRIVATE 
            $<$<CONFIG:Release>:-Wl,--strip-all>
            $<$<CONFIG:MinSizeRel>:-Wl,--strip-all>
        )
    endif()
endfunction()

# -----------------------------------------------------------------------------
# Feature Detection
# -----------------------------------------------------------------------------
function(qb_check_cpp_features)
    # Check for C++17 features (guaranteed by the C++20 baseline)
    check_cxx_source_compiles(
        "#include <optional>
         int main() { std::optional<int> opt; return 0; }"
        QB_HAS_OPTIONAL
    )

    check_cxx_source_compiles(
        "#include <string_view>
         int main() { std::string_view sv; return 0; }"
        QB_HAS_STRING_VIEW
    )

    check_cxx_source_compiles(
        "#include <variant>
         int main() { std::variant<int, double> v; return 0; }"
        QB_HAS_VARIANT
    )

    check_cxx_source_compiles(
        "#include <concepts>
         template<typename T>
         concept Integral = std::is_integral_v<T>;
         int main() { return 0; }"
        QB_HAS_CONCEPTS
    )

    # Check for C++23 features
    check_cxx_source_compiles(
        "#include <expected>
         int main() { std::expected<int, int> e; return 0; }"
        QB_HAS_EXPECTED
    )

    check_cxx_source_compiles(
        "#include <format>
         int main() { std::format_args args; return 0; }"
        QB_HAS_FORMAT
    )

    check_cxx_source_compiles(
        "#include <print>
         int main() { std::print(\"Hello\"); return 0; }"
        QB_HAS_PRINT
    )

    # Set compile definitions based on feature availability
    if(QB_HAS_OPTIONAL)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_OPTIONAL=1")
    endif()
    if(QB_HAS_STRING_VIEW)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_STRING_VIEW=1")
    endif()
    if(QB_HAS_VARIANT)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_VARIANT=1")
    endif()
    if(QB_HAS_CONCEPTS)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_CONCEPTS=1")
    endif()
    if(QB_HAS_EXPECTED)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_EXPECTED=1")
    endif()
    if(QB_HAS_FORMAT)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_FORMAT=1")
    endif()
    if(QB_HAS_PRINT)
        list(APPEND QB_COMPILE_DEFINITIONS "QB_HAS_PRINT=1")
    endif()

    # Update parent scope
    set(QB_COMPILE_DEFINITIONS ${QB_COMPILE_DEFINITIONS} PARENT_SCOPE)
endfunction()

# Run feature detection
qb_check_cpp_features()

# Suppress Windows.h min/max macros — added to QB_COMPILE_DEFINITIONS so they
# propagate as PUBLIC compile definitions to every target that links against qb,
# including third-party modules (qbm, examples, tests). This guarantees
# NOMINMAX is on the compiler command line before any header is parsed.
if(QB_COMPILER_MSVC)
    list(APPEND QB_COMPILE_DEFINITIONS "NOMINMAX" "WIN32_LEAN_AND_MEAN")
endif()

# -----------------------------------------------------------------------------
# Compiler Configuration Summary
# -----------------------------------------------------------------------------
function(qb_print_compiler_info)
    qb_debug_message("Compiler Configuration:")
    qb_debug_message("  Compiler: ${QB_COMPILER_NAME} ${CMAKE_CXX_COMPILER_VERSION}")
    qb_debug_message("  Base flags: ${QB_CXX_FLAGS_BASE}")
    qb_debug_message("  Release flags: ${QB_CXX_FLAGS_RELEASE}")
    qb_debug_message("  Debug flags: ${QB_CXX_FLAGS_DEBUG}")
    qb_debug_message("  Optimizations: ${QB_ENABLE_OPTIMIZATIONS}")
    qb_debug_message("  LTO: ${QB_ENABLE_LTO}")
    qb_debug_message("  Native arch: ${QB_ENABLE_NATIVE_ARCH}")
endfunction()

# -----------------------------------------------------------------------------
# Utility Functions for Special Cases
# -----------------------------------------------------------------------------
function(config_compiler_with_no_warning)
    # Temporarily disable warnings for problematic external modules
    if(QB_COMPILER_MSVC)
        add_compile_options(/w)
    elseif(QB_COMPILER_GCC OR QB_COMPILER_CLANG)
        add_compile_options(-w)
    endif()
    qb_debug_message("Disabled warnings for external modules")
endfunction()

function(config_compiler_and_linker)
    # This function is kept for compatibility but the real configuration
    # is now handled automatically by qb_apply_compiler_flags()
    qb_debug_message("Compiler configuration handled automatically")
endfunction()

# -----------------------------------------------------------------------------
# Publish compiler state to CACHE INTERNAL (cross-scope visibility)
# -----------------------------------------------------------------------------
# qb_apply_compiler_flags() / qb_apply_linker_flags() are invoked from foreign
# directory scopes (qbm/* modules, examples — added by the top-level project, not by
# qb). Plain directory-scoped variables are invisible there, which previously left
# qbm targets without qb's warning/optimization flags (no -march=native, -Wall, etc.).
# Freezing the finalized flag set into the cache makes it visible everywhere.
foreach(_v
    QB_COMPILER_MSVC QB_COMPILER_GCC QB_COMPILER_CLANG QB_COMPILER_INTEL QB_COMPILER_NAME
    QB_CXX_FLAGS_BASE QB_CXX_FLAGS_DEBUG QB_CXX_FLAGS_RELEASE
    QB_CXX_FLAGS_RELWITHDEBINFO QB_CXX_FLAGS_MINSIZEREL
    QB_SANITIZE_COMPILE_OPTS QB_SANITIZE_LINK_OPTS
    QB_COVERAGE_COMPILE_OPTS QB_COVERAGE_LINK_OPTS)
    set(${_v} "${${_v}}" CACHE INTERNAL "")
endforeach()

# Mark compiler configuration as loaded
set(QB_COMPILER_LOADED TRUE CACHE INTERNAL "qb compiler configuration loaded")
