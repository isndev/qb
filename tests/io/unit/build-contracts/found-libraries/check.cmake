# qb-io-test-unit-found-libraries (Huly QB-1192). The searches qbDependencies.cmake runs for Argon2, zlib, zstd and brotli
# (cmake/qbFoundLibraries.cmake) on libraries planted in a scratch root, through the fixture beside this file:
# - a library found is an input of the configure, its include directory is not (the header it holds is); a path set by
#   hand that never existed -- a library a superbuild builds later -- is kept and said, never an input (POSIX, where
#   /usr/local is a default prefix); nothing found from the start stops nothing (every platform).
# - POSIX, with Ninja and with Unix Makefiles where their tools are found: an update that puts each library elsewhere and
#   removes the version found re-runs the configure at `cmake --build`, which finds each where the update put it, and
#   the next build has no work; a library removed and found nowhere else stops the configure, naming each entry still
#   missing and the remedy, and keeps stopping until it is back -- each of the four; a header removed from an include
#   directory that stays (a shared prefix) is searched again too; a path set by hand is no input, and a configure keeps
#   it and says so; a package never had (a stray header) stops nothing; zlib's debug library gone loses nothing, and
#   FindZLIB's own ZLIB_LIBRARY override clears a stop.
#   Where pkg-config is found, the Homebrew shape: versioned kegs that pkg-config alone names, its answer asked again.
#
# - the configure that registered this test, when it passes LIVE_SEARCHED: each package it searched went through its
#   wrapper, a path its search produced is recorded, and each file it found -- a library, an include directory's
#   header -- is an input of the configure (LIVE_<entry> = <value>|<value before the search>|<record>).
#
#   cmake -DQB_CMAKE_MODULE_DIR=<qb>/cmake -DWORK_DIR=<scratch directory> -DGENERATOR=<this build's>
#         [-DPLATFORM=..] [-DTOOLSET=..] [-DMAKE_PROGRAM=..]
#         [-DLIVE_SEARCHED=<argon2|zlib|zstd|brotli> -DLIVE_INPUTS=<'|'-separated> -DLIVE_<entry>=..] -P check.cmake

cmake_minimum_required(VERSION 3.24)

foreach(_required QB_CMAKE_MODULE_DIR WORK_DIR GENERATOR)
    if(NOT ${_required})
        message(FATAL_ERROR "found-libraries: ${_required} is required")
    endif()
endforeach()
set(_fixture "${CMAKE_CURRENT_LIST_DIR}/fixture")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
# Nothing of the environment the suite runs in may steer the fixture's searches: the root hints of the modules it runs
# -- ARGON2_ROOT and ZLIB_ROOT, searched too; OPENSSL_ROOT_DIR, where FindOpenSSL then searches alone (3.28 and 3.31
# read), never finding the OpenSSL planted under /usr/local -- and a toolchain file, which CMake applies to every fresh
# build tree from 3.21 on (a cross one turns each fixture configure into a cross build).
foreach(_env ARGON2_ROOT ZLIB_ROOT OPENSSL_ROOT_DIR CMAKE_TOOLCHAIN_FILE)
    unset(ENV{${_env}})
endforeach()
set(_env "") # an environment for the fixture's configures and builds: `cmake -E env` arguments

set(_posix FALSE)
if(NOT WIN32 AND EXISTS /bin/sh)
    set(_posix TRUE)
endif()
# The FindOpenSSL the fixture runs -- qb's module directory first, then this CMake's: whether it searches zlib itself
# for a static OpenSSL (CMake 3.26 on). Its pkg-config call before 3.31, made whether pkg-config was found or not, the
# fixture answers with a command that does nothing when pkg-config is off.
set(_find_openssl "${QB_CMAKE_MODULE_DIR}/FindOpenSSL.cmake")
if(NOT EXISTS "${_find_openssl}")
    set(_find_openssl "${CMAKE_ROOT}/Modules/FindOpenSSL.cmake")
endif()
file(READ "${_find_openssl}" _find_openssl)
set(_openssl_finds_zlib FALSE)
if(_find_openssl MATCHES "find_package\\(ZLIB\\)")
    set(_openssl_finds_zlib TRUE)
endif()
# ... and whether a static OpenSSL whose pkg-config names -pthread makes it call find_package(Threads) on any UNIX, the
# branch it takes on Linux without one.
set(_openssl_finds_threads FALSE)
if(_find_openssl MATCHES "\"-pthread\"[^\n]*\n[^\n]*\n[^\n]*find_package\\(Threads\\)")
    set(_openssl_finds_threads TRUE)
endif()
# A library's file name as find_library() looks for it on this host.
function(library_file name out_var)
    if(WIN32)
        set(${out_var} "${name}.lib" PARENT_SCOPE)
    elseif(APPLE)
        set(${out_var} "lib${name}.dylib" PARENT_SCOPE)
    else()
        set(${out_var} "lib${name}.so" PARENT_SCOPE)
    endif()
endfunction()
function(escape text out_var) # <text> as a regex that matches it
    string(REGEX REPLACE "([][+.*()^$?|\\\\{}])" "\\\\\\1" _escaped "${text}")
    set(${out_var} "${_escaped}" PARENT_SCOPE)
endfunction()
# plant(<package> <prefix> [NO <library>...] [ALSO <library>...]): the headers and libraries the module of <package>
# looks for, under <prefix> -- openssl and ngtcp2 too, for the consumer of an installed qb.
function(plant package prefix)
    cmake_parse_arguments(_plant "" "" "NO;ALSO" ${ARGN})
    if(package STREQUAL "argon2")
        set(_headers "argon2.h")
        set(_libraries argon2)
    elseif(package STREQUAL "zlib")
        set(_headers "zlib.h")
        set(_libraries z)
    elseif(package STREQUAL "zstd")
        set(_headers "zstd.h")
        set(_libraries zstd)
    elseif(package STREQUAL "openssl")
        set(_headers "openssl/ssl.h" "openssl/opensslv.h")
        set(_libraries ssl crypto)
    elseif(package STREQUAL "ngtcp2")
        set(_headers "ngtcp2/ngtcp2.h" "ngtcp2/ngtcp2_crypto.h" "ngtcp2/ngtcp2_crypto_ossl.h")
        set(_libraries ngtcp2 ngtcp2_crypto_ossl)
    else()
        set(_headers "brotli/encode.h")
        set(_libraries brotlienc brotlidec brotlicommon)
    endif()
    foreach(_header IN LISTS _headers)
        file(WRITE "${prefix}/include/${_header}"
             "#define ZLIB_VERSION \"1.3.1\"\n#define ZSTD_VERSION_MAJOR 1\n#define ZSTD_VERSION_MINOR 5\n"
             "#define ZSTD_VERSION_RELEASE 7\n# define OPENSSL_VERSION_STR \"3.0.0\"\n")
    endforeach()
    foreach(_library IN LISTS _libraries _plant_ALSO)
        if(NOT _library IN_LIST _plant_NO)
            library_file(${_library} _file)
            file(WRITE "${prefix}/lib/${_file}" "")
        endif()
    endforeach()
endfunction()

# configure_fixture(<case> OK|FAIL <-D...>... [MUST <regex>...] [NOT <regex>...] [HAS <line regex>...]): configures the
# fixture in <case>/build, then checks its exit, its output (MUST hold each regex, NOT none) and verdict.txt (HAS a line
# matching each). The output's lines are joined first: CMake wraps a message's text.
set(_cases 0)
set(_failures "")
set(_fixture_args "-DQB_CMAKE_MODULE_DIR=${QB_CMAKE_MODULE_DIR}") # what <_fixture> is configured with
function(configure_fixture name expect)
    set(_args -S "${_fixture}" -B "${WORK_DIR}/${name}/build" -G "${GENERATOR}" -Wno-dev ${_fixture_args}
              "-DRESULT_DIR=${WORK_DIR}/${name}" "-DPLANT_ROOT=${WORK_DIR}/${name}/root")
    if(PLATFORM)
        list(APPEND _args -A "${PLATFORM}")
    endif()
    if(TOOLSET)
        list(APPEND _args -T "${TOOLSET}")
    endif()
    if(MAKE_PROGRAM)
        list(APPEND _args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
    endif()
    set(_kind DEFINE)
    set(_checks "")
    foreach(_arg IN LISTS ARGN)
        if(_arg STREQUAL "MUST" OR _arg STREQUAL "NOT" OR _arg STREQUAL "HAS")
            set(_kind "${_arg}")
        elseif(_kind STREQUAL "DEFINE")
            list(APPEND _args "${_arg}")
        else()
            list(APPEND _checks "${_kind}" "${_arg}")
        endif()
    endforeach()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${_env} "${CMAKE_COMMAND}" ${_args}
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    string(REGEX REPLACE "[ \t\r]*\n[ \t]*" " " _flat "${_out}")
    set(_verdict "")
    if(EXISTS "${WORK_DIR}/${name}/verdict.txt")
        file(READ "${WORK_DIR}/${name}/verdict.txt" _verdict)
    endif()
    math(EXPR _n "${_cases} + 1")
    set(_cases ${_n} PARENT_SCOPE)
    set(_wrong "")
    if(expect STREQUAL "OK" AND NOT _rc EQUAL 0)
        string(APPEND _wrong "\n    the configure failed (${_rc})")
    elseif(expect STREQUAL "FAIL" AND _rc EQUAL 0)
        string(APPEND _wrong "\n    the configure succeeded")
    endif()
    list(LENGTH _checks _length)
    set(_i 0)
    while(_i LESS _length)
        list(GET _checks ${_i} _kind)
        math(EXPR _i "${_i} + 1")
        list(GET _checks ${_i} _check)
        math(EXPR _i "${_i} + 1")
        if(_kind STREQUAL "MUST" AND NOT "${_flat}" MATCHES "${_check}")
            string(APPEND _wrong "\n    no '${_check}' in its output")
        elseif(_kind STREQUAL "NOT" AND "${_flat}" MATCHES "${_check}")
            string(APPEND _wrong "\n    '${_check}' in its output")
        elseif(_kind STREQUAL "HAS" AND NOT "\n${_verdict}" MATCHES "\n${_check}\n")
            string(APPEND _wrong "\n    verdict.txt has no line '${_check}'")
        endif()
    endwhile()
    if(_wrong)
        set(_failures "${_failures}\n  ${name}:${_wrong}\n    verdict.txt: ${_verdict}\n    output: ${_flat}" PARENT_SCOPE)
    endif()
endfunction()

# Found: each library an input of the configure, each include directory not -- the header it holds is. POSIX, where
# /usr/local is one of the prefixes every search looks in (re-rooted under the planted root).
if(_posix)
    set(_name "found")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    foreach(_package argon2 zlib zstd brotli)
        plant(${_package} "${_root}/usr/local")
    endforeach()
    set(_inputs "")
    foreach(_file argon2 z zstd brotlienc brotlidec brotlicommon)
        library_file(${_file} _file)
        list(APPEND _inputs "INPUTS=.*\\|${_root_regex}/usr/local/lib/${_file}\\|.*")
    endforeach()
    foreach(_header argon2.h zlib.h zstd.h brotli/encode.h)
        escape("${_header}" _header)
        list(APPEND _inputs "INPUTS=.*\\|${_root_regex}/usr/local/include/${_header}\\|.*")
    endforeach()
    configure_fixture(${_name} OK "-DFIND=argon2|zlib|zstd|brotli"
                      NOT "CMake Warning" "searched again" "is gone"
                      HAS "ARGON2=TRUE" "ZLIB=TRUE" "ZSTD=TRUE" "BROTLI=TRUE" ${_inputs})
    configure_fixture(${_name} OK "-DFIND=argon2|zlib|zstd|brotli"
                      NOT "INPUTS=.*${_root_regex}/usr/local/include\\|")
    # Never there: a library a superbuild builds after this configure is kept as given, and is no input.
    set(_name "not-yet-built")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant(zstd "${_root}/usr/local" NO zstd)
    library_file(zstd _file)
    configure_fixture(${_name} OK "-DFIND=zstd" "-DZSTD_LIBRARY=${_root}/later/${_file}"
                      MUST "ZSTD_LIBRARY \\(${_root_regex}/later/[^)]*\\) does not exist; this build directory's search did not find it .*: kept as given"
                      NOT "searched again" "is gone"
                      HAS "ZSTD=TRUE" "ZSTD_LIBRARY=${_root_regex}/later/.*" "INPUTS=\\|${_root_regex}/usr/local/include/zstd\\.h\\|")
    # ... a Windows-shaped path too: its backslashes are text, said as given.
    configure_fixture(${_name} OK "-DFIND=zstd" "-DZSTD_LIBRARY=${_root}/later\\deps\\${_file}"
                      MUST "ZSTD_LIBRARY \\(${_root_regex}/later.deps.[^)]*\\) does not exist; [^:]*: kept as given"
                      NOT "searched again" "is gone" "CMake Error")
endif()
# Nothing from the start, every platform: nothing was had, nothing stops.
file(MAKE_DIRECTORY "${WORK_DIR}/absent/root")
configure_fixture(absent OK "-DFIND=argon2|zlib|zstd|brotli"
                  NOT "is gone" "searched again"
                  HAS "ARGON2=FALSE" "ZLIB=FALSE" "ZSTD=FALSE" "BROTLI=FALSE" "INPUTS=\\|\\|")
if(_failures)
    message(FATAL_ERROR "found-libraries: ${_cases} configures, wrong ones:${_failures}")
endif()

# `cmake --build` re-runs the configure when a library found is removed, with Ninja and Unix Makefiles where found.
set(_builds 0)
set(_generators "")
if(_posix)
    find_program(_ninja NAMES ninja ninja-build)
    if("${GENERATOR}" STREQUAL "Ninja" AND MAKE_PROGRAM)
        set(_ninja "${MAKE_PROGRAM}")
    endif()
    if(_ninja)
        list(APPEND _generators "Ninja")
    endif()
    find_program(_make NAMES make gmake)
    if(_make)
        list(APPEND _generators "Unix Makefiles")
    endif()
    if(NOT _generators)
        message(FATAL_ERROR "found-libraries: neither ninja nor make found on a POSIX host")
    endif()
endif()
# build_fixture(<case> <what changed> <configures wanted so far> [<line of verdict.txt wanted>...]
#               [OUT <regex its output must hold>...] [NOTOUT <regex it must not>...]); <configures wanted> FAILS: the
#               build must fail.
function(build_fixture name label want_count)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${_env} "${CMAKE_COMMAND}" --build "${WORK_DIR}/${name}/build"
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    string(REGEX REPLACE "[ \t\r]*\n[ \t]*" " " _flat "${_out}")
    # A configure that never reached the end writes neither file: a case that failed, said as one, not a test stopped
    # here with the cases after it left unrun.
    set(_lines "")
    set(_verdict "")
    if(EXISTS "${WORK_DIR}/${name}/configures.txt")
        file(STRINGS "${WORK_DIR}/${name}/configures.txt" _lines)
    endif()
    if(EXISTS "${WORK_DIR}/${name}/verdict.txt")
        file(READ "${WORK_DIR}/${name}/verdict.txt" _verdict)
    endif()
    list(LENGTH _lines _count)
    math(EXPR _n "${_builds} + 1")
    set(_builds ${_n} PARENT_SCOPE)
    set(_wrong "")
    if(want_count STREQUAL "FAILS")
        if(_rc EQUAL 0)
            string(APPEND _wrong " the build succeeded;")
        endif()
    elseif(NOT _rc EQUAL 0)
        string(APPEND _wrong " build exit ${_rc};")
    elseif(NOT _count EQUAL want_count)
        string(APPEND _wrong " ${_count} configures, want ${want_count};")
    endif()
    set(_kind LINE)
    foreach(_check IN LISTS ARGN)
        if(_check STREQUAL "OUT" OR _check STREQUAL "NOTOUT")
            set(_kind "${_check}")
        elseif(_kind STREQUAL "OUT" AND NOT "${_flat}" MATCHES "${_check}")
            string(APPEND _wrong " no '${_check}' in its output;")
        elseif(_kind STREQUAL "NOTOUT" AND "${_flat}" MATCHES "${_check}")
            string(APPEND _wrong " '${_check}' in its output;")
        elseif(_kind STREQUAL "LINE" AND NOT "\n${_verdict}" MATCHES "\n${_check}\n")
            string(APPEND _wrong " no line '${_check}';")
        endif()
    endforeach()
    if(_wrong)
        string(APPEND _failures "\n  ${name}, ${label}:${_wrong}\n    verdict.txt: ${_verdict}\n    output: ${_out}")
        set(_failures "${_failures}" PARENT_SCOPE)
    endif()
endfunction()
function(use_generator generator) # sets GENERATOR and MAKE_PROGRAM in the caller for <generator>
    set(GENERATOR "${generator}" PARENT_SCOPE)
    if(generator STREQUAL "Ninja")
        set(MAKE_PROGRAM "${_ninja}" PARENT_SCOPE)
    else()
        set(MAKE_PROGRAM "" PARENT_SCOPE)
    endif()
endfunction()
set(_all "-DFIND=argon2|zlib|zstd|brotli")
foreach(_generator IN LISTS _generators)
    use_generator("${_generator}")
    string(REPLACE " " "-" _name "rebuild-${_generator}")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    # Each in a versioned keg of its own the search was pointed at, as Homebrew's Cellar.
    foreach(_package argon2 zlib zstd brotli)
        plant(${_package} "${_root}/Cellar/${_package}/1")
    endforeach()
    # The four kegs as one CMAKE_PREFIX_PATH, through an initial cache: a list on the command line would be split.
    file(WRITE "${WORK_DIR}/${_name}/prefixes.cmake"
         "set(CMAKE_PREFIX_PATH \"/Cellar/argon2/1;/Cellar/zlib/1;/Cellar/zstd/1;/Cellar/brotli/1\" CACHE STRING \"\")\n")
    # qb builds zlib from source when a system one is missing (QB_DEPS_FETCH_FALLBACK, ON by default) in a build that
    # installs nothing (QB_INSTALL, ON for a top-level project like this one): the fallback turned off here, in such a
    # build, a zlib lost is lost, and stops the configure like the other three.
    configure_fixture(${_name} OK ${_all} -C "${WORK_DIR}/${_name}/prefixes.cmake" "-DQB_DEPS_FETCH_FALLBACK=OFF"
                      "-DQB_INSTALL=OFF"
                      HAS "ARGON2=TRUE" "ZLIB=TRUE" "ZSTD=TRUE" "BROTLI=TRUE"
                          "ARGON2_LIBRARY=${_root_regex}/Cellar/argon2/1/lib/.*"
                          "ZLIB_LIBRARY_RELEASE=${_root_regex}/Cellar/zlib/1/lib/.*"
                          "ZSTD_LIBRARY=${_root_regex}/Cellar/zstd/1/lib/.*"
                          "BROTLI_COMMON_LIBRARY=${_root_regex}/Cellar/brotli/1/lib/.*")
    build_fixture(${_name} "nothing changed" 1)
    # An update puts each where the default search finds it, and removes the version found: searched again, found there.
    foreach(_package argon2 zlib zstd brotli)
        plant(${_package} "${_root}/usr/local")
        file(REMOVE_RECURSE "${_root}/Cellar/${_package}/1")
    endforeach()
    build_fixture(${_name} "each updated elsewhere, the version found removed" 2
                  "ARGON2=TRUE" "ZLIB=TRUE" "ZSTD=TRUE" "BROTLI=TRUE"
                  "ARGON2_INCLUDE_DIR=${_root_regex}/usr/local/include" "ARGON2_LIBRARY=${_root_regex}/usr/local/lib/.*"
                  "ZLIB_INCLUDE_DIR=${_root_regex}/usr/local/include" "ZLIB_LIBRARY_RELEASE=${_root_regex}/usr/local/lib/.*"
                  "ZSTD_INCLUDE_DIR=${_root_regex}/usr/local/include" "ZSTD_LIBRARY=${_root_regex}/usr/local/lib/.*"
                  "BROTLI_INCLUDE_DIR=${_root_regex}/usr/local/include"
                  "BROTLI_ENCODER_LIBRARY=${_root_regex}/usr/local/lib/.*"
                  "BROTLI_DECODER_LIBRARY=${_root_regex}/usr/local/lib/.*"
                  "BROTLI_COMMON_LIBRARY=${_root_regex}/usr/local/lib/.*"
                  OUT "ARGON2_LIBRARY \\(${_root_regex}/Cellar/argon2/1/lib/[^)]*\\) no longer exists: searched again"
                      "ZLIB_LIBRARY_RELEASE \\(${_root_regex}/Cellar/zlib/1/lib/[^)]*\\) no longer exists: searched again"
                      "ZSTD_LIBRARY \\(${_root_regex}/Cellar/zstd/1/lib/[^)]*\\) no longer exists: searched again"
                      "BROTLI_COMMON_LIBRARY \\(${_root_regex}/Cellar/brotli/1/lib/[^)]*\\) no longer exists: searched again"
                      "ARGON2_INCLUDE_DIR \\(${_root_regex}/Cellar/argon2/1/include\\) no longer exists: searched again")
    build_fixture(${_name} "nothing changed since" 2)
    set(_configures 2)
    # Each removed in turn and found nowhere else: the configure stops, naming what is still missing and the remedy, and
    # keeps stopping until it is back. The include directory stays (a shared prefix): its header is what is gone.
    foreach(_case "argon2;argon2.h;argon2;Argon2;-DCMAKE_DISABLE_FIND_PACKAGE_Argon2=ON to build without it"
                  "zlib;zlib.h;z;zlib;QB_WITH_COMPRESSION=OFF"
                  "zstd;zstd.h;zstd;zstd;-DQB_WITH_ZSTD=OFF"
                  "brotli;brotli/encode.h;brotlienc;brotli;-DQB_WITH_BROTLI=OFF")
        list(GET _case 0 _package)
        list(GET _case 1 _header)
        list(GET _case 2 _library)
        list(GET _case 3 _what)
        list(GET _case 4 _remedy)
        string(TOUPPER "${_package}" _entry)
        set(_include "${_entry}_INCLUDE_DIR")
        library_file(${_library} _file)
        escape("${_header}" _header_regex)
        escape("${_remedy}" _remedy_regex)
        file(REMOVE "${_root}/usr/local/include/${_header}" "${_root}/usr/local/lib/${_file}")
        build_fixture(${_name} "${_package} removed" FAILS
                      OUT "${_include} \\(${_root_regex}/usr/local/include\\) no longer holds ${_header_regex}: searched again"
                          "the ${_what} this build directory found is gone and was found nowhere else -- ${_include} \\(${_root_regex}/usr/local/include\\), [A-Z0-9_]+ \\(${_root_regex}/usr/local/lib/${_file}\\)"
                          "${_remedy_regex}")
        build_fixture(${_name} "built again, nothing done" FAILS
                      OUT "the ${_what} this build directory found is gone and was found nowhere else -- ${_include}")
        plant(${_package} "${_root}/usr/local")
        math(EXPR _configures "${_configures} + 1")
        build_fixture(${_name} "${_package} back" ${_configures} "${_include}=${_root_regex}/usr/local/include")
    endforeach()
    # brotli moved where its common library is missing: every entry is searched again, and the stop names the one
    # found nowhere, not those found again beside it.
    foreach(_library brotlienc brotlidec brotlicommon)
        library_file(${_library} _file)
        file(REMOVE "${_root}/usr/local/lib/${_file}")
    endforeach()
    file(REMOVE_RECURSE "${_root}/usr/local/include/brotli")
    plant(brotli "${_root}/usr" NO brotlicommon)
    library_file(brotlicommon _file)
    build_fixture(${_name} "brotli moved where libbrotlicommon is missing" FAILS
                  OUT "found nowhere else -- BROTLI_COMMON_LIBRARY \\(${_root_regex}/usr/local/lib/${_file}\\): install"
                      "BROTLI_ENCODER_LIBRARY \\(${_root_regex}/usr/local/lib/[^)]*\\) no longer exists: searched again"
                  NOTOUT "found nowhere else -- [^:]*BROTLI_(ENCODER_LIBRARY|DECODER_LIBRARY|INCLUDE_DIR) \\(")
    plant(brotli "${_root}/usr")
    build_fixture(${_name} "libbrotlicommon there too" 7 "BROTLI=TRUE"
                  "BROTLI_COMMON_LIBRARY=${_root_regex}/usr/lib/.*" "BROTLI_ENCODER_LIBRARY=${_root_regex}/usr/lib/.*")
    build_fixture(${_name} "nothing changed since" 7)
endforeach()
if(_generators)
    list(GET _generators 0 _generator)
    use_generator("${_generator}")
    # A path set by hand -- as a superbuild names a library it builds, then removes with its own clean -- is never
    # searched again, and is no input of the configure (a superbuild's own library would re-run it at each of its
    # rebuilds): its removal re-runs nothing, and the next configure keeps it and says how to search again.
    set(_name "set-by-hand")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant(argon2 "${_root}/usr/local")
    plant(argon2 "${_root}/hand")
    library_file(argon2 _file)
    configure_fixture(${_name} OK "-DFIND=argon2" "-DARGON2_LIBRARY=${_root}/hand/lib/${_file}"
                      HAS "ARGON2=TRUE" "INPUTS=\\|${_root_regex}/usr/local/include/argon2\\.h\\|")
    file(REMOVE_RECURSE "${_root}/hand")
    build_fixture(${_name} "the library set by hand removed" 1 "ARGON2_LIBRARY=${_root_regex}/hand/lib/.*")
    configure_fixture(${_name} OK "-DFIND=argon2"
                      MUST "ARGON2_LIBRARY \\(${_root_regex}/hand/lib/[^)]*\\) does not exist; this build directory's search did not find it .*: kept as given -- set it again, or configure with --fresh to search for it"
                      NOT "is gone" HAS "ARGON2_LIBRARY=${_root_regex}/hand/lib/.*")
    # A package lost, then turned off (CMAKE_DISABLE_FIND_PACKAGE_<Package>: find_package() searches nothing): the build
    # goes on without it -- Argon2's way out, which has no switch of its own, and zlib's with its fallback off.
    set(_name "turned-off")
    set(_root "${WORK_DIR}/${_name}/root")
    plant(argon2 "${_root}/usr/local")
    plant(zlib "${_root}/usr/local")
    configure_fixture(${_name} OK "-DFIND=argon2|zlib" "-DQB_DEPS_FETCH_FALLBACK=OFF" "-DQB_INSTALL=OFF"
                      HAS "ARGON2=TRUE" "ZLIB=TRUE")
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} FAIL "-DFIND=argon2|zlib" MUST "the Argon2 this build directory found is gone")
    configure_fixture(${_name} FAIL "-DFIND=argon2|zlib" "-DCMAKE_DISABLE_FIND_PACKAGE_Argon2=ON"
                      MUST "the zlib this build directory found is gone" NOT "the Argon2 this build directory found")
    configure_fixture(${_name} OK "-DFIND=argon2|zlib" "-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON" NOT "is gone"
                      HAS "ARGON2=" "ZLIB=") # nothing searched: <Package>_FOUND is not even set
    build_fixture(${_name} "nothing changed since" 2 "ARGON2=" "ZLIB=")
    # Turned off by the configure that sees the loss: nothing is searched again, nor said.
    set(_name "turned-off-at-once")
    set(_root "${WORK_DIR}/${_name}/root")
    plant(argon2 "${_root}/usr/local")
    configure_fixture(${_name} OK "-DFIND=argon2" HAS "ARGON2=TRUE")
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} OK "-DFIND=argon2" "-DCMAKE_DISABLE_FIND_PACKAGE_Argon2=ON"
                      NOT "is gone" "searched again" HAS "ARGON2=")
    # A package this build directory never had -- a stray header, no library -- stops nothing when that header goes.
    set(_name "never-had")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant(argon2 "${_root}/usr/local" NO argon2)
    configure_fixture(${_name} OK "-DFIND=argon2"
                      HAS "ARGON2=FALSE" "ARGON2_INCLUDE_DIR=${_root_regex}/usr/local/include")
    file(REMOVE "${_root}/usr/local/include/argon2.h")
    build_fixture(${_name} "the stray header removed" 2 "ARGON2=FALSE"
                  OUT "ARGON2_INCLUDE_DIR \\(${_root_regex}/usr/local/include\\) no longer holds argon2\\.h: searched again"
                  NOTOUT "is gone")
    # zlib: a debug library gone beside the release one is searched again and loses nothing; and after a stop, the
    # override FindZLIB itself offers -- ZLIB_LIBRARY set by hand, which it takes without searching -- is enough.
    set(_name "zlib-variants")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant(zlib "${_root}/Cellar/zlib/1" ALSO zd)
    file(WRITE "${WORK_DIR}/${_name}/prefixes.cmake" "set(CMAKE_PREFIX_PATH \"/Cellar/zlib/1\" CACHE STRING \"\")\n")
    configure_fixture(${_name} OK "-DFIND=zlib" -C "${WORK_DIR}/${_name}/prefixes.cmake" "-DQB_DEPS_FETCH_FALLBACK=OFF"
                      "-DQB_INSTALL=OFF" HAS "ZLIB=TRUE" "ZLIB_LIBRARY_DEBUG=${_root_regex}/Cellar/zlib/1/lib/.*")
    library_file(zd _file)
    file(REMOVE "${_root}/Cellar/zlib/1/lib/${_file}")
    build_fixture(${_name} "the debug zlib removed, the release one kept" 2 "ZLIB=TRUE"
                  "ZLIB_LIBRARY_DEBUG=ZLIB_LIBRARY_DEBUG-NOTFOUND"
                  "ZLIB_LIBRARY_RELEASE=${_root_regex}/Cellar/zlib/1/lib/.*"
                  OUT "ZLIB_LIBRARY_DEBUG \\([^)]*\\) no longer exists: searched again" NOTOUT "is gone")
    library_file(z _file)
    file(REMOVE "${_root}/Cellar/zlib/1/lib/${_file}")
    build_fixture(${_name} "the release zlib removed" FAILS
                  OUT "the zlib this build directory found is gone and was found nowhere else -- ZLIB_LIBRARY_RELEASE"
                      "let qb build one from source \\(QB_DEPS_FETCH_FALLBACK=ON, QB_INSTALL=OFF\\)"
                  NOTOUT "QB_WITH_SSL=OFF")
    # zlib built from source instead -- in a build that installs nothing: an installable qb refuses a zlib built from
    # source -- or provided by a parent project: compression is not lost, nothing stops.
    configure_fixture(${_name} FAIL "-DFIND=zlib" "-DQB_DEPS_FETCH_FALLBACK=ON" "-DQB_INSTALL=ON"
                      MUST "the zlib this build directory found is gone")
    configure_fixture(${_name} OK "-DFIND=zlib" "-DQB_DEPS_FETCH_FALLBACK=ON" "-DQB_INSTALL=OFF"
                      NOT "is gone" HAS "ZLIB=FALSE")
    configure_fixture(${_name} OK "-DFIND=zlib" "-DQB_DEPS_FETCH_FALLBACK=ON" "-DQB_INSTALL=OFF" "-DPARENT_ZLIB_TARGET=ON"
                      NOT "is gone" HAS "ZLIB=TRUE") # the parent's, which qb links: nothing built from source
    configure_fixture(${_name} OK "-DFIND=zlib" "-DQB_DEPS_FETCH_FALLBACK=OFF" "-DPARENT_ZLIB_TARGET=ON"
                      NOT "is gone" HAS "ZLIB=TRUE") # ... with no fallback of qb's own
    configure_fixture(${_name} FAIL "-DFIND=zlib" "-DQB_DEPS_FETCH_FALLBACK=OFF" "-DPARENT_ZLIB_TARGET=OFF"
                      MUST "the zlib this build directory found is gone")
    plant(zlib "${_root}/elsewhere")
    configure_fixture(${_name} OK "-DFIND=zlib" "-DZLIB_LIBRARY=${_root}/elsewhere/lib/${_file}"
                      NOT "is gone" HAS "ZLIB=TRUE")
    # A parent's ZLIB::ZLIB beside a zlib never found: nothing lost, so nothing taken -- qb builds its own, as before.
    file(MAKE_DIRECTORY "${WORK_DIR}/parent-zlib-fresh/root")
    configure_fixture(parent-zlib-fresh OK "-DFIND=zlib" "-DPARENT_ZLIB_TARGET=ON" NOT "is gone" HAS "ZLIB=FALSE")
    # zlib around a shared OpenSSL, compression off: nothing searches zlib, so nothing is said, its lost entries
    # forgotten quietly; and a path set by hand that two searches look at is said once.
    if(_posix)
        set(_name "openssl-zlib-quiet")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        file(WRITE "${_root}/usr/local/include/openssl/ssl.h" "")
        file(WRITE "${_root}/usr/local/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"3.0.0\"\n")
        library_file(ssl _file)
        file(WRITE "${_root}/usr/local/lib/${_file}" "")
        library_file(crypto _file)
        file(WRITE "${_root}/usr/local/lib/${_file}" "")
        plant(zlib "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} OK "-DFIND=openssl|zlib" "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1"
                          "-DQB_DEPS_FETCH_FALLBACK=OFF" "-DQB_INSTALL=OFF"
                          HAS "OPENSSL=TRUE" "ZLIB=TRUE" "ZLIB_LIBRARY_RELEASE=${_root_regex}/Cellar/zlib/1/lib/.*")
        file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DQB_WITH_COMPRESSION=OFF"
                          NOT "ZLIB_[A-Z_]+ \\([^)]*\\) [^:]*: searched again"
                          HAS "OPENSSL=TRUE" "ZLIB_LIBRARY_RELEASE=")
        # A path set by hand there is not what a search found -- not recorded -- and kept as given once gone.
        file(WRITE "${_root}/hand/libz.a" "")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DQB_WITH_COMPRESSION=OFF"
                          "-DZLIB_LIBRARY_RELEASE=${_root}/hand/libz.a" NOT "searched again")
        file(REMOVE "${_root}/hand/libz.a")
        plant(zlib "${_root}/Cellar/zlib/1")
        set(_said "ZLIB_LIBRARY_RELEASE \\(${_root_regex}/hand/libz\\.a\\) does not exist")
        configure_fixture(${_name} OK "-DFIND=openssl|zlib" "-DQB_WITH_COMPRESSION=ON"
                          MUST "${_said}" NOT "${_said}.*${_said}" "searched again")
        # OpenSSL and zlib lost at once, compression on: OpenSSL's stop ends the configure before qb's zlib search, so
        # nothing is said of zlib.
        set(_name "openssl-zlib-both-lost")
        set(_root "${WORK_DIR}/${_name}/root")
        file(COPY "${WORK_DIR}/openssl-zlib-quiet/root/usr" DESTINATION "${_root}")
        plant(zlib "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} OK "-DFIND=openssl|zlib" "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1"
                          "-DQB_DEPS_FETCH_FALLBACK=OFF" "-DQB_INSTALL=OFF" HAS "OPENSSL=TRUE" "ZLIB=TRUE")
        file(REMOVE_RECURSE "${_root}/usr" "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} FAIL "-DFIND=openssl|zlib" MUST "the OpenSSL this build directory found is gone"
                          NOT "ZLIB_[A-Z_]+ \\([^)]*\\) [^:]*: searched again")
        # ... nor when Argon2's stop, between OpenSSL's search and zlib's, ends it.
        set(_name "openssl-argon2-zlib-lost")
        set(_root "${WORK_DIR}/${_name}/root")
        file(COPY "${WORK_DIR}/openssl-zlib-quiet/root/usr" DESTINATION "${_root}")
        plant(argon2 "${_root}/Cellar/deps/1")
        plant(zlib "${_root}/Cellar/deps/1")
        configure_fixture(${_name} OK "-DFIND=openssl|argon2|zlib" "-DCMAKE_PREFIX_PATH=/Cellar/deps/1"
                          "-DQB_DEPS_FETCH_FALLBACK=OFF" "-DQB_INSTALL=OFF" HAS "OPENSSL=TRUE" "ARGON2=TRUE" "ZLIB=TRUE")
        file(REMOVE_RECURSE "${_root}/Cellar/deps/1")
        configure_fixture(${_name} FAIL "-DFIND=openssl|argon2|zlib" MUST "the Argon2 this build directory found is gone"
                          NOT "ZLIB_[A-Z_]+ \\([^)]*\\) [^:]*: searched again")
    endif()
    # The Homebrew shape: versioned kegs pkg-config alone names (planted .pc files, PKG_CONFIG_LIBDIR naming them alone).
    # An upgrade into a new keg, the old one removed: found again only if the search asks pkg-config again.
    find_program(_pkg_config NAMES pkg-config pkgconf)
    if(_pkg_config)
        set(_name "pkg-config")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        function(plant_kegs version) # argon2, zstd and brotli in kegs of <version>, and the .pc files that name them
            foreach(_package argon2 zstd brotli)
                plant(${_package} "${_root}/Cellar/${_package}/${version}")
            endforeach()
            foreach(_pc "libargon2;argon2;-largon2" "libzstd;zstd;-lzstd" "libbrotlienc;brotli;-lbrotlienc"
                        "libbrotlidec;brotli;-lbrotlidec" "libbrotlicommon;brotli;-lbrotlicommon")
                list(GET _pc 0 _module)
                list(GET _pc 1 _package)
                list(GET _pc 2 _libs)
                file(WRITE "${_root}/pc/${_module}.pc"
                     "prefix=/Cellar/${_package}/${version}\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\n"
                     "Name: ${_module}\nDescription: planted\nVersion: 1.0.${version}\nLibs: -L\${libdir} ${_libs}\n"
                     "Cflags: -I\${includedir}\n")
            endforeach()
        endfunction()
        plant_kegs(1)
        # A prefix the environment names, as the GitHub macOS runner's CMAKE_PREFIX_PATH names Homebrew's packages: its
        # pkg-config files name kegs outside the root, so a search that heeded them would find nothing.
        set(_decoy "${WORK_DIR}/${_name}/decoy")
        foreach(_pc IN ITEMS libargon2 libzstd libbrotlienc libbrotlidec libbrotlicommon openssl zlib)
            file(WRITE "${_decoy}/lib/pkgconfig/${_pc}.pc"
                 "prefix=/decoy\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\nName: ${_pc}\n"
                 "Description: decoy\nVersion: 9.0.0\nLibs: -L\${libdir}\nCflags: -I\${includedir}\n")
        endforeach()
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR="
                 "CMAKE_PREFIX_PATH=${_decoy}")
        configure_fixture(${_name} OK "-DFIND=argon2|zstd|brotli" "-DUSE_PKG_CONFIG=ON"
                          "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                          HAS "ARGON2=TRUE" "ZSTD=TRUE" "BROTLI=TRUE"
                              "ARGON2_LIBRARY=${_root_regex}/Cellar/argon2/1/lib/.*"
                              "ZSTD_LIBRARY=${_root_regex}/Cellar/zstd/1/lib/.*"
                              "BROTLI_DECODER_LIBRARY=${_root_regex}/Cellar/brotli/1/lib/.*")
        build_fixture(${_name} "nothing changed" 1)
        plant_kegs(2)
        foreach(_package argon2 zstd brotli)
            file(REMOVE_RECURSE "${_root}/Cellar/${_package}/1")
        endforeach()
        build_fixture(${_name} "upgrades pkg-config alone names, the old kegs removed" 2
                      "ARGON2=TRUE" "ZSTD=TRUE" "BROTLI=TRUE"
                      "ARGON2_INCLUDE_DIR=${_root_regex}/Cellar/argon2/2/include"
                      "ARGON2_LIBRARY=${_root_regex}/Cellar/argon2/2/lib/.*"
                      "ZSTD_LIBRARY=${_root_regex}/Cellar/zstd/2/lib/.*"
                      "BROTLI_ENCODER_LIBRARY=${_root_regex}/Cellar/brotli/2/lib/.*"
                      "BROTLI_COMMON_LIBRARY=${_root_regex}/Cellar/brotli/2/lib/.*")
        build_fixture(${_name} "nothing changed since" 2)
        set(_env "")
    endif()
    if(_pkg_config AND NOT _openssl_finds_zlib)
        message(STATUS "found-libraries: FindOpenSSL searches no zlib here: the static OpenSSL cases are skipped")
    elseif(_pkg_config)
        # A static OpenSSL whose pkg-config names -lz: FindOpenSSL searches zlib itself (a static one), before qb's zlib
        # search -- zlib's entries are searched again around it too: a zlib lost still stops the configure.
        set(_name "openssl-zlib")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        file(WRITE "${_root}/usr/local/include/openssl/ssl.h" "")
        file(WRITE "${_root}/usr/local/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"3.0.0\"\n")
        file(WRITE "${_root}/usr/local/lib/libssl.a" "")
        file(WRITE "${_root}/usr/local/lib/libcrypto.a" "")
        file(WRITE "${_root}/pc/openssl.pc"
             "prefix=/usr/local\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\nName: OpenSSL\n"
             "Description: planted\nVersion: 3.0.0\nLibs: -L\${libdir} -lssl -lcrypto\nLibs.private: -lz\n"
             "Cflags: -I\${includedir}\n")
        plant(zlib "${_root}/Cellar/zlib/1")
        file(WRITE "${_root}/Cellar/zlib/1/lib/libz.a" "")
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR="
                 "CMAKE_PREFIX_PATH=${_decoy}")
        configure_fixture(${_name} OK "-DFIND=openssl|zlib" "-DUSE_PKG_CONFIG=ON"
                          "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}" "-DOPENSSL_USE_STATIC_LIBS=ON"
                          "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1"
                          "-DQB_DEPS_FETCH_FALLBACK=OFF" "-DQB_INSTALL=OFF"
                          HAS "OPENSSL=TRUE" "ZLIB=TRUE"
                              "ZLIB_LIBRARY_RELEASE=${_root_regex}/Cellar/zlib/1/lib/libz\\.a")
        file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} FAIL "-DFIND=openssl|zlib"
                          MUST "ZLIB_LIBRARY_RELEASE \\([^)]*\\) no longer exists: searched again"
                               "the zlib this build directory found is gone"
                               "QB_WITH_COMPRESSION=OFF .* with QB_WITH_SSL=OFF: the OpenSSL found links zlib too"
                          NOT "(ZLIB_LIBRARY_RELEASE \\([^)]*\\) no longer exists: searched again).*ZLIB_LIBRARY_RELEASE \\([^)]*\\) no longer exists")
        # With compression on, the zlib qb builds from source would be OpenSSL's too: nothing stops.
        configure_fixture(${_name} OK "-DFIND=openssl|zlib" "-DQB_DEPS_FETCH_FALLBACK=ON" NOT "is gone")
        # Back, then lost and turned off by the configure that sees it: nothing searched again or said, there either.
        plant(zlib "${_root}/Cellar/zlib/1")
        file(WRITE "${_root}/Cellar/zlib/1/lib/libz.a" "")
        configure_fixture(${_name} OK "-DFIND=openssl|zlib" HAS "ZLIB=TRUE")
        file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} OK "-DFIND=openssl|zlib" "-DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON"
                          NOT "is gone" "ZLIB_[A-Z_]+ \\([^)]*\\) [^:]*: searched again" HAS "ZLIB=")
        # Compression off -- no zlib search of qb's own: the zlib that OpenSSL's search found is an input all the same,
        # and its removal re-runs the configure at the next build, which stops on it, found nowhere else (the generate
        # would fail on the ZLIB::ZLIB OpenSSL links, naming neither), until it is found elsewhere.
        set(_name "openssl-zlib-inputs")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        file(COPY "${WORK_DIR}/openssl-zlib/root/usr" "${WORK_DIR}/openssl-zlib/root/pc" DESTINATION "${_root}")
        plant(zlib "${_root}/Cellar/zlib/1")
        file(WRITE "${_root}/Cellar/zlib/1/lib/libz.a" "")
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR="
                 "CMAKE_PREFIX_PATH=${_decoy}")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DUSE_PKG_CONFIG=ON"
                          "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}" "-DOPENSSL_USE_STATIC_LIBS=ON"
                          "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1" "-DQB_WITH_COMPRESSION=OFF"
                          HAS "OPENSSL=TRUE" "INPUTS=.*\\|${_root_regex}/Cellar/zlib/1/lib/libz\\.a\\|.*")
        build_fixture(${_name} "nothing changed" 1)
        file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
        build_fixture(${_name} "the zlib OpenSSL's search found removed" FAILS
                      OUT "ZLIB_LIBRARY_RELEASE \\([^)]*\\) no longer exists: searched again"
                          "the zlib this build directory found is gone and was found nowhere else -- ZLIB_INCLUDE_DIR"
                          "the OpenSSL found links it")
        plant(zlib "${_root}/Cellar/zlib/2")
        file(WRITE "${_root}/Cellar/zlib/2/lib/libz.a" "")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DCMAKE_PREFIX_PATH=/Cellar/zlib/2"
                          NOT "is gone" HAS "OPENSSL=TRUE" "ZLIB_LIBRARY_RELEASE=${_root_regex}/Cellar/zlib/2/lib/libz\\.a")
        # An OpenSSL static by itself (no OPENSSL_USE_STATIC_LIBS), naming -lz among its libraries: FindOpenSSL's zlib
        # search is told by the ZLIB::ZLIB it links, after it -- recorded and an input from the first configure, and
        # searched again at the next ones.
        set(_name "openssl-zlib-linked")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        file(COPY "${WORK_DIR}/openssl-zlib/root/usr" DESTINATION "${_root}")
        file(WRITE "${_root}/pc/openssl.pc"
             "prefix=/usr/local\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\nName: OpenSSL\n"
             "Description: planted\nVersion: 3.0.0\nLibs: -L\${libdir} -lssl -lcrypto -lz\nCflags: -I\${includedir}\n")
        plant(zlib "${_root}/Cellar/zlib/1" ALSO zd)
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR="
                 "CMAKE_PREFIX_PATH=${_decoy}")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DUSE_PKG_CONFIG=ON" "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                          "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1" "-DQB_WITH_COMPRESSION=OFF"
                          HAS "OPENSSL=TRUE" "INPUTS=.*\\|${_root_regex}/Cellar/zlib/1/lib/libz[^|]*\\|.*")
        build_fixture(${_name} "nothing changed" 1)
        # Its debug library removed beside the release one: searched again, zlib found -- nothing stops.
        library_file(zd _file)
        file(REMOVE "${_root}/Cellar/zlib/1/lib/${_file}")
        build_fixture(${_name} "its debug library removed" 2
                      OUT "ZLIB_LIBRARY_DEBUG \\([^)]*\\) no longer exists: searched again" NOTOUT "is gone")
        file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
        build_fixture(${_name} "the zlib it linked removed" FAILS
                      OUT "ZLIB_LIBRARY_RELEASE \\([^)]*\\) no longer exists: searched again"
                          "the zlib this build directory found is gone" "configure with -DQB_WITH_SSL=OFF")
        # The remedy it names: OpenSSL's three entries removed for one that does not link zlib -- nothing stops.
        set(_shared "${_root}/shared")
        file(WRITE "${_shared}/include/openssl/ssl.h" "")
        file(WRITE "${_shared}/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"3.0.0\"\n")
        foreach(_library ssl crypto)
            library_file(${_library} _file)
            file(WRITE "${_shared}/lib/${_file}" "")
        endforeach()
        configure_fixture(${_name} OK "-DFIND=openssl" "-UOPENSSL_INCLUDE_DIR" "-UOPENSSL_SSL_LIBRARY"
                          "-UOPENSSL_CRYPTO_LIBRARY" "-DOPENSSL_ROOT_DIR=/shared"
                          NOT "is gone" HAS "OPENSSL=TRUE" "OPENSSL_LINKS_ZLIB=FALSE")
        # A parent project's ZLIB::ZLIB is the one OpenSSL links then: its zlib lost stops nothing.
        set(_name "openssl-zlib-parent")
        set(_root "${WORK_DIR}/${_name}/root")
        file(COPY "${WORK_DIR}/openssl-zlib/root/usr" "${WORK_DIR}/openssl-zlib-linked/root/pc" DESTINATION "${_root}")
        plant(zlib "${_root}/Cellar/zlib/1")
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR="
                 "CMAKE_PREFIX_PATH=${_decoy}")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DUSE_PKG_CONFIG=ON" "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                          "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1" "-DQB_WITH_COMPRESSION=OFF" HAS "OPENSSL_LINKS_ZLIB=TRUE")
        file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DPARENT_ZLIB_TARGET=ON" NOT "is gone")
        # Compression turned on by the configure that sees that zlib lost: this build directory had it, found by
        # OpenSSL's search, and qb's own zlib search stops on it.
        set(_name "openssl-zlib-compression")
        set(_root "${WORK_DIR}/${_name}/root")
        file(COPY "${WORK_DIR}/openssl-zlib/root/usr" "${WORK_DIR}/openssl-zlib-linked/root/pc" DESTINATION "${_root}")
        plant(zlib "${_root}/Cellar/zlib/1")
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR="
                 "CMAKE_PREFIX_PATH=${_decoy}")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DUSE_PKG_CONFIG=ON" "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                          "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1" "-DQB_WITH_COMPRESSION=OFF"
                          "-DQB_DEPS_FETCH_FALLBACK=OFF" "-DQB_INSTALL=OFF" HAS "OPENSSL_LINKS_ZLIB=TRUE")
        file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} FAIL "-DFIND=openssl|zlib" "-DQB_WITH_COMPRESSION=ON"
                          MUST "the zlib this build directory found is gone")
        # An OpenSSL that did not link zlib (its static libraries not asked for) becomes one that does, in the configure
        # that sees the zlib this build directory recorded lost: searched again around its search all the same, and
        # found nowhere, the configure stops on it.
        set(_name "openssl-zlib-switch")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        file(COPY "${WORK_DIR}/openssl-zlib/root/usr" "${WORK_DIR}/openssl-zlib/root/pc" DESTINATION "${_root}")
        plant(zlib "${_root}/Cellar/zlib/1")
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR="
                 "CMAKE_PREFIX_PATH=${_decoy}")
        configure_fixture(${_name} OK "-DFIND=openssl|zlib" "-DUSE_PKG_CONFIG=ON" "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                          "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1" "-DQB_DEPS_FETCH_FALLBACK=OFF" "-DQB_INSTALL=OFF"
                          HAS "ZLIB=TRUE" "OPENSSL_LINKS_ZLIB=FALSE")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DQB_WITH_COMPRESSION=OFF" HAS "OPENSSL_LINKS_ZLIB=FALSE")
        file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} FAIL "-DFIND=openssl" "-UOPENSSL_INCLUDE_DIR" "-UOPENSSL_SSL_LIBRARY"
                          "-UOPENSSL_CRYPTO_LIBRARY" "-DOPENSSL_USE_STATIC_LIBS=ON"
                          MUST "ZLIB_LIBRARY_RELEASE \\(${_root_regex}/Cellar/zlib/1/lib/[^)]*\\) no longer exists: searched again"
                               "the zlib this build directory found is gone")
        # ... and the other way: an OpenSSL that linked zlib left for one that does not, as that zlib is lost -- nothing
        # searches zlib then, nothing is said.
        set(_name "openssl-zlib-leave")
        set(_root "${WORK_DIR}/${_name}/root")
        file(COPY "${WORK_DIR}/openssl-zlib/root/usr" "${WORK_DIR}/openssl-zlib-linked/root/pc"
                  "${WORK_DIR}/openssl-zlib-linked/root/shared" DESTINATION "${_root}")
        plant(zlib "${_root}/Cellar/zlib/1")
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR="
                 "CMAKE_PREFIX_PATH=${_decoy}")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DUSE_PKG_CONFIG=ON" "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                          "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1" "-DQB_WITH_COMPRESSION=OFF" HAS "OPENSSL_LINKS_ZLIB=TRUE")
        file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} OK "-DFIND=openssl" "-UOPENSSL_INCLUDE_DIR" "-UOPENSSL_SSL_LIBRARY"
                          "-UOPENSSL_CRYPTO_LIBRARY" "-DOPENSSL_ROOT_DIR=/shared"
                          NOT "searched again" "is gone" HAS "OPENSSL=TRUE" "OPENSSL_LINKS_ZLIB=FALSE")
        # A configure that finds no OpenSSL as its zlib goes too: the one that finds it again, its zlib still lost,
        # stops on it.
        set(_name "openssl-zlib-absent")
        set(_root "${WORK_DIR}/${_name}/root")
        file(COPY "${WORK_DIR}/openssl-zlib/root/usr" "${WORK_DIR}/openssl-zlib-linked/root/pc" DESTINATION "${_root}")
        plant(zlib "${_root}/Cellar/zlib/1")
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR="
                 "CMAKE_PREFIX_PATH=${_decoy}")
        configure_fixture(${_name} OK "-DFIND=openssl" "-DUSE_PKG_CONFIG=ON" "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                          "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1" "-DQB_WITH_COMPRESSION=OFF"
                          HAS "OPENSSL=TRUE" "OPENSSL_LINKS_ZLIB=TRUE")
        file(REMOVE_RECURSE "${_root}/usr" "${_root}/Cellar/zlib/1")
        configure_fixture(${_name} FAIL "-DFIND=openssl" MUST "the OpenSSL this build directory found is gone"
                          NOT "ZLIB_[A-Z_]+ \\([^)]*\\) [^:]*: searched again")
        file(COPY "${WORK_DIR}/openssl-zlib/root/usr" DESTINATION "${_root}")
        configure_fixture(${_name} FAIL "-DFIND=openssl" MUST "the zlib this build directory found is gone"
                          NOT "the OpenSSL this build directory found is gone")
        set(_env "")
    endif()
    if(NOT _pkg_config)
        message(STATUS "found-libraries: no pkg-config on this host: the pkg-config cases, the consumer's too, are skipped")
    endif()
endif()
# The consumer of an installed qb (Huly QB-1201): the qbConfig.cmake an install generates searches the consumer's
# libraries the same way -- Argon2, zlib, zstd and brotli, OpenSSL, and libngtcp2 inside FindNgtcp2.cmake -- through the
# consumer project beside this file, which loads none of qb's own CMake. Every platform: a library never found fails
# the configure as before, CMake naming the package it could not find. POSIX, with Ninja and Unix Makefiles where their tools are found: an update that
# removes the version found re-runs the configure at `cmake --build`, which finds each where the update put it; one
# removed and found nowhere else makes find_package(qb) report qb not found, naming each entry and the remedy -- a
# reason a consumer that did not require qb reads, its configure going on -- until it is back; a path set by hand is
# kept and no input; a library this project found before it asked for qb is left as it is; pkg-config's Homebrew shape.
set(_lib_fixture "${_fixture}")
set(_fixture "${CMAKE_CURRENT_LIST_DIR}/consumer")
# Whether this CMake's find_dependency() marks a package a transitive dependency (FeatureSummary reads the mark).
file(READ "${CMAKE_ROOT}/Modules/CMakeFindDependencyMacro.cmake" _find_dependency)
set(_transitive_marks FALSE)
if(_find_dependency MATCHES "TRANSITIVE_DEPENDENCY")
    set(_transitive_marks TRUE)
endif()
set(_fixture_args "") # nothing of qb's tree: an installed qb carries what it needs
include(CMakePackageConfigHelpers)
# install_qb(<case> <package>...): <case>/prefix as the install of a qb built with each <package> -- argon2, zlib,
# zstd, brotli, openssl, ngtcp2 -- lays it out: qbConfig.cmake generated from cmake/qbConfig.cmake.in with that qb's
# values (OpenSSL 3.0.0, the version plant() gives), and the files the install puts beside it; stand-ins for Threads
# and nlohmann_json in <case>/prefix/stand-ins -- nlohmann_json's adds a directory to the module path, as a package's
# own files may, which qb's must leave there.
function(install_qb name)
    set(_dir "${WORK_DIR}/${name}/prefix")
    set(QB_FRAMEWORK_VERSION 0.0.0)
    set(QB_FRAMEWORK_VERSION_MAJOR 0)
    set(QB_FRAMEWORK_VERSION_MINOR 0)
    set(QB_FRAMEWORK_VERSION_PATCH 0)
    set(OPENSSL_VERSION 3.0.0)
    set(QB_ABI_UNORDERED_MAP planted)
    set(_files "${QB_CMAKE_MODULE_DIR}/qbFoundLibraries.cmake")
    foreach(_flag SSL COMPRESSION ZSTD BROTLI ARGON2 QUIC)
        set(QB_HAS_${_flag} FALSE)
    endforeach()
    foreach(_package IN LISTS ARGN)
        if(_package STREQUAL "argon2")
            set(QB_HAS_ARGON2 TRUE)
            list(APPEND _files "${QB_CMAKE_MODULE_DIR}/FindArgon2.cmake")
        elseif(_package STREQUAL "zlib")
            set(QB_HAS_COMPRESSION TRUE)
        elseif(_package STREQUAL "zstd")
            set(QB_HAS_ZSTD TRUE)
            list(APPEND _files "${QB_CMAKE_MODULE_DIR}/FindZstd.cmake")
        elseif(_package STREQUAL "brotli")
            set(QB_HAS_BROTLI TRUE)
            list(APPEND _files "${QB_CMAKE_MODULE_DIR}/FindBrotli.cmake")
        elseif(_package STREQUAL "openssl")
            set(QB_HAS_SSL TRUE)
        else()
            set(QB_HAS_QUIC TRUE)
            list(APPEND _files "${QB_CMAKE_MODULE_DIR}/FindNgtcp2.cmake")
        endif()
    endforeach()
    configure_package_config_file("${QB_CMAKE_MODULE_DIR}/qbConfig.cmake.in" "${_dir}/lib/cmake/qb/qbConfig.cmake"
                                  INSTALL_DESTINATION lib/cmake/qb INSTALL_PREFIX "${_dir}")
    # One PACKAGE_INIT block: a comment that named its token was expanded too, and every installed qbConfig.cmake
    # carried a second one in the middle of that comment.
    file(STRINGS "${_dir}/lib/cmake/qb/qbConfig.cmake" _inits REGEX "Expanded from @PACKAGE_INIT@")
    list(LENGTH _inits _inits)
    if(NOT _inits EQUAL 1)
        set(_failures "${_failures}\n  ${name}: the generated qbConfig.cmake expands PACKAGE_INIT ${_inits} times"
            PARENT_SCOPE)
    endif()
    file(COPY ${_files} DESTINATION "${_dir}/lib/cmake/qb")
    file(WRITE "${_dir}/stand-ins/FindThreads.cmake"
         "set(Threads_FOUND TRUE)\nif(NOT TARGET Threads::Threads)\n    add_library(Threads::Threads INTERFACE IMPORTED)\n"
         "endif()\n")
    file(WRITE "${_dir}/stand-ins/Findnlohmann_json.cmake"
         "set(nlohmann_json_FOUND TRUE)\nset(nlohmann_json_VERSION 3.11.3)\n"
         "list(APPEND CMAKE_MODULE_PATH \"\${CMAKE_CURRENT_LIST_DIR}/nlohmann\")\n")
endfunction()
# Every platform: Zstd never found -- CMake names the package it could not find, and nothing is gone; asked for without
# REQUIRED, qb is not found, and of this file's variables only the saved module path is left, as find_dependency()'s
# return has always left it.
install_qb(consumer-absent zstd)
file(MAKE_DIRECTORY "${WORK_DIR}/consumer-absent/root")
configure_fixture(consumer-absent FAIL MUST "Could not find a package configuration file provided by \"Zstd\""
                  NOT "is gone" "searched again")
configure_fixture(consumer-absent OK "-DOPTIONAL=ON" NOT "is gone" HAS "QB=0" "LEAKED=\\|_qb_saved_module_path\\|")
foreach(_generator IN LISTS _generators)
    use_generator("${_generator}")
    string(REPLACE " " "-" _name "consumer-${_generator}")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    install_qb(${_name} argon2 zlib zstd brotli openssl ngtcp2)
    set(_prefixes "")
    foreach(_package argon2 zlib zstd brotli openssl ngtcp2)
        plant(${_package} "${_root}/Cellar/${_package}/1")
        list(APPEND _prefixes "/Cellar/${_package}/1")
    endforeach()
    file(WRITE "${WORK_DIR}/${_name}/prefixes.cmake" "set(CMAKE_PREFIX_PATH \"${_prefixes}\" CACHE STRING \"\")\n")
    set(_inputs "")
    foreach(_file "argon2;argon2" "zlib;z" "zstd;zstd" "brotli;brotlienc" "brotli;brotlidec" "brotli;brotlicommon"
                  "openssl;ssl" "openssl;crypto" "ngtcp2;ngtcp2" "ngtcp2;ngtcp2_crypto_ossl")
        list(GET _file 0 _package)
        list(GET _file 1 _library)
        library_file(${_library} _library)
        list(APPEND _inputs "INPUTS=.*\\|${_root_regex}/Cellar/${_package}/1/lib/${_library}\\|.*")
    endforeach()
    foreach(_header "argon2;argon2.h" "zlib;zlib.h" "zstd;zstd.h" "brotli;brotli/encode.h" "openssl;openssl/ssl.h"
                    "ngtcp2;ngtcp2/ngtcp2.h" "ngtcp2;ngtcp2/ngtcp2_crypto.h" "ngtcp2;ngtcp2/ngtcp2_crypto_ossl.h")
        list(GET _header 0 _package)
        list(GET _header 1 _file)
        escape("${_file}" _file)
        list(APPEND _inputs "INPUTS=.*\\|${_root_regex}/Cellar/${_package}/1/include/${_file}\\|.*")
    endforeach()
    # The module path as the packages qb found left it, without qb's own directory: two entries, a list's ";" matched
    # by "." (a ";" in an argument would split it).
    escape("${WORK_DIR}/${_name}/prefix/stand-ins" _stand_ins)
    set(_stand_ins "${_stand_ins}.${_stand_ins}/nlohmann")
    set(_marks "")
    if(_transitive_marks) # each found through qb: still marked a dependency of qb's, as find_dependency() marks it
        set(_marks "TRANSITIVE=ZLIB:TRUE OpenSSL:TRUE Argon2:TRUE Zstd:TRUE Brotli:TRUE Ngtcp2:TRUE ")
    endif()
    set(_policy "") # one generator's consumer as a project from before CMake 3.21, whose foreach keeps its variable
    if(_generator STREQUAL "Unix Makefiles")
        set(_policy "-DCMP0124_OLD=ON")
    endif()
    configure_fixture(${_name} OK -C "${WORK_DIR}/${_name}/prefixes.cmake" ${_policy}
                      NOT "CMake Warning" "searched again" "is gone"
                      HAS "QB=1" "MODULE_PATH=${_stand_ins}" "LEAKED=\\|\\|" "IMPORT_PREFIX=" ${_marks} "ARGON2=TRUE" "ZLIB=TRUE" "ZSTD=TRUE" "BROTLI=TRUE" "OPENSSL=TRUE" "NGTCP2=TRUE"
                          "ARGON2_LIBRARY=${_root_regex}/Cellar/argon2/1/lib/.*"
                          "OPENSSL_SSL_LIBRARY=${_root_regex}/Cellar/openssl/1/lib/.*"
                          "NGTCP2_CRYPTO_OSSL_LIBRARY=${_root_regex}/Cellar/ngtcp2/1/lib/.*" ${_inputs})
    build_fixture(${_name} "nothing changed" 1)
    # An update puts each where the default search finds it, and removes the version found: searched again, found there.
    foreach(_package argon2 zlib zstd brotli openssl ngtcp2)
        plant(${_package} "${_root}/usr/local")
        file(REMOVE_RECURSE "${_root}/Cellar/${_package}/1")
    endforeach()
    build_fixture(${_name} "each updated elsewhere, the version found removed" 2
                  "QB=1" "LEAKED=\\|\\|" "ARGON2_LIBRARY=${_root_regex}/usr/local/lib/.*" "ZLIB_LIBRARY_RELEASE=${_root_regex}/usr/local/lib/.*"
                  "ZSTD_LIBRARY=${_root_regex}/usr/local/lib/.*" "BROTLI_COMMON_LIBRARY=${_root_regex}/usr/local/lib/.*"
                  "OPENSSL_INCLUDE_DIR=${_root_regex}/usr/local/include" "OPENSSL_SSL_LIBRARY=${_root_regex}/usr/local/lib/.*"
                  "NGTCP2_INCLUDE_DIR=${_root_regex}/usr/local/include"
                  "NGTCP2_CRYPTO_OSSL_LIBRARY=${_root_regex}/usr/local/lib/.*"
                  OUT "ARGON2_LIBRARY \\(${_root_regex}/Cellar/argon2/1/lib/[^)]*\\) no longer exists: searched again"
                      "ZLIB_LIBRARY_RELEASE \\(${_root_regex}/Cellar/zlib/1/lib/[^)]*\\) no longer exists: searched again"
                      "ZSTD_INCLUDE_DIR \\(${_root_regex}/Cellar/zstd/1/include\\) no longer exists: searched again"
                      "BROTLI_DECODER_LIBRARY \\(${_root_regex}/Cellar/brotli/1/lib/[^)]*\\) no longer exists: searched again"
                      "OPENSSL_CRYPTO_LIBRARY \\(${_root_regex}/Cellar/openssl/1/lib/[^)]*\\) no longer exists: searched again"
                      "NGTCP2_LIBRARY \\(${_root_regex}/Cellar/ngtcp2/1/lib/[^)]*\\) no longer exists: searched again")
    build_fixture(${_name} "nothing changed since" 2)
    set(_configures 2)
    # Each removed in turn and found nowhere else: qb is not found, the reason naming what is still missing and the
    # remedy, until it is back. The include directory stays (a shared prefix): its header is what is gone.
    foreach(_case "argon2;argon2.h;Argon2;ARGON2;QB_HAS_ARGON2" "zlib;zlib.h;zlib;ZLIB;QB_HAS_COMPRESSION"
                  "zstd;zstd.h;zstd;ZSTD;QB_HAS_ZSTD" "brotli;brotli/encode.h;brotli;BROTLI;QB_HAS_BROTLI"
                  "openssl;openssl/ssl.h;OpenSSL;OPENSSL;QB_HAS_SSL" "ngtcp2;ngtcp2/ngtcp2.h;libngtcp2;NGTCP2;QB_HAS_QUIC")
        list(GET _case 0 _package)
        list(GET _case 1 _header)
        list(GET _case 2 _what)
        list(GET _case 3 _entry)
        list(GET _case 4 _flag)
        escape("${_header}" _header_regex)
        file(REMOVE_RECURSE "${WORK_DIR}/${_name}/one") # this package's files, as plant() lays them out
        plant(${_package} "${WORK_DIR}/${_name}/one")
        file(GLOB _mine RELATIVE "${WORK_DIR}/${_name}/one" "${WORK_DIR}/${_name}/one/lib/*"
                                                            "${WORK_DIR}/${_name}/one/include/*")
        foreach(_file IN LISTS _mine)
            file(REMOVE_RECURSE "${_root}/usr/local/${_file}")
        endforeach()
        build_fixture(${_name} "${_package} removed" FAILS
                      OUT "${_entry}_INCLUDE_DIR \\(${_root_regex}/usr/local/include\\) no longer holds ${_header_regex}: searched again"
                          "the ${_what} this build directory found is gone and was found nowhere else -- ${_entry}_INCLUDE_DIR \\(${_root_regex}/usr/local/include\\), "
                          "this qb links it \\(${_flag}\\)")
        build_fixture(${_name} "built again, nothing done" FAILS
                      OUT "the ${_what} this build directory found is gone and was found nowhere else")
        plant(${_package} "${_root}/usr/local")
        math(EXPR _configures "${_configures} + 1")
        build_fixture(${_name} "${_package} back" ${_configures} "QB=1" "LEAKED=\\|\\|"
                      "${_entry}_INCLUDE_DIR=${_root_regex}/usr/local/include")
    endforeach()
endforeach()
if(_generators)
    list(GET _generators 0 _generator)
    use_generator("${_generator}")
    # A path set by hand is kept as given, and is no input: its removal re-runs nothing.
    set(_name "consumer-set-by-hand")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    install_qb(${_name} argon2)
    plant(argon2 "${_root}/usr/local")
    plant(argon2 "${_root}/hand")
    library_file(argon2 _file)
    configure_fixture(${_name} OK "-DARGON2_LIBRARY=${_root}/hand/lib/${_file}"
                      HAS "QB=1" "INPUTS=\\|${_root_regex}/usr/local/include/argon2\\.h\\|")
    file(REMOVE_RECURSE "${_root}/hand")
    build_fixture(${_name} "the library set by hand removed" 1 "ARGON2_LIBRARY=${_root_regex}/hand/lib/.*")
    configure_fixture(${_name} OK
                      MUST "ARGON2_LIBRARY \\(${_root_regex}/hand/lib/[^)]*\\) does not exist; this build directory's search did not find it .*: kept as given"
                      NOT "is gone" HAS "ARGON2_LIBRARY=${_root_regex}/hand/lib/.*")
    # A package never had -- a stray header -- is not "gone": the configure fails as before, naming the package.
    set(_name "consumer-never-had")
    set(_root "${WORK_DIR}/${_name}/root")
    install_qb(${_name} argon2)
    plant(argon2 "${_root}/usr/local" NO argon2)
    configure_fixture(${_name} FAIL MUST "Could not find a package configuration file provided by \"Argon2\""
                      NOT "is gone")
    file(REMOVE "${_root}/usr/local/include/argon2.h")
    configure_fixture(${_name} FAIL MUST "Could not find a package configuration file provided by \"Argon2\""
                      "ARGON2_INCLUDE_DIR \\([^)]*\\) no longer holds argon2\\.h: searched again" NOT "is gone")
    # Each never found, the others found: CMake names it, the module path is as the packages found before left it, and
    # of this file's variables only the saved module path is left, once this file has saved it, as find_dependency()'s
    # return has always left it. Argon2, zstd and brotli again beside the project's own imported target, which qb's
    # search leaves alone: the quiet probe before find_dependency() restores the module path all the same.
    foreach(_case "zlib;ZLIB;" "openssl;OpenSSL;" "argon2;Argon2;" "ngtcp2;Ngtcp2;" "zstd;Zstd;" "brotli;Brotli;"
                  "argon2;Argon2;Argon2::Argon2" "zstd;Zstd;Zstd::Zstd" "brotli;Brotli;Brotli::Encoder|Brotli::Decoder")
        list(GET _case 0 _package)
        list(GET _case 1 _what)
        list(GET _case 2 _targets)
        set(_name "consumer-absent-${_package}")
        set(_args "")
        if(_targets)
            string(APPEND _name "-own")
            set(_args "-DPARENT_TARGETS=${_targets}")
        endif()
        set(_root "${WORK_DIR}/${_name}/root")
        install_qb(${_name} argon2 zlib zstd brotli openssl ngtcp2)
        foreach(_other argon2 zlib zstd brotli openssl ngtcp2)
            if(NOT _other STREQUAL _package)
                plant(${_other} "${_root}/usr/local")
            endif()
        endforeach()
        escape("${WORK_DIR}/${_name}/prefix/stand-ins" _stand_ins)
        if(_package STREQUAL "zlib" OR _package STREQUAL "openssl") # before nlohmann_json, and the module path saved
            set(_leaked "LEAKED=\\|\\|")
        else()
            set(_leaked "LEAKED=\\|_qb_saved_module_path\\|")
            set(_stand_ins "${_stand_ins}.${_stand_ins}/nlohmann")
        endif()
        configure_fixture(${_name} OK "-DOPTIONAL=ON" ${_args} NOT "is gone" "searched again"
                          HAS "QB=0" "MODULE_PATH=${_stand_ins}" "${_leaked}"
                              "REASON=qb could not be found because dependency ${_what} could not be found\\.")
    endforeach()
    # Found in directories side by side, each searching again (an imported target is seen only in the directory that
    # made it and below): each still marked a dependency of qb's -- and one the project found itself, in a directory of
    # its own, is still the project's own, as find_dependency() leaves it.
    if(_transitive_marks)
        foreach(_case "qb|qb;ZLIB:TRUE OpenSSL:TRUE Argon2:TRUE Zstd:TRUE Brotli:TRUE Ngtcp2:TRUE"
                      "ZLIB|Zstd|qb;ZLIB:(False|FALSE) OpenSSL:TRUE Argon2:TRUE Zstd:(False|FALSE) Brotli:TRUE Ngtcp2:TRUE")
            list(GET _case 0 _siblings)
            list(GET _case 1 _marks)
            string(MAKE_C_IDENTIFIER "${_siblings}" _name)
            set(_name "consumer-siblings-${_name}")
            set(_root "${WORK_DIR}/${_name}/root")
            install_qb(${_name} argon2 zlib zstd brotli openssl ngtcp2)
            foreach(_package argon2 zlib zstd brotli openssl ngtcp2)
                plant(${_package} "${_root}/usr/local")
            endforeach()
            configure_fixture(${_name} OK "-DSIBLINGS=${_siblings}" NOT "is gone"
                              HAS "QB=1" "TRANSITIVE=${_marks} " "LEAKED=\\|\\|")
        endforeach()
    endif()
    # A consumer that does not require qb reads the reason, and its configure goes on.
    set(_name "consumer-optional")
    set(_root "${WORK_DIR}/${_name}/root")
    install_qb(${_name} zstd)
    plant(zstd "${_root}/Cellar/zstd/1")
    configure_fixture(${_name} OK "-DOPTIONAL=ON" "-DCMAKE_PREFIX_PATH=/Cellar/zstd/1" HAS "QB=1" "ZSTD=TRUE")
    file(REMOVE_RECURSE "${_root}/Cellar/zstd/1")
    escape("${WORK_DIR}/${_name}/prefix/stand-ins" _stand_ins)
    set(_stand_ins "${_stand_ins}.${_stand_ins}/nlohmann")
    configure_fixture(${_name} OK "-DOPTIONAL=ON"
                      HAS "QB=0" "MODULE_PATH=${_stand_ins}" "LEAKED=\\|\\|" "IMPORT_PREFIX="
                          "REASON=the zstd this build directory found is gone and was found nowhere else -- ZSTD_INCLUDE_DIR .*")
    # ... zlib and OpenSSL too, lost before qb saves the module path: the project's is left as it was -- not emptied, nor
    # set to what an earlier find_package(qb) whose find_dependency() failed left saved.
    foreach(_case "zlib;zlib;ZLIB_INCLUDE_DIR" "openssl;OpenSSL;OPENSSL_INCLUDE_DIR")
        list(GET _case 0 _package)
        list(GET _case 1 _what)
        list(GET _case 2 _entry)
        set(_lost "consumer-optional-${_package}")
        set(_lost_root "${WORK_DIR}/${_lost}/root")
        install_qb(${_lost} ${_package})
        plant(${_package} "${_lost_root}/Cellar/${_package}/1")
        configure_fixture(${_lost} OK "-DOPTIONAL=ON" "-DCMAKE_PREFIX_PATH=/Cellar/${_package}/1" HAS "QB=1")
        file(REMOVE_RECURSE "${_lost_root}/Cellar/${_package}/1")
        escape("${WORK_DIR}/${_lost}/prefix/stand-ins" _lost_stand_ins)
        configure_fixture(${_lost} OK "-DOPTIONAL=ON" "-DSTALE_SAVED_MODULE_PATH=/stale"
                          HAS "QB=0" "MODULE_PATH=${_lost_stand_ins}" "LEAKED=\\|\\|" "IMPORT_PREFIX="
                              "REASON=the ${_what} this build directory found is gone and was found nowhere else -- ${_entry} .*")
    endforeach()
    # ... twice, as two qbm packages each find qb: each reports it, the first not requiring qb, the second requiring it.
    configure_fixture(${_name} FAIL "-DOPTIONAL=OFF" "-DFIND_TWICE=ON"
                      MUST "Reason given by package: +the zstd this build directory found is gone and was found nowhere else")
    plant(zstd "${_root}/Cellar/zstd/1")
    configure_fixture(${_name} OK "-DFIND_TWICE=ON" HAS "QB=1" "FIRST=1\\|" "LEAKED=\\|\\|")
    file(REMOVE_RECURSE "${_root}/Cellar/zstd/1")
    configure_fixture(${_name} OK "-DOPTIONAL=ON" "-DFIND_TWICE=ON"
                      HAS "QB=0" "FIRST=0\\|the zstd this build directory found is gone .*")
    # Found by this project before it asked for qb -- the imported target qb links exists -- a library is left as it is:
    # not searched again, no input; lost, it fails no more than it did before.
    foreach(_case "zlib;ZLIB::ZLIB;ZLIB" "zstd;Zstd::Zstd;ZSTD" "openssl;OpenSSL::SSL|OpenSSL::Crypto;OPENSSL")
        list(GET _case 0 _package)
        list(GET _case 1 _targets)
        list(GET _case 2 _flag)
        set(_name "consumer-found-before-${_package}")
        set(_root "${WORK_DIR}/${_name}/root")
        install_qb(${_name} ${_package})
        plant(${_package} "${_root}/Cellar/${_package}/1")
        configure_fixture(${_name} OK "-DPARENT_TARGETS=${_targets}" "-DCMAKE_PREFIX_PATH=/Cellar/${_package}/1"
                          HAS "QB=1" "${_flag}=TRUE" "INPUTS=\\|\\|")
        file(REMOVE_RECURSE "${_root}/Cellar/${_package}/1")
        configure_fixture(${_name} OK "-DPARENT_TARGETS=${_targets}" NOT "searched again" "is gone" HAS "QB=1")
    endforeach()
    # Half of what qb links made by the project -- Brotli::Encoder without Brotli::Decoder, OpenSSL::SSL without
    # OpenSSL::Crypto: qb's own search runs, records what it found and searches it again when an update removes it --
    # and beside the project's OpenSSL::SSL needs libcrypto alone: the update's OpenSSL has no libssl, which qb does not
    # link there.
    foreach(_case "brotli;Brotli::Encoder;BROTLI_DECODER_LIBRARY" "openssl;OpenSSL::SSL;OPENSSL_CRYPTO_LIBRARY")
        list(GET _case 0 _package)
        list(GET _case 1 _targets)
        list(GET _case 2 _entry)
        set(_name "consumer-found-half-${_package}")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        install_qb(${_name} ${_package})
        plant(${_package} "${_root}/Cellar/${_package}/1")
        configure_fixture(${_name} OK "-DPARENT_TARGETS=${_targets}" "-DCMAKE_PREFIX_PATH=/Cellar/${_package}/1"
                          HAS "QB=1" "${_entry}=${_root_regex}/Cellar/${_package}/1/lib/.*")
        set(_without "")
        if(_package STREQUAL "openssl")
            set(_without NO ssl)
        endif()
        plant(${_package} "${_root}/usr/local" ${_without})
        file(REMOVE_RECURSE "${_root}/Cellar/${_package}/1")
        build_fixture(${_name} "${_package} updated elsewhere, the version found removed" 2 "QB=1"
                      "${_entry}=${_root_regex}/usr/local/lib/.*")
    endforeach()
    # ... in one directory of the project, another without that target: libssl is recorded there all the same, and
    # searched again where the other directory, which links it, finds it after the update.
    set(_name "consumer-found-half-openssl-siblings")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    install_qb(${_name} openssl)
    plant(openssl "${_root}/Cellar/openssl/1")
    configure_fixture(${_name} OK "-DSIBLINGS=qb+OpenSSL::SSL|qb" "-DCMAKE_PREFIX_PATH=/Cellar/openssl/1"
                      HAS "QB=1" "OPENSSL_SSL_LIBRARY=${_root_regex}/Cellar/openssl/1/lib/.*")
    plant(openssl "${_root}/usr/local")
    file(REMOVE_RECURSE "${_root}/Cellar/openssl/1")
    build_fixture(${_name} "openssl updated elsewhere, the version found removed" 2 "QB=1"
                  "OPENSSL_SSL_LIBRARY=${_root_regex}/usr/local/lib/.*" NOTOUT "kept as given")
    # From a qb built without compression, beside a shared OpenSSL: no search of qb's looks at zlib, so a zlib path of
    # the project's own that is gone is not qb's to speak of.
    set(_name "consumer-zlib-not-ours")
    set(_root "${WORK_DIR}/${_name}/root")
    install_qb(${_name} openssl)
    plant(openssl "${_root}/usr/local")
    plant(zlib "${_root}/own")
    library_file(z _file)
    configure_fixture(${_name} OK "-DZLIB_LIBRARY_RELEASE=${_root}/own/lib/${_file}"
                      HAS "QB=1" "OPENSSL=TRUE" "LEAKED=\\|\\|")
    file(REMOVE_RECURSE "${_root}/own")
    configure_fixture(${_name} OK HAS "QB=1" "LEAKED=\\|\\|"
                      NOT "ZLIB_[A-Z_]+ \\([^)]*\\) does not exist" "searched again")
    # OpenSSL's libssl lost while a libcrypto and headers are found elsewhere: FindOpenSSL would report OpenSSL found,
    # and qb links libssl -- reported, naming libssl alone.
    set(_name "consumer-libssl-lost")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    install_qb(${_name} openssl)
    plant(openssl "${_root}/Cellar/openssl/1")
    plant(openssl "${_root}/usr/local" NO ssl)
    configure_fixture(${_name} OK "-DCMAKE_PREFIX_PATH=/Cellar/openssl/1"
                      HAS "QB=1" "OPENSSL_SSL_LIBRARY=${_root_regex}/Cellar/openssl/1/lib/.*")
    file(REMOVE_RECURSE "${_root}/Cellar/openssl/1")
    configure_fixture(${_name} FAIL
                      MUST "the OpenSSL this build directory found is gone and was found nowhere else -- OPENSSL_SSL_LIBRARY \\(${_root_regex}/Cellar/openssl/1/lib/[^)]*\\): install one"
                      NOT "-- OPENSSL_INCLUDE_DIR \\(" "OPENSSL_CRYPTO_LIBRARY \\([^)]*\\): install")
    # QB: find_package(QB) loads qbConfig.cmake on a file system that ignores case (macOS, Windows), and the loss is
    # reported under the name asked for. A copy as qb-config.cmake, which find_package(QB) looks for too, gives the same
    # file to a file system that tells case apart.
    set(_name "consumer-optional")
    file(COPY_FILE "${WORK_DIR}/${_name}/prefix/lib/cmake/qb/qbConfig.cmake"
         "${WORK_DIR}/${_name}/prefix/lib/cmake/qb/qb-config.cmake")
    plant(zstd "${WORK_DIR}/${_name}/root/Cellar/zstd/1")
    configure_fixture(${_name} OK "-DOPTIONAL=ON" "-DFIND_TWICE=OFF" "-DPACKAGE_NAME=QB" HAS "QB=1")
    file(REMOVE_RECURSE "${WORK_DIR}/${_name}/root/Cellar/zstd/1")
    configure_fixture(${_name} OK "-DOPTIONAL=ON" "-DPACKAGE_NAME=QB"
                      HAS "QB=0" "REASON=the zstd this build directory found is gone and was found nowhere else .*")
    # The Homebrew shape: versioned kegs pkg-config alone names, upgraded, the old ones removed -- pkg-config asked again.
    if(_pkg_config)
        set(_name "consumer-pkg-config")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        install_qb(${_name} argon2 zstd brotli openssl ngtcp2)
        function(plant_consumer_kegs version)
            foreach(_package argon2 zstd brotli openssl ngtcp2)
                plant(${_package} "${_root}/Cellar/${_package}/${version}")
            endforeach()
            foreach(_pc "libargon2;argon2;-largon2" "libzstd;zstd;-lzstd" "libbrotlienc;brotli;-lbrotlienc"
                        "libbrotlidec;brotli;-lbrotlidec" "libbrotlicommon;brotli;-lbrotlicommon"
                        "openssl;openssl;-lssl -lcrypto" "libngtcp2;ngtcp2;-lngtcp2"
                        "libngtcp2_crypto_ossl;ngtcp2;-lngtcp2_crypto_ossl")
                list(GET _pc 0 _module)
                list(GET _pc 1 _package)
                list(GET _pc 2 _libs)
                file(WRITE "${_root}/pc/${_module}.pc"
                     "prefix=/Cellar/${_package}/${version}\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\n"
                     "Name: ${_module}\nDescription: planted\nVersion: 3.0.${version}\nLibs: -L\${libdir} ${_libs}\n"
                     "Cflags: -I\${includedir}\n")
            endforeach()
        endfunction()
        plant_consumer_kegs(1)
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR=")
        configure_fixture(${_name} OK "-DUSE_PKG_CONFIG=ON" "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                          HAS "QB=1" "ARGON2_LIBRARY=${_root_regex}/Cellar/argon2/1/lib/.*"
                              "ZSTD_LIBRARY=${_root_regex}/Cellar/zstd/1/lib/.*"
                              "BROTLI_DECODER_LIBRARY=${_root_regex}/Cellar/brotli/1/lib/.*"
                              "OPENSSL_SSL_LIBRARY=${_root_regex}/Cellar/openssl/1/lib/.*"
                              "NGTCP2_LIBRARY=${_root_regex}/Cellar/ngtcp2/1/lib/.*")
        build_fixture(${_name} "nothing changed" 1)
        plant_consumer_kegs(2)
        foreach(_package argon2 zstd brotli openssl ngtcp2)
            file(REMOVE_RECURSE "${_root}/Cellar/${_package}/1")
        endforeach()
        build_fixture(${_name} "upgrades pkg-config alone names, the old kegs removed" 2 "QB=1"
                      "ARGON2_INCLUDE_DIR=${_root_regex}/Cellar/argon2/2/include"
                      "ARGON2_LIBRARY=${_root_regex}/Cellar/argon2/2/lib/.*"
                      "ZSTD_LIBRARY=${_root_regex}/Cellar/zstd/2/lib/.*"
                      "BROTLI_ENCODER_LIBRARY=${_root_regex}/Cellar/brotli/2/lib/.*"
                      "OPENSSL_INCLUDE_DIR=${_root_regex}/Cellar/openssl/2/include"
                      "OPENSSL_CRYPTO_LIBRARY=${_root_regex}/Cellar/openssl/2/lib/.*"
                      "NGTCP2_CRYPTO_OSSL_LIBRARY=${_root_regex}/Cellar/ngtcp2/2/lib/.*")
        build_fixture(${_name} "nothing changed since" 2)
        set(_env "")
        # A static OpenSSL whose pkg-config names -lz: FindOpenSSL searches zlib itself (CMake 3.26 and later).
        # - From a qb built without compression nothing else does: zlib's entries are searched again around OpenSSL's
        #   search, and that zlib lost and found nowhere else is reported, where the generate would fail on the
        #   ZLIB::ZLIB OpenSSL links, naming neither.
        # - From one built with it, zlib's own search runs first and decides: recorded, an input, searched again --
        #   searched after OpenSSL's, it would find the ZLIB::ZLIB FindOpenSSL made and search nothing.
        if(NOT _openssl_finds_zlib)
            message(STATUS "found-libraries: FindOpenSSL searches no zlib here: the consumer's static OpenSSL cases "
                           "are skipped")
        endif()
        foreach(_compression off on)
            if(NOT _openssl_finds_zlib)
                break()
            endif()
            set(_name "consumer-openssl-zlib-${_compression}")
            set(_root "${WORK_DIR}/${_name}/root")
            escape("${_root}" _root_regex)
            if(_compression STREQUAL "on")
                install_qb(${_name} zlib openssl)
                set(_lost "this qb links it \\(QB_HAS_COMPRESSION\\)")
            else()
                install_qb(${_name} openssl)
                set(_lost "the OpenSSL found links it")
            endif()
            plant(openssl "${_root}/usr/local" NO ssl crypto)
            file(WRITE "${_root}/usr/local/lib/libssl.a" "")
            file(WRITE "${_root}/usr/local/lib/libcrypto.a" "")
            file(WRITE "${_root}/pc/openssl.pc"
                 "prefix=/usr/local\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\nName: OpenSSL\n"
                 "Description: planted\nVersion: 3.0.0\nLibs: -L\${libdir} -lssl -lcrypto\nLibs.private: -lz\n"
                 "Cflags: -I\${includedir}\n")
            plant(zlib "${_root}/Cellar/zlib/1" NO z)
            file(WRITE "${_root}/Cellar/zlib/1/lib/libz.a" "")
            set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR=")
            set(_zlib_mark "") # FindOpenSSL's own search of zlib marks it the project's own, as on the base
            if(_transitive_marks)
                set(_zlib_mark "TRANSITIVE=ZLIB:(False|FALSE) .*")
            endif()
            # Its pkg-config names no -pthread: on Linux FindOpenSSL calls find_package(Threads), which the consumer
            # answers by enabling C, as it does on every UNIX for a static OpenSSL (LANGUAGES).
            configure_fixture(${_name} OK "-DUSE_PKG_CONFIG=ON" "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                              "-DOPENSSL_USE_STATIC_LIBS=ON" "-DCMAKE_PREFIX_PATH=/Cellar/zlib/1" "-DCMP0124_OLD=ON"
                              HAS "QB=1" "OPENSSL=TRUE" "LEAKED=\\|\\|" "LANGUAGES=C" ${_zlib_mark}
                                  "ZLIB_LIBRARY_RELEASE=${_root_regex}/Cellar/zlib/1/lib/libz\\.a"
                                  "INPUTS=.*\\|${_root_regex}/Cellar/zlib/1/lib/libz\\.a\\|.*")
            build_fixture(${_name} "nothing changed" 1)
            # Updated where the default search finds it, the version found removed: searched again, found there.
            plant(zlib "${_root}/usr/local" NO z)
            file(WRITE "${_root}/usr/local/lib/libz.a" "")
            file(REMOVE_RECURSE "${_root}/Cellar/zlib/1")
            build_fixture(${_name} "zlib updated elsewhere, the version found removed" 2 "QB=1" "LEAKED=\\|\\|"
                          "ZLIB_LIBRARY_RELEASE=${_root_regex}/usr/local/lib/libz\\.a"
                          OUT "ZLIB_LIBRARY_RELEASE \\(${_root_regex}/Cellar/zlib/1/lib/libz\\.a\\) no longer exists: searched again")
            # Removed, found nowhere else: reported, naming it and that OpenSSL links it.
            file(REMOVE "${_root}/usr/local/include/zlib.h" "${_root}/usr/local/lib/libz.a")
            build_fixture(${_name} "zlib removed" FAILS
                          OUT "the zlib this build directory found is gone and was found nowhere else -- ZLIB_INCLUDE_DIR \\(${_root_regex}/usr/local/include\\), ZLIB_LIBRARY_RELEASE"
                              "${_lost}")
            if(_compression STREQUAL "off")
                # ... beside an OpenSSL::Crypto the project made itself: the ZLIB::ZLIB FindOpenSSL links is read on
                # the OpenSSL::SSL it makes too.
                configure_fixture(${_name} FAIL "-DPARENT_TARGETS=OpenSSL::Crypto"
                                  MUST "the zlib this build directory found is gone and was found nowhere else")
            endif()
            set(_env "")
        endforeach()
        # A static OpenSSL whose pkg-config names -pthread: FindOpenSSL calls find_package(Threads) on any UNIX -- the
        # call it makes on Linux for one that names none (above) -- and CMake's FindThreads needs the C the consumer
        # enables for it: qb found, on macOS too, where nothing else reaches that call, and OpenSSL::Crypto linking the
        # Threads::Threads that call made (OPENSSL_THREADS), so the case cannot pass without it.
        if(_openssl_finds_threads)
            set(_name "consumer-openssl-pthread")
            set(_root "${WORK_DIR}/${_name}/root")
            install_qb(${_name} openssl)
            plant(openssl "${_root}/usr/local" NO ssl crypto)
            file(WRITE "${_root}/usr/local/lib/libssl.a" "")
            file(WRITE "${_root}/usr/local/lib/libcrypto.a" "")
            file(WRITE "${_root}/pc/openssl.pc"
                 "prefix=/usr/local\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\nName: OpenSSL\n"
                 "Description: planted\nVersion: 3.0.0\nLibs: -L\${libdir} -lssl -lcrypto\nLibs.private: -pthread\n"
                 "Cflags: -I\${includedir}\n")
            set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR=")
            configure_fixture(${_name} OK "-DUSE_PKG_CONFIG=ON" "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                              "-DOPENSSL_USE_STATIC_LIBS=ON"
                              HAS "QB=1" "OPENSSL=TRUE" "LEAKED=\\|\\|" "LANGUAGES=C" "OPENSSL_THREADS=TRUE")
            build_fixture(${_name} "nothing changed" 1 "QB=1" "OPENSSL_THREADS=TRUE")
            set(_env "")
        else()
            message(STATUS "found-libraries: FindOpenSSL calls no find_package(Threads) for -pthread here: the "
                           "consumer's -pthread case is skipped")
        endif()
    endif()
endif()
set(_fixture "${_lib_fixture}")
set(_fixture_args "-DQB_CMAKE_MODULE_DIR=${QB_CMAKE_MODULE_DIR}")
if(_failures)
    message(FATAL_ERROR "found-libraries: ${_cases} configures and ${_builds} builds, wrong ones:${_failures}")
endif()

# The configure that registered this test: what its own searches did.
set(_live "")
if(DEFINED LIVE_SEARCHED)
    string(REPLACE "|" ";" _searched "${LIVE_SEARCHED}")
    string(REPLACE "|" ";" _inputs "${LIVE_INPUTS}")
    foreach(_package IN LISTS _searched)
        if(_package STREQUAL "argon2")
            set(_entries ARGON2_INCLUDE_DIR/argon2.h ARGON2_LIBRARY)
        elseif(_package STREQUAL "zlib")
            set(_entries ZLIB_INCLUDE_DIR/zlib.h ZLIB_LIBRARY_RELEASE)
        elseif(_package STREQUAL "zstd")
            set(_entries ZSTD_INCLUDE_DIR/zstd.h ZSTD_LIBRARY)
        else()
            set(_entries BROTLI_INCLUDE_DIR/brotli/encode.h BROTLI_ENCODER_LIBRARY BROTLI_DECODER_LIBRARY
                         BROTLI_COMMON_LIBRARY)
        endif()
        foreach(_spec IN LISTS _entries)
            string(REGEX MATCH "^[A-Z0-9_]+" _entry "${_spec}")
            string(REGEX REPLACE "^[A-Z0-9_]+" "" _file "${_spec}")
            if(NOT DEFINED LIVE_${_entry})
                message(FATAL_ERROR "found-libraries: the configure found ${_package} but did not search ${_entry} "
                                    "through qbFoundLibraries.cmake")
            endif()
            string(REPLACE "|" ";" _reading "${LIVE_${_entry}}")
            list(LENGTH _reading _fields)
            if(NOT _fields EQUAL 3)
                list(APPEND _reading "" "" "")
            endif()
            list(GET _reading 0 _value)
            list(GET _reading 1 _before)
            list(GET _reading 2 _record)
            # Named as checked only when a check applied: a value this search produced, or one it recorded.
            if(IS_ABSOLUTE "${_value}" AND EXISTS "${_value}${_file}"
               AND (NOT "${_value}" STREQUAL "${_before}" OR "${_record}" STREQUAL "${_value}"))
                if(NOT "${_value}" STREQUAL "${_before}" AND NOT "${_record}" STREQUAL "${_value}")
                    message(FATAL_ERROR "found-libraries: the configure's search found ${_entry} (${_value}) but "
                                        "recorded '${_record}': its removal would not be searched again")
                endif()
                if(NOT IS_DIRECTORY "${_value}${_file}" AND NOT "${_value}${_file}" IN_LIST _inputs)
                    message(FATAL_ERROR "found-libraries: ${_value}${_file} (${_entry}) is no input of the configure: "
                                        "its removal would not re-run it")
                endif()
                string(APPEND _live " ${_entry}")
            endif()
        endforeach()
    endforeach()
    if("${_live}" STREQUAL "")
        set(_live " none found")
    endif()
    set(_live "; this configure's searches (${LIVE_SEARCHED}) checked:${_live}")
endif()
string(REPLACE ";" ", " _generators_shown "${_generators}")
if(NOT _generators)
    set(_generators_shown "none: nothing is rebuilt on this host")
endif()
message(STATUS "found-libraries: ${_cases} configures and ${_builds} builds right (rebuilt with: ${_generators_shown})"
               "${_live}")
