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
        "/bigobj"           # Raise the COFF section-count limit (see below)
    )
    # /bigobj is not optional for this codebase. A COFF object file carries a 16-bit section
    # count (65280 usable); heavy template + coroutine translation units approach it on their
    # own, and AddressSanitizer pushes them over by emitting per-variable instrumentation
    # metadata sections. Without it:
    #   qbm/http/tests/system/http3/http3-loopback.cpp:
    #     fatal error C1128: number of sections exceeded object file format limit
    # under `cmake --preset sanitize -DQB_SANITIZE=address`. ELF and Mach-O have no comparable
    # limit, so neither the Linux nor the macOS toolchain can ever surface this -- it is a pure
    # COFF/Windows constraint. Applied to EVERY target rather than to the one file that tripped
    # first, because the next heavy TU would simply hit it again. The only cost is slightly
    # larger .obj files; there is no codegen or runtime effect.

    # Warning configuration
    # /W4 enforces four classes this project deliberately does NOT enforce on GCC/clang, where the
    # set is `-Wall -Wextra -Wpedantic -Wno-unused-parameter` (see below). Because CI promotes
    # warnings to errors, leaving them on meant Windows failing the build on rules no other
    # platform applies -- a policy split, not a code defect. Each suppression below names the
    # GCC/clang decision it mirrors, so the two sets stay comparable when either is changed.
    list(APPEND QB_CXX_FLAGS_BASE
        "/W4"               # Warning level 4
        "/wd4251"           # DLL interface (no GCC/clang equivalent)
        "/wd4275"           # non-DLL interface (no GCC/clang equivalent)
        "/wd4996"           # deprecated functions
        "/wd4100"           # unreferenced parameter -- mirrors -Wno-unused-parameter
        "/wd4244"           # narrowing conversion -- GCC/clang do not enable -Wconversion
        "/wd4267"           # size_t narrowing -- same class as 4244
        "/wd4456"           # local hides local     -- GCC/clang do not enable -Wshadow
        "/wd4457"           # local hides parameter -- same class as 4456
        "/wd4458"           # local hides member    -- same class as 4456
        "/wd4459"           # local hides global    -- same class as 4456
        "/wd4324"           # structure padded due to alignas: that IS the point of alignas, and
                            # the framework over-aligns deliberately (EventBucket, mpsc queues,
                            # nanolog Item). No GCC/clang counterpart exists at any -W level.
        "/wd4702"           # unreachable code -- GCC/clang do not enable -Wunreachable-code. MSVC
                            # reports it per TEMPLATE INSTANTIATION (io.h's `if constexpr` disposal
                            # arms), so it fires on code that is live for other instantiations.
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
        # NOTE: /GL is NOT here. Whole-program optimisation belongs to QB_ENABLE_LTO (below),
        # which is OFF by default. Having it unconditionally in the Release set meant MSVC found
        # /GL objects at link time, ABORTED the link and restarted it with /LTCG for every target
        # -- 211 restarts in one CI run -- while QB_ENABLE_LTO advertised the feature as opt-in.
        # Consumers also got LTCG they never asked for, and LTO objects are not portable across
        # compiler versions.
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
        # /GL lives HERE, not in the unconditional Release set: it is what LTO means on MSVC, and
        # /LTCG below is useless without it. Release-scoped so a Debug build is unaffected.
        list(APPEND QB_CXX_FLAGS_RELEASE "/GL")
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

            # ...and it must reach EVERY object in the link, including third-party ones.
            #
            # With /fsanitize=address the Microsoft STL turns on its container annotations and
            # stamps a `detect_mismatch` record into each object file. Linking an object built
            # WITH the flag against one built WITHOUT it is then a hard error, not a degraded
            # build:
            #   gtest.lib(gtest-all.cc.obj) : error LNK2038: mismatch detected for
            #   'annotate_string': value '0' doesn't match value '1' in pipe-allocator.cpp.obj
            #   (same for 'annotate_vector' and 'annotate_optional')
            # QB_SANITIZE_COMPILE_OPTS is applied per target by qb_apply_compiler_flags(), which
            # covers qb's and qbm's own targets but NOT the googletest/gmock targets FetchContent
            # builds -- so every single test executable failed to link and the `sanitize` preset
            # was completely unusable on Windows. clang and gcc link mixed sanitized/unsanitized
            # objects without complaint, which is exactly why neither reference platform could
            # reveal this.
            #
            # Directory-scope so every target created afterwards inherits it -- this include()
            # runs before qbFetchGoogleDeps.cmake, so googletest is covered. Targets that also
            # receive the flag per-target simply get it twice, which MSVC accepts.
            add_compile_options("/fsanitize=address")
            qb_status_message("Sanitizers enabled (MSVC): address (build-wide: MSVC cannot link mixed ASan/non-ASan objects)")

            # Name what was DROPPED, not only what was kept. The `sanitize` preset asks for
            # `address,undefined` and MSVC has no UBSan, so half the request disappears here.
            # The status line above says "address", which IMPLIES it -- and an implication is
            # not a report: GUARDRAILS.md tells an agent to "run sanitize (ASan+UBSan)", so a
            # green Windows run was readable as UBSan coverage that never existed. A leg that
            # quietly drops out while the run still prints green is the exact defect class the
            # verify.sh battery exists for; the same standard belongs here.
            string(REPLACE "," ";" _qb_san_requested "${QB_SANITIZE}")
            list(REMOVE_ITEM _qb_san_requested "address")
            if(_qb_san_requested)
                # ONE argument: qb_warning_message() forwards its ARGN, and CMake joins a
                # multi-argument call with ';' -- which printed ";was honoured as ADDRESS ONLY".
                qb_warning_message("MSVC has no ${_qb_san_requested} sanitizer — QB_SANITIZE='${QB_SANITIZE}' was honoured as ADDRESS ONLY. Do not report this build as covering them.")
            endif()
            unset(_qb_san_requested)
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
        # Give Windows the stack POSIX already gives.
        #
        # A PE image reserves 1 MiB of stack by default; Linux and macOS give 8 MiB, and
        # std::thread inherits the image's reserve, so every VirtualCore worker gets it too.
        # qb's recursion budgets were calibrated on the POSIX figure -- concretely,
        # kJsonMaxNestingDepth = 512 in qb/io/protocol/json.h bounds a recursive-descent parse.
        # Measured max survivable nesting on this toolchain, one process per depth, parsing on a
        # thread created with an explicit reserve:
        #
        #                              1 MiB stack        8 MiB stack
        #   json::parse   (text)       ~32760  (64x)      -            <- text frames are tiny
        #   from_msgpack  /O2          ~2208   (4.3x)     -
        #   from_msgpack  /Od          ~1144   (2.2x)     -
        #   from_msgpack  ASan         ~492    (1.0x)     ~3988 (7.8x)
        #
        # The binary reader's frames are ~30x the text parser's, so at 1 MiB the accepted limit
        # (512) sits exactly ON the cliff (492) under ASan -- which is precisely the
        # `AddressSanitizer: stack-overflow` that killed qb-io-test-unit-json-depth-guard. At
        # 8 MiB the margin is 7.8x, i.e. the margin the constant was chosen for.
        #
        # Reserving is not committing: this costs address space, not RAM. On x64 that is free.
        # Preferred over lowering the depth limit because it changes NO accepted input and fixes
        # the whole class rather than the one path that happened to be measured. INTERFACE so an
        # application linking qb inherits it; MSVC honours the LAST /STACK, so a consumer that
        # wants a different size just passes its own.
        #
        # 64-BIT ONLY, deliberately. A 32-bit process has ~2 GiB of user address space, so an
        # 8 MiB *reserve* per thread caps it at ~256 threads -- an actor framework can plausibly
        # want more, and exhausting address space is a worse failure than a deep parse. qb does
        # support 32-bit (QB_ARCH_32), so on that target the right lever is a lower nesting limit
        # for the binary path, not a bigger stack. Left as a stated gap rather than traded away
        # silently.
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            target_link_options(${target} PRIVATE /STACK:8388608)
            get_target_property(_qb_type ${target} TYPE)
            if(NOT _qb_type STREQUAL "EXECUTABLE")
                target_link_options(${target} INTERFACE /STACK:8388608)
            endif()
        endif()

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
    # Probe AT qb's language level, not at the compiler's default one.
    #
    # qbConfig.cmake only writes the global CMAKE_CXX_STANDARD when qb is top-level (deliberately
    # -- see the long note there about leaking qb's standard into a parent). Nothing else set it,
    # and check_cxx_source_compiles() with no -std= compiles at the COMPILER's default: C++14 on
    # Apple clang / GCC. So in every embedded build -- which includes this superproject and every
    # FetchContent consumer -- six of the seven probes below failed for want of an -std= flag, not
    # for want of the feature:
    #     src.cxx:2:28: error: no member named 'optional' in namespace 'std'
    # The one survivor, QB_HAS_STRING_VIEW (libc++ exposes <string_view> pre-C++17), then reached
    # consumers inside qbTargets.cmake's INTERFACE_COMPILE_DEFINITIONS. Two hosts with different
    # default standards therefore produced packages with different public compile definitions --
    # package nondeterminism, exit 0, and the only trace is an ordinary
    # "-- Performing Test QB_HAS_OPTIONAL - Failed" status line.
    #
    # Function scope, so this does not leak: CMP0067 (NEW under cmake_minimum_required(3.24))
    # makes try_compile honour these.
    set(CMAKE_CXX_STANDARD ${QB_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
