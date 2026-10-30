# qb-io-test-unit-quic-openssl-verdict (Huly QB-1185, QB-1190). The OpenSSL and QUIC decisions qbDependencies.cmake
# takes (cmake/qbOpenSSL.cmake) on libraries planted in a scratch root, through the fixture beside this file:
# - two OpenSSL majors disable QUIC with a warning, and QB_REQUIRE_FEATURES=ON makes that a configure error -- what the
#   qb CI lanes configure with -- while one major enables QUIC silently, two copies of one major keep it with a warning
#   and never fail, and an absent libngtcp2 fails only a required QUIC; a parent's FindNgtcp2 module that won the search
#   is not compared. Planted readings, on every platform.
# - qb's own headers, libssl and libcrypto not one major -- a half-cleared cache -- stop the configure, QUIC or not, with
#   the three majors and the remedy, which then works (POSIX: read through a stand-in tool that prints the planted
#   listing, as the module reads otool or readelf); a static or unread library takes its headers' major; LibreSSL, and
#   a system whose file names need not carry the release's major, are not compared.
# - the files read are inputs of the configure: `cmake --build` re-runs it when a library is rewritten, when the link the
#   search found is moved onto a file older than the build -- as a package manager restores an archive's times -- and
#   the one it named removed (libngtcp2's, qb's libssl), when qb's opensslv.h changes, and when files vanish: a path the
#   search found is then searched again -- found where an upgrade put it, pkg-config asked again; found nowhere, the
#   configure stops and names it, never building on without it -- and the build converges, never re-running for ever,
#   while a path set by hand or never there (a library a superbuild builds) is kept and said. qb's OpenSSL is an input
#   with libngtcp2 or without it. In a Homebrew-shaped planted root searched by qb's FindNgtcp2.cmake and
#   CMake's FindOpenSSL, with Ninja and with Unix Makefiles, each where its tool is found (POSIX). On macOS, an upgrade
#   that keeps the old keg moves only the install name the found Cellar path carries: that re-runs it too when the new
#   keg's files are newer than the build directory (Homebrew relocates a bottle's dylibs as it pours them, so their
#   time is the upgrade's), and when the keg the link named is removed; the library read is the one the install name
#   names -- libngtcp2's, and qb's libssl for its own check. A file older than the build directory at a path read before,
#   that path still there, is not seen by the build tool: qb-io-test-unit-ngtcp2-openssl-listing reports it.
#
#   cmake -DQB_CMAKE_MODULE_DIR=<qb>/cmake -DWORK_DIR=<scratch directory> -DGENERATOR=<this build's>
#         [-DPLATFORM=..] [-DTOOLSET=..] [-DMAKE_PROGRAM=..] -P check.cmake

cmake_minimum_required(VERSION 3.24)

foreach(_required QB_CMAKE_MODULE_DIR WORK_DIR GENERATOR)
    if(NOT ${_required})
        message(FATAL_ERROR "quic-openssl-verdict: ${_required} is required")
    endif()
endforeach()
set(_fixture "${CMAKE_CURRENT_LIST_DIR}/fixture")
file(REMOVE_RECURSE "${WORK_DIR}")
unset(ENV{OPENSSL_ROOT_DIR}) # FindOpenSSL would search there, and only there
set(_env "") # an environment for the fixture's configures and builds: `cmake -E env` arguments

file(MAKE_DIRECTORY "${WORK_DIR}")

set(_posix FALSE)
if(NOT WIN32 AND EXISTS /bin/sh)
    set(_posix TRUE)
endif()

# A planted library is a file whose content is the listing the stand-in tool prints for it, in the host's format; its
# install name, for `otool -D`, is the content of <file>.id when there is one.
set(_tool "${WORK_DIR}/tool")
if(_posix)
    file(WRITE "${_tool}" "#!/bin/sh\necho \"$2:\"\nif [ \"$1\" = -D ]; then [ -f \"$2.id\" ] && cat \"$2.id\"; exit 0; fi\n"
                          "cat \"$2\"\n")
    file(CHMOD "${_tool}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif()
function(plant file ssl_major crypto_major)
    set(_format ELF)
    if(APPLE)
        set(_format MACHO)
    endif()
    plant_as(${_format} "${file}" "${ssl_major}" "${crypto_major}")
endfunction()
function(plant_as format file ssl_major crypto_major) # plant(), in <format>: MACHO or ELF, whatever the host's
    set(_listing "")
    foreach(_name IN ITEMS ssl crypto)
        set(_major "${${_name}_major}")
        if("${_major}" STREQUAL "")
            continue()
        endif()
        if(format STREQUAL "MACHO")
            string(APPEND _listing "\t/opt/o${_major}/lib/lib${_name}.${_major}.dylib (compatibility version ${_major}.0.0)\n")
        else()
            string(APPEND _listing " 0x0000000000000001 (NEEDED)             Shared library: [lib${_name}.so.${_major}]\n")
        endif()
    endforeach()
    get_filename_component(_dir "${file}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${file}" "${_listing}")
endfunction()
function(escape text out_var) # <text> as a regex that matches it
    string(REGEX REPLACE "([][+.*()^$?|\\\\{}])" "\\\\\\1" _escaped "${text}")
    set(${out_var} "${_escaped}" PARENT_SCOPE)
endfunction()

# configure_fixture(<case> <build dir> OK|FAIL <-D...>... [MUST <regex>...] [NOT <regex>...] [HAS <line regex>...]):
# configures the fixture, then checks its exit, its output (MUST hold each regex, NOT none) and verdict.txt (HAS a line
# matching each). The output's lines are joined first: CMake wraps a warning's text.
set(_cases 0)
set(_failures "")
function(configure_fixture name build expect)
    set(_args -S "${_fixture}" -B "${build}" -G "${GENERATOR}" --no-warn-unused-cli
              "-DQB_CMAKE_MODULE_DIR=${QB_CMAKE_MODULE_DIR}"
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
    if(APPLE)
        list(APPEND _args "-DNGTCP2_OTOOL_EXECUTABLE=${_tool}")
    elseif(_posix)
        list(APPEND _args "-DCMAKE_READELF=${_tool}")
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

# A planted libngtcp2 under <case>/root -- its headers and its core library; the crypto helper is each case's -- as the
# cache entries that make qb's FindNgtcp2.cmake find it, into <out_var>.
function(plant_ngtcp2 name out_var)
    set(_root "${WORK_DIR}/${name}/root")
    file(MAKE_DIRECTORY "${_root}/include/ngtcp2" "${_root}/lib")
    foreach(_header ngtcp2.h ngtcp2_crypto.h ngtcp2_crypto_ossl.h)
        file(WRITE "${_root}/include/ngtcp2/${_header}" "")
    endforeach()
    file(WRITE "${_root}/lib/libngtcp2.planted" "")
    set(${out_var} "-DNGTCP2_INCLUDE_DIR=${_root}/include" "-DNGTCP2_LIBRARY=${_root}/lib/libngtcp2.planted"
        "-DNGTCP2_CRYPTO_INCLUDE_DIR=${_root}/include" "-DNGTCP2_CRYPTO_OSSL_INCLUDE_DIR=${_root}/include" PARENT_SCOPE)
endfunction()

set(_majors "two OpenSSL majors in one process crash at the first QUIC handshake - QUIC support disabled")
set(_required "QB_REQUIRE_FEATURES=ON, so this downgrade is an error")
set(_unread "-DOPENSSL_SSL_LIBRARY=/nonexistent/libssl.so" "-DOPENSSL_CRYPTO_LIBRARY=/nonexistent/libcrypto.so"
            "-DOPENSSL_VERSION=3.5.9" "-DOPENSSL_INCLUDE_DIR=/nonexistent/include")

# Planted readings, every platform: qb's libraries are not there to read, so its side is its headers' major.
plant_ngtcp2(majors-required _ngtcp2)
file(WRITE "${WORK_DIR}/majors-required/root/lib/helper" "")
configure_fixture(majors-required "${WORK_DIR}/majors-required/build" FAIL ${_ngtcp2} ${_unread}
                  "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${WORK_DIR}/majors-required/root/lib/helper"
                  "-DPLANTED_NGTCP2=4|/opt/o4/lib/libssl.4.dylib|4|/opt/o4/lib/libcrypto.4.dylib"
                  "-DQB_REQUIRE_FEATURES=ON"
                  MUST "CMake Error" "${_required}" "${_majors}"
                       "links OpenSSL 4 \\(/opt/o4/lib/libssl\\.4\\.dylib\\) and qb links OpenSSL 3 "
                       "Build qb against the OpenSSL libngtcp2 uses \\(-DOPENSSL_ROOT_DIR=/opt/o4\\)")
plant_ngtcp2(majors _ngtcp2)
file(WRITE "${WORK_DIR}/majors/root/lib/helper" "")
configure_fixture(majors "${WORK_DIR}/majors/build" OK ${_ngtcp2} ${_unread}
                  "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${WORK_DIR}/majors/root/lib/helper"
                  "-DPLANTED_NGTCP2=4|/opt/o4/lib/libssl.4.dylib|4|/opt/o4/lib/libcrypto.4.dylib"
                  "-DQB_REQUIRE_FEATURES=OFF"
                  MUST "CMake Warning" "${_majors}" NOT "${_required}" "its TLS loads both"
                  HAS "QB_HAS_QUIC=FALSE" HAS "VERDICT=MAJORS" HAS "COMPARED=TRUE" HAS "LIBRARIES=")
plant_ngtcp2(one-major _ngtcp2)
file(WRITE "${WORK_DIR}/one-major/root/lib/helper" "")
configure_fixture(one-major "${WORK_DIR}/one-major/build" OK ${_ngtcp2} ${_unread}
                  "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${WORK_DIR}/one-major/root/lib/helper" "-DPLANTED_NGTCP2=3||3|"
                  "-DQB_REQUIRE_FEATURES=ON"
                  NOT "CMake Warning" "OpenSSL majors"
                  HAS "QB_HAS_QUIC=TRUE" HAS "VERDICT=" HAS "LIBRARIES=Ngtcp2::ngtcp2.Ngtcp2::crypto_ossl")
# No libngtcp2: a required QUIC is a downgrade, an AUTO one is not.
file(MAKE_DIRECTORY "${WORK_DIR}/absent-required/root" "${WORK_DIR}/absent-auto/root")
configure_fixture(absent-required "${WORK_DIR}/absent-required/build" FAIL ${_unread}
                  "-DQB_WITH_QUIC=ON" "-DQB_REQUIRE_FEATURES=ON"
                  MUST "libngtcp2 not found - QUIC support disabled" "${_required}")
configure_fixture(absent-auto "${WORK_DIR}/absent-auto/build" OK ${_unread} "-DQB_WITH_QUIC=AUTO" "-DQB_REQUIRE_FEATURES=ON"
                  MUST "libngtcp2 not found - QUIC transport auto-disabled" NOT "CMake Warning"
                  HAS "QB_HAS_QUIC=FALSE" HAS "COMPARED=FALSE")
# No OpenSSL from the start: the search finds nothing, nothing was had, nothing stops -- OpenSSL is not found.
file(MAKE_DIRECTORY "${WORK_DIR}/openssl-absent/root")
configure_fixture(openssl-absent "${WORK_DIR}/openssl-absent/build" OK "-DFIND_OPENSSL=ON"
                  NOT "is gone" HAS "OPENSSL=FALSE")
# Paths that never existed -- libraries a superbuild builds after this configure -- are kept, never searched again.
plant_ngtcp2(not-yet-built _ngtcp2)
set(_later "${WORK_DIR}/not-yet-built/later")
escape("${_later}" _later_regex)
configure_fixture(not-yet-built "${WORK_DIR}/not-yet-built/build" OK ${_ngtcp2} ${_unread}
                  "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${_later}/libngtcp2_crypto_ossl.so" "-DPLANTED_NGTCP2=3||3|"
                  "-DOPENSSL_CRYPTO_LIBRARY=${_later}/libcrypto.so"
                  HAS "NGTCP2=TRUE" HAS "NGTCP2_CRYPTO_OSSL_LIBRARY=${_later_regex}/libngtcp2_crypto_ossl\\.so"
                  HAS "OPENSSL_CRYPTO_LIBRARY=${_later_regex}/libcrypto\\.so")
# A parent project's FindNgtcp2 module won the search: it defines none of the readings qb's own does, so nothing is
# compared -- not qb's side alone against nothing -- and QUIC is enabled as found.
plant_ngtcp2(parent-module _ngtcp2)
file(WRITE "${WORK_DIR}/parent-module/modules/FindNgtcp2.cmake" "set(Ngtcp2_FOUND TRUE)\n")
configure_fixture(parent-module "${WORK_DIR}/parent-module/build" OK ${_unread}
                  "-DPARENT_MODULE_DIR=${WORK_DIR}/parent-module/modules"
                  NOT "CMake Warning"
                  HAS "NGTCP2=TRUE" HAS "COMPARED=FALSE" HAS "QB_HAS_QUIC=TRUE")
# ... and one that defines them -- an installed qb's FindNgtcp2 of an earlier version on the module path: compared, and
# nothing that only qb's own module defines is called (its functions-only include stops before _ngtcp2_held_file()).
plant_ngtcp2(parent-module-readings _ngtcp2)
file(WRITE "${WORK_DIR}/parent-module-readings/modules/FindNgtcp2.cmake"
     "set(Ngtcp2_FOUND TRUE)\nset(Ngtcp2_OPENSSL_MAJOR 3)\nset(Ngtcp2_OPENSSL_LIBRARY \"\")\n"
     "set(Ngtcp2_OPENSSL_CRYPTO_MAJOR 3)\nset(Ngtcp2_OPENSSL_CRYPTO_LIBRARY \"\")\n")
configure_fixture(parent-module-readings "${WORK_DIR}/parent-module-readings/build" OK ${_ngtcp2} ${_unread}
                  "-DPARENT_MODULE_DIR=${WORK_DIR}/parent-module-readings/modules"
                  NOT "CMake Warning"
                  HAS "NGTCP2=TRUE" HAS "COMPARED=TRUE" HAS "QB_HAS_QUIC=TRUE")

if(_posix)
    # Read through the stand-in tool. qb's own OpenSSL not one major (Huly QB-1190) stops the configure, naming the three
    # entries, their majors and the remedy -- before any QUIC decision, whatever QB_REQUIRE_FEATURES says: a cache that
    # contradicts itself, not a missing feature. A half-cleared cache: qb's libssl re-found on OpenSSL 4, its headers and
    # libcrypto still OpenSSL 3's -- beside a libngtcp2 on 4 that agrees with that libssl, without libngtcp2, QUIC off.
    escape("Remove all three -- cmake -U OPENSSL_INCLUDE_DIR -U OPENSSL_SSL_LIBRARY -U OPENSSL_CRYPTO_LIBRARY [-DOPENSSL_ROOT_DIR=<one OpenSSL>] <build directory> -- or configure with --fresh"
           _split_remedy)
    set(_split "qb's OpenSSL is not one major: OPENSSL_INCLUDE_DIR \\(")
    foreach(_case half-cleared half-cleared-no-quic half-cleared-quic-off)
        set(_lib "${WORK_DIR}/${_case}/root/lib")
        set(_quic "-DQB_WITH_QUIC=OFF")
        if(_case STREQUAL "half-cleared")
            plant_ngtcp2(${_case} _quic)
            plant("${_lib}/helper" 4 4)
            list(APPEND _quic "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${_lib}/helper" "-DQB_REQUIRE_FEATURES=ON")
        elseif(_case STREQUAL "half-cleared-no-quic")
            set(_quic "-DQB_WITH_QUIC=AUTO")
        endif()
        plant("${_lib}/o4/libssl" 4 4)
        plant("${_lib}/o3/libcrypto" "" 3)
        escape("${_lib}" _lib_regex)
        configure_fixture(${_case} "${WORK_DIR}/${_case}/build" FAIL ${_quic}
                          "-DOPENSSL_SSL_LIBRARY=${_lib}/o4/libssl" "-DOPENSSL_CRYPTO_LIBRARY=${_lib}/o3/libcrypto"
                          "-DOPENSSL_VERSION=3.5.9" "-DOPENSSL_INCLUDE_DIR=${_lib}/o3/include"
                          MUST "CMake Error" "${_split}${_lib_regex}/o3/include\\) holds the headers of OpenSSL 3\\.5\\.9, for libraries of major 3; OPENSSL_SSL_LIBRARY \\(${_lib_regex}/o4/libssl\\) is a libssl of major 4; and OPENSSL_CRYPTO_LIBRARY \\(${_lib_regex}/o3/libcrypto\\) is a libcrypto of major 3\\."
                               "${_split_remedy}" "Huly QB-1190"
                          NOT "${_required}" "QUIC support disabled" "libngtcp2")
    endforeach()
    # The other shapes measured: a libcrypto ahead of its libssl and headers; headers ahead of both libraries; a static
    # libssl, and one no tool reads, taken as their headers' major beside a libcrypto of another.
    function(split_case name ssl_file ssl_major crypto_major version expected)
        set(_lib "${WORK_DIR}/${name}/root/lib")
        if(ssl_file MATCHES "\\.a$")
            file(MAKE_DIRECTORY "${_lib}")
            file(WRITE "${_lib}/${ssl_file}" "")
        else()
            plant("${_lib}/${ssl_file}" "${ssl_major}" "${ssl_major}")
        endif()
        plant("${_lib}/libcrypto" "" "${crypto_major}")
        configure_fixture(${name} "${WORK_DIR}/${name}/build" FAIL "-DQB_WITH_QUIC=OFF"
                          "-DOPENSSL_SSL_LIBRARY=${_lib}/${ssl_file}" "-DOPENSSL_CRYPTO_LIBRARY=${_lib}/libcrypto"
                          "-DOPENSSL_VERSION=${version}" "-DOPENSSL_INCLUDE_DIR=${_lib}/include"
                          MUST "${_split}[^)]*\\) ${expected}" "${_split_remedy}")
        set(_cases ${_cases} PARENT_SCOPE)
        set(_failures "${_failures}" PARENT_SCOPE)
    endfunction()
    split_case(crypto-ahead libssl 3 4 3.5.9 "holds the headers of OpenSSL 3\\.5\\.9, for libraries of major 3; OPENSSL_SSL_LIBRARY \\([^)]*\\) is a libssl of major 3; and OPENSSL_CRYPTO_LIBRARY \\([^)]*\\) is a libcrypto of major 4\\.")
    split_case(headers-ahead libssl 3 3 4.1.0 "holds the headers of OpenSSL 4\\.1\\.0, for libraries of major 4; OPENSSL_SSL_LIBRARY \\([^)]*\\) is a libssl of major 3; and OPENSSL_CRYPTO_LIBRARY \\([^)]*\\) is a libcrypto of major 3\\.")
    split_case(static-split libssl.a "" 4 3.5.9 "holds the headers of OpenSSL 3\\.5\\.9, for libraries of major 3; OPENSSL_SSL_LIBRARY \\([^)]*/libssl\\.a\\) is a static libssl, taken as major 3 like its headers; and OPENSSL_CRYPTO_LIBRARY \\([^)]*\\) is a libcrypto of major 4\\.")
    split_case(unread-split libssl "" 4 3.5.9 "holds the headers of OpenSSL 3\\.5\\.9, for libraries of major 3; OPENSSL_SSL_LIBRARY \\([^)]*/libssl\\) was not read, taken as major 3 like its headers; and OPENSSL_CRYPTO_LIBRARY \\([^)]*\\) is a libcrypto of major 4\\.")
    # The major the headers name their libraries with is OPENSSL_SHLIB_VERSION's first number (SHLIB_VERSION_NUMBER's
    # on 1.1), which a build may set apart from the release's: quictls names 3.1's libraries libssl.so.81.3. One OpenSSL
    # so numbered is not refused; its libcrypto beside another OpenSSL's is, by that number. OPENSSL_SHLIB_VERSION wins
    # over SHLIB_VERSION_NUMBER, wherever each stands in the file; an unreadable version still compares the libraries.
    function(numbered_case name expect version defines ssl crypto)
        set(_lib "${WORK_DIR}/${name}/root/lib")
        plant("${_lib}/libssl" "${ssl}" "${ssl}")
        plant("${_lib}/libcrypto" "" "${crypto}")
        file(WRITE "${_lib}/include/openssl/opensslv.h" "${defines}")
        configure_fixture(${name} "${WORK_DIR}/${name}/build" ${expect} "-DQB_WITH_QUIC=OFF"
                          "-DOPENSSL_SSL_LIBRARY=${_lib}/libssl" "-DOPENSSL_CRYPTO_LIBRARY=${_lib}/libcrypto"
                          "-DOPENSSL_VERSION=${version}" "-DOPENSSL_INCLUDE_DIR=${_lib}/include" ${ARGN})
        set(_cases ${_cases} PARENT_SCOPE)
        set(_failures "${_failures}" PARENT_SCOPE)
    endfunction()
    numbered_case(quictls OK 3.1.7 "# define OPENSSL_VERSION_MAJOR 3\n# define OPENSSL_SHLIB_VERSION 81.3\n" 81 81
                  NOT "not one major" HAS "OPENSSL_COMPARED=TRUE" HAS "OPENSSL_READ=81\\|81")
    numbered_case(quictls-split FAIL 3.1.7 "# define OPENSSL_SHLIB_VERSION 81.3\n" 81 3
                  MUST "${_split}[^)]*\\) holds the headers of OpenSSL 3\\.1\\.7, for libraries of major 81; OPENSSL_SSL_LIBRARY \\([^)]*\\) is a libssl of major 81; and OPENSSL_CRYPTO_LIBRARY \\([^)]*\\) is a libcrypto of major 3\\.")
    numbered_case(openssl-1.1 OK 1.1.1w "# define SHLIB_VERSION_NUMBER \"1.1\"\n" 1 1
                  NOT "not one major" HAS "OPENSSL_READ=1\\|1")
    numbered_case(shlib-first OK 3.1.7 "# define OPENSSL_SHLIB_VERSION 81.3\n# define SHLIB_VERSION_NUMBER \"1.1\"\n" 81 81
                  NOT "not one major")
    numbered_case(shlib-last OK 3.1.7 "# define SHLIB_VERSION_NUMBER \"1.1\"\n# define OPENSSL_SHLIB_VERSION 81.3\n" 81 81
                  NOT "not one major")
    numbered_case(version-unread FAIL "" "" 3 4
                  MUST "${_split}[^)]*\\) holds OpenSSL headers whose version FindOpenSSL could not read; OPENSSL_SSL_LIBRARY \\([^)]*\\) is a libssl of major 3; and OPENSSL_CRYPTO_LIBRARY \\([^)]*\\) is a libcrypto of major 4\\.")
    # One major, read: the readings are the libraries', not the headers' stood in.
    set(_lib "${WORK_DIR}/one-openssl/root/lib")
    plant("${_lib}/libssl" 3 3)
    plant("${_lib}/libcrypto" "" 3)
    configure_fixture(one-openssl "${WORK_DIR}/one-openssl/build" OK "-DQB_WITH_QUIC=OFF"
                      "-DOPENSSL_SSL_LIBRARY=${_lib}/libssl" "-DOPENSSL_CRYPTO_LIBRARY=${_lib}/libcrypto"
                      "-DOPENSSL_VERSION=3.5.9" "-DOPENSSL_INCLUDE_DIR=${_lib}/include"
                      NOT "CMake Warning" "not one major"
                      HAS "OPENSSL_COMPARED=TRUE" HAS "OPENSSL_READ=3\\|3")
    # LibreSSL numbers its libssl and libcrypto apart: never compared, said.
    set(_lib "${WORK_DIR}/libressl/root/lib")
    plant("${_lib}/libssl" 55 52)
    plant("${_lib}/libcrypto" "" 52)
    file(WRITE "${_lib}/include/openssl/opensslv.h" "#define LIBRESSL_VERSION_NUMBER\t0x3080200fL\n")
    configure_fixture(libressl "${WORK_DIR}/libressl/build" OK "-DQB_WITH_QUIC=OFF"
                      "-DOPENSSL_SSL_LIBRARY=${_lib}/libssl" "-DOPENSSL_CRYPTO_LIBRARY=${_lib}/libcrypto"
                      "-DOPENSSL_VERSION=2.0.0" "-DOPENSSL_INCLUDE_DIR=${_lib}/include"
                      MUST "OpenSSL majors not compared: LibreSSL numbers its libssl and libcrypto apart"
                      NOT "not one major"
                      HAS "OPENSSL_COMPARED=FALSE" HAS "OPENSSL_READ=\\|")
    # A system whose base OpenSSL need not carry the release's major in its file names (FreeBSD's libssl.so.30 holds
    # OpenSSL 3): never compared, said. Read through the stand-in as readelf, whatever the host.
    set(_lib "${WORK_DIR}/elsewhere/root/lib")
    plant_as(ELF "${_lib}/libssl" 30 30)
    plant_as(ELF "${_lib}/libcrypto" "" 30)
    configure_fixture(elsewhere "${WORK_DIR}/elsewhere/build" OK "-DQB_WITH_QUIC=OFF" "-DCMAKE_SYSTEM_NAME=FreeBSD"
                      "-DCMAKE_READELF=${_tool}"
                      "-DOPENSSL_SSL_LIBRARY=${_lib}/libssl" "-DOPENSSL_CRYPTO_LIBRARY=${_lib}/libcrypto"
                      "-DOPENSSL_VERSION=3.0.12" "-DOPENSSL_INCLUDE_DIR=${_lib}/include"
                      MUST "OpenSSL majors not compared on FreeBSD: a library's file name need not carry its release's major there"
                      NOT "not one major"
                      HAS "OPENSSL_COMPARED=FALSE")
    # There, the QUIC decision still reads qb's libssl and libcrypto, and two majors refuse QUIC with the remedy that
    # fits when libngtcp2 read nothing.
    plant_ngtcp2(split-qb _ngtcp2)
    set(_lib "${WORK_DIR}/split-qb/root/lib")
    file(WRITE "${_lib}/helper" "")
    plant_as(ELF "${_lib}/libssl" 4 4)
    plant_as(ELF "${_lib}/libcrypto" "" 3)
    configure_fixture(split-qb "${WORK_DIR}/split-qb/build" OK ${_ngtcp2} "-DPLANTED_NGTCP2=|||"
                      "-DCMAKE_SYSTEM_NAME=FreeBSD" "-DCMAKE_READELF=${_tool}"
                      "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${_lib}/helper" "-DOPENSSL_SSL_LIBRARY=${_lib}/libssl"
                      "-DOPENSSL_CRYPTO_LIBRARY=${_lib}/libcrypto" "-DOPENSSL_VERSION=4.1.0"
                      MUST "${_majors}" "links an OpenSSL no tool could read"
                           "qb links the libssl of OpenSSL 4 \\([^)]*\\) with the libcrypto of OpenSSL 3"
                           "Build qb against one OpenSSL in a new build directory"
                      HAS "VERDICT=MAJORS" HAS "OPENSSL_COMPARED=FALSE")
    # The same with headers that number their libraries 81 (quictls), beside a helper on libssl.so.81: two copies of
    # one, not two majors -- QUIC's comparison takes qb's static side as the check took its headers.
    plant_ngtcp2(copies-numbered _ngtcp2)
    set(_lib "${WORK_DIR}/copies-numbered/root/lib")
    plant("${_lib}/helper" 81 81)
    file(WRITE "${_lib}/libssl.a" "")
    file(WRITE "${_lib}/libcrypto.a" "")
    file(WRITE "${_lib}/include/openssl/opensslv.h" "# define OPENSSL_SHLIB_VERSION 81.3\n")
    configure_fixture(copies-numbered "${WORK_DIR}/copies-numbered/build" OK ${_ngtcp2}
                      "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${_lib}/helper" "-DOPENSSL_SSL_LIBRARY=${_lib}/libssl.a"
                      "-DOPENSSL_CRYPTO_LIBRARY=${_lib}/libcrypto.a" "-DOPENSSL_VERSION=3.1.7"
                      "-DOPENSSL_INCLUDE_DIR=${_lib}/include" "-DQB_REQUIRE_FEATURES=ON"
                      MUST "two copies of OpenSSL 81 in one process" NOT "${_required}" "not one major"
                      HAS "QB_HAS_QUIC=TRUE" HAS "VERDICT=COPIES")
    # Two copies of one major -- a static libssl in qb -- warn and keep QUIC, even under QB_REQUIRE_FEATURES=ON; the
    # static archives read nothing and take their headers' major.
    plant_ngtcp2(copies _ngtcp2)
    set(_lib "${WORK_DIR}/copies/root/lib")
    plant("${_lib}/helper" 3 3)
    file(WRITE "${_lib}/libssl.a" "")
    file(WRITE "${_lib}/libcrypto.a" "")
    configure_fixture(copies "${WORK_DIR}/copies/build" OK ${_ngtcp2}
                      "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${_lib}/helper" "-DOPENSSL_SSL_LIBRARY=${_lib}/libssl.a"
                      "-DOPENSSL_CRYPTO_LIBRARY=${_lib}/libcrypto.a" "-DOPENSSL_VERSION=3.5.9"
                      "-DOPENSSL_INCLUDE_DIR=${_lib}/include" "-DQB_REQUIRE_FEATURES=ON"
                      MUST "CMake Warning" "two copies of OpenSSL 3 in one process" "qb links OpenSSL 3 .* statically"
                           "shared \\(OPENSSL_USE_STATIC_LIBS off\\)" NOT "${_required}" "not one major"
                      HAS "QB_HAS_QUIC=TRUE" HAS "VERDICT=COPIES" HAS "OPENSSL_COMPARED=TRUE" HAS "OPENSSL_READ=\\|")
endif()
if(_failures)
    message(FATAL_ERROR "quic-openssl-verdict: ${_cases} configures, wrong ones:${_failures}")
endif()

# `cmake --build` re-runs the configure when a library it read changes, with each generator whose tool is found.
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
        message(FATAL_ERROR "quic-openssl-verdict: neither ninja nor make found on a POSIX host")
    endif()
endif()
# build_fixture(<case> <what changed> <configures wanted so far> <verdict wanted> [<line of verdict.txt wanted>...]
#               [OUT <regex its output must hold>...] [NOTOUT <regex it must not>...]); <configures wanted> FAILS: the
#               build must fail, on its output.
function(build_fixture name label want_count want_verdict)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${_env} "${CMAKE_COMMAND}" --build "${WORK_DIR}/${name}/build"
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    string(REGEX REPLACE "[ \t\r]*\n[ \t]*" " " _flat "${_out}")
    file(STRINGS "${WORK_DIR}/${name}/configures.txt" _lines)
    list(LENGTH _lines _count)
    file(READ "${WORK_DIR}/${name}/verdict.txt" _verdict)
    math(EXPR _n "${_builds} + 1")
    set(_builds ${_n} PARENT_SCOPE)
    set(_wrong "")
    set(_checks "VERDICT=${want_verdict}" ${ARGN})
    if(want_count STREQUAL "FAILS")
        set(_checks ${ARGN})
        if(_rc EQUAL 0)
            string(APPEND _wrong " the build succeeded;")
        endif()
    elseif(NOT _rc EQUAL 0)
        string(APPEND _wrong " build exit ${_rc};")
    elseif(NOT _count EQUAL want_count)
        string(APPEND _wrong " ${_count} configures, want ${want_count};")
    endif()
    set(_kind LINE)
    foreach(_check IN LISTS _checks)
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
# Waits for the clock to leave the second the build system of <case> was last written in, so that a file written next
# is newer on a file system that keeps whole seconds; no wait on one that keeps less.
function(after_configure name)
    set(_build "${WORK_DIR}/${name}/build")
    set(_newest 0)
    foreach(_file IN ITEMS build.ninja Makefile CMakeFiles/Makefile.cmake CMakeCache.txt)
        if(EXISTS "${_build}/${_file}")
            file(TIMESTAMP "${_build}/${_file}" _written "%s" UTC)
            if(_written GREATER _newest)
                set(_newest ${_written})
            endif()
        endif()
    endforeach()
    foreach(_attempt RANGE 30)
        string(TIMESTAMP _now "%s" UTC)
        if(_now GREATER _newest)
            break()
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
    endforeach()
endfunction()
# A Homebrew-shaped root the searches find their libraries in: libngtcp2 in versioned kegs behind an opt/ link, qb's
# OpenSSL with its libssl and libcrypto behind links as distributions install them.
set(_ext ".so")
if(APPLE)
    set(_ext ".dylib")
endif()
function(plant_keg keg major) # headers, libngtcp2 and its crypto helper on OpenSSL <major>
    file(MAKE_DIRECTORY "${keg}/include/ngtcp2")
    foreach(_header ngtcp2.h ngtcp2_crypto.h ngtcp2_crypto_ossl.h)
        file(WRITE "${keg}/include/ngtcp2/${_header}" "")
    endforeach()
    file(WRITE "${keg}/lib/libngtcp2${_ext}" "")
    plant("${keg}/lib/libngtcp2_crypto_ossl${_ext}" ${major} ${major})
endfunction()
function(plant_openssl prefix major file) # an OpenSSL <major> whose libssl and libcrypto are links to <file>s
    file(WRITE "${prefix}/include/openssl/ssl.h" "")
    file(WRITE "${prefix}/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"${major}.0.0\"\n")
    plant("${prefix}/lib/libssl.${file}" ${major} ${major})
    point("${prefix}/lib/libssl${_ext}" "${prefix}/lib/libssl.${file}")
    plant("${prefix}/lib/libcrypto.${file}" "" ${major})
    point("${prefix}/lib/libcrypto${_ext}" "${prefix}/lib/libcrypto.${file}")
endfunction()
function(point link target) # (re)points the link <link> at <target>
    file(REMOVE "${link}")
    get_filename_component(_dir "${link}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dir}")
    file(CREATE_LINK "${target}" "${link}" SYMBOLIC)
endfunction()
function(backdate file) # as a package manager restores an archive's times
    execute_process(COMMAND touch -t 200006150000 "${file}" RESULT_VARIABLE _rc)
    file(TIMESTAMP "${file}" _year "%Y" UTC)
    if(NOT _rc EQUAL 0 OR NOT _year EQUAL 2000)
        message(FATAL_ERROR "quic-openssl-verdict: could not date ${file} back to 2000 (touch exit ${_rc}, year "
                            "'${_year}'): the older-file cases would prove nothing")
    endif()
endfunction()
function(entries out_var prefix) # the five lines of verdict.txt that name libngtcp2's cache entries under <prefix>
    set(_lines "")
    foreach(_entry IN ITEMS NGTCP2_INCLUDE_DIR NGTCP2_LIBRARY NGTCP2_CRYPTO_OSSL_LIBRARY NGTCP2_CRYPTO_INCLUDE_DIR
                            NGTCP2_CRYPTO_OSSL_INCLUDE_DIR)
        list(APPEND _lines "${_entry}=${prefix}/.*")
    endforeach()
    set(${out_var} ${_lines} PARENT_SCOPE)
endfunction()
function(use_generator generator) # sets GENERATOR and MAKE_PROGRAM in the caller for <generator>
    set(GENERATOR "${generator}" PARENT_SCOPE)
    if(generator STREQUAL "Ninja")
        set(MAKE_PROGRAM "${_ninja}" PARENT_SCOPE)
    else()
        set(MAKE_PROGRAM "" PARENT_SCOPE)
    endif()
endfunction()
# The remedy qb's OpenSSL check names works (Huly QB-1190): a search cleared in part -- one entry removed, OPENSSL_ROOT_DIR
# moved -- stops the configure; all three entries removed, or --fresh, find one OpenSSL where OPENSSL_ROOT_DIR points.
if(_posix)
    foreach(_remedy unset fresh)
        set(_name "remedy-${_remedy}")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        plant_openssl("${_root}/o3" 3 a)
        plant_openssl("${_root}/o4" 4 a)
        set(_build "${WORK_DIR}/${_name}/build")
        configure_fixture(${_name} "${_build}" OK "-DQB_WITH_QUIC=OFF" "-DOPENSSL_ROOT_DIR=/o3"
                          HAS "OPENSSL=TRUE" "OPENSSL_READ=3\\|3")
        configure_fixture(${_name} "${_build}" FAIL "-U" "OPENSSL_SSL_LIBRARY" "-DOPENSSL_ROOT_DIR=/o4"
                          MUST "${_split}${_root_regex}/o3/include\\) holds the headers of OpenSSL 3\\.0\\.0, for libraries of major 3; OPENSSL_SSL_LIBRARY \\(${_root_regex}/o4/lib/libssl${_ext}\\) is a libssl of major 4; and OPENSSL_CRYPTO_LIBRARY \\(${_root_regex}/o3/lib/libcrypto${_ext}\\) is a libcrypto of major 3\\.")
        if(_remedy STREQUAL "unset")
            set(_args "-U" "OPENSSL_INCLUDE_DIR" "-U" "OPENSSL_SSL_LIBRARY" "-U" "OPENSSL_CRYPTO_LIBRARY")
        else()
            set(_args "--fresh" "-DQB_WITH_QUIC=OFF")
        endif()
        configure_fixture(${_name} "${_build}" OK ${_args} "-DOPENSSL_ROOT_DIR=/o4"
                          NOT "not one major"
                          HAS "OPENSSL=TRUE" "OPENSSL_READ=4\\|4" "OPENSSL_INCLUDE_DIR=${_root_regex}/o4/include"
                              "OPENSSL_SSL_LIBRARY=${_root_regex}/o4/lib/libssl${_ext}"
                              "OPENSSL_CRYPTO_LIBRARY=${_root_regex}/o4/lib/libcrypto${_ext}")
    endforeach()
endif()
# The openssl command that makes the test certificates is the found OpenSSL's when it makes one, before one on PATH or
# under CMAKE_PREFIX_PATH; searched at every configure, it follows qb onto another OpenSSL, and falls back to the
# default search -- then PATH's -- once the prefix has none, or one that answers `version` but cannot make a
# certificate; cross-compiling, it never takes the target root's, nor one under CMAKE_PREFIX_PATH that does not run
# here. An OPENSSL_EXECUTABLE already set -- by hand (-DOPENSSL_EXECUTABLE), by a parent's variable, by a
# find_program() -- is taken as given, a find_program() entry said once, an empty one as no command; qb's choice is
# kept as the entry, which a later find_program() takes and the next configure chooses again (Huly QB-1175).
if(_posix)
    set(_name "openssl-command")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    escape("${WORK_DIR}/${_name}/path" _path_regex)
    plant_openssl("${_root}/qb-openssl" 3 a)
    # An openssl stand-in in <dir>: `req` (what makes a certificate) exits <req_rc>, default <rc>; anything else <rc>.
    function(command dir rc)
        set(_req_rc ${rc})
        if(ARGC GREATER 2)
            set(_req_rc ${ARGV2})
        endif()
        file(WRITE "${dir}/openssl" "#!/bin/sh\nif [ \"$1\" = req ]; then exit ${_req_rc}; fi\nexit ${rc}\n")
        file(CHMOD "${dir}/openssl" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    endfunction()
    command("${_root}/qb-openssl/bin" 0)
    command("${WORK_DIR}/${_name}/path" 0)
    command("${_root}/other/bin" 0)
    # The default search takes CMAKE_PREFIX_PATH and CMAKE_PROGRAM_PATH from the environment too, ahead of PATH, and
    # the GitHub macOS runner exports one naming Homebrew's OpenSSL: the cases name their own, the environment's are
    # cleared.
    set(_env "--unset=CMAKE_PREFIX_PATH" "--unset=CMAKE_PROGRAM_PATH" "PATH=${WORK_DIR}/${_name}/path:$ENV{PATH}")
    set(_build "${WORK_DIR}/${_name}/build")
    configure_fixture(${_name} "${_build}" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF"
                      "-DCMAKE_PREFIX_PATH=${_root}/other"
                      HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE"
                          "OPENSSL_EXECUTABLE_ENTRY=${_root_regex}/qb-openssl/bin/openssl") # qb's choice, kept
    command("${_root}/qb-openssl/bin" 1)
    configure_fixture(${_name} "${_build}" OK HAS "OPENSSL_COMMAND=${_root_regex}/other/bin/openssl") # the default search
    configure_fixture(${_name} "${_build}" OK "-DCMAKE_PREFIX_PATH=" HAS "OPENSSL_COMMAND=${_path_regex}/openssl")
    command("${_root}/qb-openssl/bin" 0 1) # answers `version`, cannot make a certificate (no openssl.cnf)
    configure_fixture(${_name} "${_build}" OK HAS "OPENSSL_COMMAND=${_path_regex}/openssl")
    command("${_root}/qb-openssl/bin" 0)
    configure_fixture(${_name} "${_build}" OK HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl")
    file(REMOVE "${_root}/qb-openssl/bin/openssl")
    configure_fixture(${_name} "${_build}" OK HAS "OPENSSL_COMMAND=${_path_regex}/openssl")
    configure_fixture(${_name} "${_build}" OK "-DOPENSSL_EXECUTABLE=/by/hand/openssl"
                      HAS "OPENSSL_COMMAND=/by/hand/openssl" "OPENSSL_COMMAND_BY_HAND=TRUE")
    # Named by hand as PATH's -- what qb's Linux CI lanes pass, `command -v openssl` -- is still named.
    command("${_root}/qb-openssl/bin" 0)
    configure_fixture(${_name} "${_build}" OK "-DOPENSSL_EXECUTABLE=${WORK_DIR}/${_name}/path/openssl"
                      HAS "OPENSSL_COMMAND=${_path_regex}/openssl" "OPENSSL_COMMAND_BY_HAND=TRUE")
    # qb moved onto another OpenSSL (the three entries removed, as QB-1190's remedy says): its command follows.
    set(_build "${WORK_DIR}/${_name}/moved")
    plant_openssl("${_root}/next-openssl" 3 b)
    command("${_root}/next-openssl/bin" 0)
    configure_fixture(${_name} "${_build}" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF"
                      HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl")
    configure_fixture(${_name} "${_build}" OK "-DOPENSSL_ROOT_DIR=/next-openssl" "-U" "OPENSSL_INCLUDE_DIR"
                      "-U" "OPENSSL_SSL_LIBRARY" "-U" "OPENSSL_CRYPTO_LIBRARY"
                      HAS "OPENSSL_COMMAND=${_root_regex}/next-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE")
    # A parent project's own find_program(OPENSSL_EXECUTABLE) is taken as given, as before -- with a command named by
    # hand, which that find_program() keeps under its own help string -- and its entry is left to it.
    configure_fixture(${_name} "${WORK_DIR}/${_name}/parent-search" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      "-DQB_WITH_QUIC=OFF" "-DPARENT_FINDS_OPENSSL=ON"
                      MUST "OPENSSL_EXECUTABLE \\(${_path_regex}/openssl\\) is a find_program\\(\\) entry"
                      HAS "OPENSSL_COMMAND=${_path_regex}/openssl" "OPENSSL_COMMAND_BY_HAND=TRUE"
                          "OPENSSL_EXECUTABLE_ENTRY=${_path_regex}/openssl")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/parent-named" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      "-DQB_WITH_QUIC=OFF" "-DPARENT_FINDS_OPENSSL=ON" "-DOPENSSL_EXECUTABLE=/by/hand/openssl"
                      HAS "OPENSSL_COMMAND=/by/hand/openssl" "OPENSSL_COMMAND_BY_HAND=TRUE")
    # A consumer's find_program(OPENSSL_EXECUTABLE) after qb takes qb's choice, at every configure.
    configure_fixture(${_name} "${WORK_DIR}/${_name}/late" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF"
                      "-DLATE_FINDS_OPENSSL=ON"
                      HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE"
                          "LATE_FOUND=${_root_regex}/qb-openssl/bin/openssl")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/late" OK NOT "is a find_program\\(\\) entry"
                      HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE"
                          "LATE_FOUND=${_root_regex}/qb-openssl/bin/openssl")
    # An empty OPENSSL_EXECUTABLE makes no certificates, as find_program() kept it; NOTFOUND is none, searched for.
    configure_fixture(${_name} "${WORK_DIR}/${_name}/empty" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF"
                      "-DOPENSSL_EXECUTABLE=" HAS "OPENSSL_COMMAND=" "OPENSSL_COMMAND_BY_HAND=TRUE")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/notfound" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      "-DQB_WITH_QUIC=OFF" "-DOPENSSL_EXECUTABLE=NOTFOUND"
                      HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE"
                          "OPENSSL_EXECUTABLE_ENTRY=NOTFOUND") # another's entry, even NOTFOUND, left as it is
    # A parent project's find_program() that finds nothing: its NOTFOUND entry is left to it, at every configure.
    foreach(_time 1 2)
        configure_fixture(${_name} "${WORK_DIR}/${_name}/parent-nothing" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                          "-DQB_WITH_QUIC=OFF" "-DPARENT_FINDS_NOTHING=ON"
                          HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE"
                              "OPENSSL_EXECUTABLE_ENTRY=OPENSSL_EXECUTABLE-NOTFOUND")
    endforeach()
    # qb's choice edited in place -- ccmake, cmake-gui, an edited CMakeCache.txt keep qb's help string -- is named.
    set(_build "${WORK_DIR}/${_name}/edited")
    configure_fixture(${_name} "${_build}" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF"
                      HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl")
    file(READ "${_build}/CMakeCache.txt" _cache)
    string(REGEX REPLACE "\nOPENSSL_EXECUTABLE:FILEPATH=[^\n]*\n" "\nOPENSSL_EXECUTABLE:FILEPATH=/edited/openssl\n"
           _cache "${_cache}")
    file(WRITE "${_build}/CMakeCache.txt" "${_cache}")
    configure_fixture(${_name} "${_build}" OK
                      HAS "OPENSSL_COMMAND=/edited/openssl" "OPENSSL_COMMAND_BY_HAND=TRUE")
    # A variable that names no command, over another's cache entry: qb chooses, and leaves that entry as it is.
    configure_fixture(${_name} "${WORK_DIR}/${_name}/over-entry" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      "-DQB_WITH_QUIC=OFF" "-DOPENSSL_EXECUTABLE=/by/hand/openssl"
                      "-DPARENT_OPENSSL_EXECUTABLE=OPENSSL_EXECUTABLE-NOTFOUND"
                      HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE"
                          "OPENSSL_EXECUTABLE_ENTRY=/by/hand/openssl")
    # A parent project's variable is taken as given, as find_program() took it.
    configure_fixture(${_name} "${WORK_DIR}/${_name}/parent" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF"
                      "-DPARENT_OPENSSL_EXECUTABLE=/parent/openssl"
                      HAS "OPENSSL_COMMAND=/parent/openssl" "OPENSSL_COMMAND_BY_HAND=TRUE")
    # A build directory the old generator configured holds its find_program() entry -- PATH's it found, or a command
    # named by hand it kept, given the same help string: taken as given, as find_program() takes it, and said once,
    # with -U OPENSSL_EXECUTABLE to let qb choose; one that found nothing is no command, and qb's is searched for.
    set(_name "openssl-command-cached")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_openssl("${_root}/qb-openssl" 3 a)
    command("${_root}/qb-openssl/bin" 0)
    foreach(_old "${WORK_DIR}/openssl-command/path/openssl" "/by/hand/openssl")
        string(MAKE_C_IDENTIFIER "${_old}" _case)
        file(WRITE "${WORK_DIR}/${_name}/${_case}.cmake"
             "set(OPENSSL_EXECUTABLE \"${_old}\" CACHE FILEPATH \"Path to a program.\")\n")
        escape("${_old}" _old_regex)
        set(_said "OPENSSL_EXECUTABLE \\(${_old_regex}\\) is a find_program\\(\\) entry -- [^:]* -- taken as given")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/${_case}" OK -C "${WORK_DIR}/${_name}/${_case}.cmake"
                          "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF" MUST "${_said}"
                          HAS "OPENSSL_COMMAND=${_old_regex}" "OPENSSL_COMMAND_BY_HAND=TRUE"
                              "OPENSSL_EXECUTABLE_ENTRY=${_old_regex}")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/${_case}" OK NOT "is a find_program\\(\\) entry"
                          HAS "OPENSSL_COMMAND=${_old_regex}")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/${_case}" OK "-U" "OPENSSL_EXECUTABLE"
                          HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE"
                              "OPENSSL_EXECUTABLE_ENTRY=${_root_regex}/qb-openssl/bin/openssl")
    endforeach()
    file(WRITE "${WORK_DIR}/${_name}/notfound.cmake"
         "set(OPENSSL_EXECUTABLE \"OPENSSL_EXECUTABLE-NOTFOUND\" CACHE FILEPATH \"Path to a program.\")\n")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/notfound" OK -C "${WORK_DIR}/${_name}/notfound.cmake"
                      "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF" NOT "is a find_program\\(\\) entry"
                      HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE"
                          "OPENSSL_EXECUTABLE_ENTRY=OPENSSL_EXECUTABLE-NOTFOUND")
    # No command makes a certificate here, with a consumer's find_program() after qb: qb keeps an empty entry, which
    # that find_program() keeps -- the same at every configure, none -- and chooses again once one does.
    set(_name "openssl-command-none")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_openssl("${_root}/qb-openssl" 3 a)
    command("${_root}/qb-openssl/bin" 0 1)
    command("${WORK_DIR}/${_name}/path" 0 1)
    set(_path_before "${_env}")
    set(_env "--unset=CMAKE_PREFIX_PATH" "--unset=CMAKE_PROGRAM_PATH" "PATH=${WORK_DIR}/${_name}/path:$ENV{PATH}")
    foreach(_time 1 2)
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF"
                          "-DLATE_FINDS_OPENSSL=ON" NOT "is a find_program\\(\\) entry"
                          HAS "OPENSSL_COMMAND=" "OPENSSL_COMMAND_BY_HAND=FALSE" "OPENSSL_EXECUTABLE_ENTRY="
                              "LATE_FOUND=")
    endforeach()
    command("${_root}/qb-openssl/bin" 0)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK
                      HAS "OPENSSL_COMMAND=${_root_regex}/qb-openssl/bin/openssl" "OPENSSL_COMMAND_BY_HAND=FALSE"
                          "LATE_FOUND=${_root_regex}/qb-openssl/bin/openssl")
    set(_env "${_path_before}")
    # Cross-compiling: the found OpenSSL's command is the target's, and may not run here; nor is the target root's
    # taken, whatever the toolchain's CMAKE_FIND_ROOT_PATH_MODE_PROGRAM says, nor one under CMAKE_PREFIX_PATH that does
    # not run.
    set(_name "openssl-command-cross")
    set(_root "${WORK_DIR}/${_name}/root")
    plant_openssl("${_root}/qb-openssl" 3 a)
    command("${_root}/qb-openssl/bin" 0)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DCMAKE_SYSTEM_NAME=FreeBSD" "-DCMAKE_READELF=${_tool}"
                      "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF"
                      HAS "OPENSSL=TRUE" "OPENSSL_COMMAND=${_path_regex}/openssl")
    command("${_root}/usr/bin" 0)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/both" OK "-DCMAKE_SYSTEM_NAME=FreeBSD" "-DCMAKE_READELF=${_tool}"
                      "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF" "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=BOTH"
                      HAS "OPENSSL=TRUE" "OPENSSL_COMMAND=${_path_regex}/openssl")
    command("${_root}/target/bin" 126) # a binary for the target: not executable here
    configure_fixture(${_name} "${WORK_DIR}/${_name}/prefix" OK "-DCMAKE_SYSTEM_NAME=FreeBSD" "-DCMAKE_READELF=${_tool}"
                      "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DQB_WITH_QUIC=OFF" "-DCMAKE_PREFIX_PATH=${_root}/target"
                      HAS "OPENSSL=TRUE" "OPENSSL_COMMAND=${_path_regex}/openssl")
    set(_env "")
endif()
foreach(_generator IN LISTS _generators)
    use_generator("${_generator}")
    string(REPLACE " " "-" _name "rebuild-${_generator}")
    set(_root "${WORK_DIR}/${_name}/root")
    set(_opt "${_root}/opt/homebrew/opt/libngtcp2")
    set(_ssl "${_root}/qb-openssl")
    escape("${_root}" _root_regex)
    plant_keg("${_root}/Cellar/libngtcp2/keg-1" 3)
    point("${_opt}" "${_root}/Cellar/libngtcp2/keg-1")
    plant_openssl("${_ssl}" 3 a)
    entries(_at_opt "${_root_regex}/opt/homebrew/opt/libngtcp2")
    set(_crypto_found "OPENSSL_CRYPTO_LIBRARY=${_root_regex}/qb-openssl/lib/libcrypto${_ext}")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      HAS "VERDICT=" "QB_HAS_QUIC=TRUE" ${_at_opt} "${_crypto_found}")
    build_fixture(${_name} "nothing changed" 1 "")
    after_configure(${_name})
    plant("${_root}/Cellar/libngtcp2/keg-1/lib/libngtcp2_crypto_ossl${_ext}" 4 4)
    build_fixture(${_name} "libngtcp2 rebuilt in place against OpenSSL 4" 2 MAJORS "QB_HAS_QUIC=FALSE")
    build_fixture(${_name} "nothing changed since" 2 MAJORS)
    # An upgrade onto a keg whose files keep older times, the old keg removed: only the file read, gone, can tell.
    plant_keg("${_root}/Cellar/libngtcp2/keg-2" 3)
    backdate("${_root}/Cellar/libngtcp2/keg-2/lib/libngtcp2_crypto_ossl${_ext}")
    point("${_opt}" "${_root}/Cellar/libngtcp2/keg-2")
    file(REMOVE_RECURSE "${_root}/Cellar/libngtcp2/keg-1")
    build_fixture(${_name} "opt/ moved onto an older keg, the one read removed" 3 "" "QB_HAS_QUIC=TRUE" ${_at_opt})
    build_fixture(${_name} "nothing changed since" 3 "")
    # qb's libssl link moved onto an older file, the one it named removed: the same, on qb's side.
    plant("${_ssl}/lib/libssl.b" 3 3)
    backdate("${_ssl}/lib/libssl.b")
    point("${_ssl}/lib/libssl${_ext}" "${_ssl}/lib/libssl.b")
    file(REMOVE "${_ssl}/lib/libssl.a")
    build_fixture(${_name} "qb's libssl link moved onto an older file, the one read removed" 4 "")
    build_fixture(${_name} "nothing changed since" 4 "")
    # qb's own OpenSSL rewritten onto another major, its libcrypto then its headers: qb's check stops the configure
    # (Huly QB-1190), and keeps stopping until the file is back.
    after_configure(${_name})
    plant("${_ssl}/lib/libcrypto.a" "" 4)
    build_fixture(${_name} "qb's libcrypto rewritten onto OpenSSL 4" FAILS ""
                  OUT "qb's OpenSSL is not one major: .* is a libcrypto of major 4\\.")
    build_fixture(${_name} "built again, nothing done" FAILS "" OUT "qb's OpenSSL is not one major")
    plant("${_ssl}/lib/libcrypto.a" "" 3)
    build_fixture(${_name} "qb's libcrypto back on OpenSSL 3" 5 "")
    build_fixture(${_name} "nothing changed since" 5 "")
    after_configure(${_name})
    file(WRITE "${_ssl}/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"4.0.1\"\n")
    build_fixture(${_name} "qb's opensslv.h rewritten onto OpenSSL 4" FAILS ""
                  OUT "qb's OpenSSL is not one major: OPENSSL_INCLUDE_DIR \\([^)]*\\) holds the headers of OpenSSL 4\\.0\\.1")
    file(WRITE "${_ssl}/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"3.0.1\"\n")
    build_fixture(${_name} "qb's opensslv.h back on OpenSSL 3" 6 "")
    build_fixture(${_name} "nothing changed since" 6 "")
    # qb's libcrypto removed: what its search found is searched again, found nowhere else: the configure stops and names
    # it -- a build directory that had OpenSSL does not go on without it -- until it is back.
    file(REMOVE "${_ssl}/lib/libcrypto${_ext}" "${_ssl}/lib/libcrypto.a")
    build_fixture(${_name} "qb's libcrypto removed" FAILS ""
                  OUT "OPENSSL_CRYPTO_LIBRARY \\(${_root_regex}/qb-openssl/lib/libcrypto[^)]*\\) no longer exists: searched again"
                      "the OpenSSL this build directory found is gone and was found nowhere else -- OPENSSL_CRYPTO_LIBRARY")
    build_fixture(${_name} "built again, nothing done" FAILS ""
                  OUT "the OpenSSL this build directory found is gone and was found nowhere else -- OPENSSL_CRYPTO_LIBRARY")
    plant_openssl("${_ssl}" 3 c)
    build_fixture(${_name} "qb's libcrypto back" 7 "" "OPENSSL=TRUE" "${_crypto_found}")
    build_fixture(${_name} "nothing changed since" 7 "")
    # libngtcp2 removed: every entry its search found is searched again, found nowhere else: the configure stops.
    file(REMOVE_RECURSE "${_root}/Cellar/libngtcp2/keg-2")
    file(REMOVE "${_opt}")
    build_fixture(${_name} "libngtcp2 removed" FAILS ""
                  OUT "NGTCP2_CRYPTO_OSSL_LIBRARY \\(${_root_regex}/opt/homebrew/opt/libngtcp2/lib/[^)]*\\) no longer exists: searched again"
                      "the libngtcp2 this build directory found is gone and was found nowhere else -- NGTCP2_")
    build_fixture(${_name} "built again, nothing done" FAILS ""
                  OUT "the libngtcp2 this build directory found is gone and was found nowhere else -- NGTCP2_")
    plant_keg("${_root}/Cellar/libngtcp2/keg-3" 3)
    point("${_opt}" "${_root}/Cellar/libngtcp2/keg-3")
    build_fixture(${_name} "libngtcp2 back" 8 "" "NGTCP2=TRUE" ${_at_opt})
    build_fixture(${_name} "nothing changed since" 8 "")
endforeach()
if(_generators)
    list(GET _generators 0 _generator)
    use_generator("${_generator}")
    # The Cellar shape, as pkg-config gives it: the searches find a versioned keg's own files. An upgrade puts the new
    # version elsewhere and removes the old keg: every entry is searched again and found there.
    set(_name "found-elsewhere")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_keg("${_root}/Cellar/libngtcp2/keg-1" 3)
    plant_openssl("${_root}/usr/local" 3 a)
    entries(_in_keg "${_root_regex}/Cellar/libngtcp2/keg-1")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DCMAKE_PREFIX_PATH=/Cellar/libngtcp2/keg-1"
                      "-DFIND_OPENSSL=ON"
                      HAS "VERDICT=" ${_in_keg} "OPENSSL_SSL_LIBRARY=${_root_regex}/usr/local/lib/libssl${_ext}")
    build_fixture(${_name} "nothing changed" 1 "")
    plant_keg("${_root}/Cellar/libngtcp2/keg-2" 3)
    point("${_root}/opt/homebrew/opt/libngtcp2" "${_root}/Cellar/libngtcp2/keg-2")
    plant_openssl("${_root}/usr" 3 b)
    file(REMOVE_RECURSE "${_root}/Cellar/libngtcp2/keg-1" "${_root}/usr/local")
    entries(_at_opt "${_root_regex}/opt/homebrew/opt/libngtcp2")
    build_fixture(${_name} "both upgraded elsewhere, the old ones removed" 2 "" "NGTCP2=TRUE" ${_at_opt}
                  "OPENSSL_SSL_LIBRARY=${_root_regex}/usr/lib/libssl${_ext}"
                  "OPENSSL_CRYPTO_LIBRARY=${_root_regex}/usr/lib/libcrypto${_ext}"
                  "OPENSSL_INCLUDE_DIR=${_root_regex}/usr/include"
                  OUT "OPENSSL_SSL_LIBRARY \\([^)]*\\) no longer exists: searched again")
    build_fixture(${_name} "nothing changed since" 2 "")
    # Removed again: the record followed the library to where the upgrade put it, and the stop names that path.
    file(REMOVE_RECURSE "${_root}/usr")
    build_fixture(${_name} "the upgraded OpenSSL removed in turn" FAILS ""
                  OUT "found nowhere else -- OPENSSL_INCLUDE_DIR \\(${_root_regex}/usr/include\\), OPENSSL_SSL_LIBRARY \\(${_root_regex}/usr/lib/libssl")
    plant_openssl("${_root}/usr/local" 3 c)
    build_fixture(${_name} "an OpenSSL back elsewhere" 3 "" "OPENSSL=TRUE"
                  "OPENSSL_SSL_LIBRARY=${_root_regex}/usr/local/lib/libssl${_ext}")
    build_fixture(${_name} "nothing changed since" 3 "")
    # Moved again, but the new place has no libssl: FindOpenSSL still finds OpenSSL (it requires libcrypto and the
    # headers only), and the stop names libssl alone -- not the libcrypto and headers found again.
    file(REMOVE_RECURSE "${_root}/usr/local")
    plant_openssl("${_root}/usr" 3 d)
    file(REMOVE "${_root}/usr/lib/libssl${_ext}" "${_root}/usr/lib/libssl.d")
    build_fixture(${_name} "moved where there is no libssl" FAILS ""
                  OUT "found nowhere else -- OPENSSL_SSL_LIBRARY \\(${_root_regex}/usr/local/lib/libssl"
                  NOTOUT "found nowhere else -- [^:]*OPENSSL_CRYPTO_LIBRARY \\(" "found nowhere else -- [^:]*OPENSSL_INCLUDE_DIR \\(")
    plant_openssl("${_root}/usr" 3 e)
    build_fixture(${_name} "its libssl back" 4 "" "OPENSSL=TRUE" "OPENSSL_SSL_LIBRARY=${_root_regex}/usr/lib/libssl${_ext}")
    build_fixture(${_name} "nothing changed since" 4 "")
    # A search that finds libssl but no libcrypto finds no OpenSSL -- and still records the libssl it found: once
    # libcrypto is there and libssl is removed, the stop names it.
    set(_name "partial-first")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_openssl("${_root}/usr" 3 a)
    file(REMOVE "${_root}/usr/lib/libcrypto${_ext}" "${_root}/usr/lib/libcrypto.a")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON"
                      HAS "OPENSSL=FALSE" "OPENSSL_SSL_LIBRARY=${_root_regex}/usr/lib/libssl${_ext}")
    plant_openssl("${_root}/usr" 3 a)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" HAS "OPENSSL=TRUE")
    file(REMOVE "${_root}/usr/lib/libssl${_ext}" "${_root}/usr/lib/libssl.a")
    build_fixture(${_name} "the libssl found first removed" FAILS ""
                  OUT "found nowhere else -- OPENSSL_SSL_LIBRARY \\(${_root_regex}/usr/lib/libssl")
    # A package removal that takes OpenSSL's headers from an include directory that stays -- a shared prefix -- leaves
    # OPENSSL_INCLUDE_DIR naming a directory without openssl/ssl.h: searched again too, and found where OpenSSL now is.
    set(_name "shared-include")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_openssl("${_root}/usr/local" 3 a)
    file(WRITE "${_root}/usr/local/include/other.h" "")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                      HAS "OPENSSL=TRUE" "OPENSSL_INCLUDE_DIR=${_root_regex}/usr/local/include")
    file(REMOVE_RECURSE "${_root}/usr/local/include/openssl")
    file(REMOVE "${_root}/usr/local/lib/libssl${_ext}" "${_root}/usr/local/lib/libssl.a"
         "${_root}/usr/local/lib/libcrypto${_ext}" "${_root}/usr/local/lib/libcrypto.a")
    plant_openssl("${_root}/usr" 3 b)
    build_fixture(${_name} "OpenSSL removed from the shared prefix, installed elsewhere" 2 "" "OPENSSL=TRUE"
                  "OPENSSL_INCLUDE_DIR=${_root_regex}/usr/include"
                  "OPENSSL_SSL_LIBRARY=${_root_regex}/usr/lib/libssl${_ext}"
                  OUT "OPENSSL_INCLUDE_DIR \\(${_root_regex}/usr/local/include\\) no longer holds openssl/ssl\\.h: searched again")
    build_fixture(${_name} "nothing changed since" 2 "")
    # Headers of an OpenSSL this build directory never had a library for -- strays in a shared prefix -- are searched
    # again once removed, and stop nothing (Huly QB-1192).
    set(_name "never-had")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    file(WRITE "${_root}/usr/local/include/openssl/ssl.h" "")
    file(WRITE "${_root}/usr/local/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"3.0.0\"\n")
    file(WRITE "${_root}/usr/local/include/other.h" "")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                      HAS "OPENSSL=FALSE" "OPENSSL_INCLUDE_DIR=${_root_regex}/usr/local/include")
    file(REMOVE_RECURSE "${_root}/usr/local/include/openssl")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                      MUST "OPENSSL_INCLUDE_DIR \\(${_root_regex}/usr/local/include\\) no longer holds openssl/ssl\\.h: searched again"
                      NOT "is gone" HAS "OPENSSL=FALSE")
    # An OpenSSL whose headers are an Apple framework: find_path() gives the framework, whose headers are under
    # Headers -- recorded and searched again like any other.
    if(APPLE)
        set(_name "framework-include")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        set(_framework "${_root_regex}/Library/Frameworks/openssl\\.framework")
        plant_openssl("${_root}/usr/local" 3 a)
        file(MAKE_DIRECTORY "${_root}/Library/Frameworks/openssl.framework")
        file(RENAME "${_root}/usr/local/include/openssl" "${_root}/Library/Frameworks/openssl.framework/Headers")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                          HAS "OPENSSL=TRUE" "OPENSSL_INCLUDE_DIR=${_framework}")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" NOT "kept as given"
                          HAS "OPENSSL=TRUE")
        file(REMOVE_RECURSE "${_root}/Library/Frameworks/openssl.framework")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" FAIL "-DFIND_OPENSSL=ON"
                          MUST "OPENSSL_INCLUDE_DIR \\(${_framework}\\) no longer exists: searched again"
                               "the OpenSSL this build directory found is gone and was found nowhere else -- OPENSSL_")
        # The same for libngtcp2's headers as a framework.
        set(_name "framework-include-ngtcp2")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        set(_framework "${_root_regex}/Library/Frameworks/ngtcp2\\.framework")
        plant_keg("${_root}/usr/local" 3)
        plant_openssl("${_root}/qb-openssl" 3 a)
        file(MAKE_DIRECTORY "${_root}/Library/Frameworks/ngtcp2.framework")
        file(RENAME "${_root}/usr/local/include/ngtcp2" "${_root}/Library/Frameworks/ngtcp2.framework/Headers")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                          HAS "NGTCP2=TRUE" "NGTCP2_INCLUDE_DIR=${_framework}")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK NOT "kept as given" HAS "NGTCP2=TRUE")
        file(REMOVE_RECURSE "${_root}/Library/Frameworks/ngtcp2.framework")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" FAIL
                          MUST "NGTCP2_INCLUDE_DIR \\(${_framework}\\) no longer exists: searched again"
                               "the libngtcp2 this build directory found is gone and was found nowhere else -- NGTCP2_")
    endif()
    # A build directory QB-1185's code configured recorded the libraries its search found, not that it had OpenSSL: its
    # records stand in for that, and its OpenSSL lost still stops the configure.
    set(_name "recorded-before")
    set(_root "${WORK_DIR}/${_name}/root")
    plant_openssl("${_root}/usr/local" 3 a)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF" HAS "OPENSSL=TRUE")
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" FAIL "-DFIND_OPENSSL=ON" "-U" "_QB_HAD_OpenSSL"
                      MUST "the OpenSSL this build directory found is gone and was found nowhere else")
    # Libraries of an OpenSSL this build directory never found -- no headers beside them -- are recorded too, and their
    # removal stops nothing, however many configures saw them; nor does an OpenSSL it had once its search is turned off.
    set(_name "openssl-never-found")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_openssl("${_root}/usr/local" 3 a)
    file(REMOVE_RECURSE "${_root}/usr/local/include")
    foreach(_time 1 2)
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                          HAS "OPENSSL=FALSE" "OPENSSL_SSL_LIBRARY=${_root_regex}/usr/local/lib/libssl${_ext}")
    endforeach()
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                      MUST "OPENSSL_SSL_LIBRARY \\(${_root_regex}/usr/local/lib/libssl[^)]*\\) no longer exists: searched again"
                      NOT "is gone" HAS "OPENSSL=FALSE")
    plant_openssl("${_root}/usr/local" 3 b)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF" HAS "OPENSSL=TRUE")
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" FAIL "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                      MUST "the OpenSSL this build directory found is gone")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                      "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON" NOT "is gone" HAS "OPENSSL=") # nothing searched
    # Turned off by the configure that sees the loss: nothing is searched again, nor said.
    plant_openssl("${_root}/usr/local" 3 c)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/at-once" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                      HAS "OPENSSL=TRUE")
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/at-once" OK "-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON"
                      NOT "is gone" "searched again" HAS "OPENSSL=")
    # The same for libngtcp2 (Huly QB-1192): its headers and core library without its crypto helper are no libngtcp2,
    # and their removal stops nothing; a build directory its earlier search configured had it when every entry finding
    # it needs was recorded or set.
    set(_name "ngtcp2-never-found")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_keg("${_root}/usr/local" 3)
    file(REMOVE "${_root}/usr/local/lib/libngtcp2_crypto_ossl${_ext}")
    plant_openssl("${_root}/qb-openssl" 3 a)
    foreach(_time 1 2)
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                          HAS "NGTCP2=FALSE" "NGTCP2_LIBRARY=${_root_regex}/usr/local/lib/libngtcp2${_ext}")
    endforeach()
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      MUST "NGTCP2_LIBRARY \\(${_root_regex}/usr/local/lib/libngtcp2[^)]*\\) no longer exists: searched again"
                      NOT "is gone" HAS "NGTCP2=FALSE" "NGTCP2_VANISHED=")
    plant_keg("${_root}/usr/local" 3)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" HAS "NGTCP2=TRUE")
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" FAIL "-DOPENSSL_ROOT_DIR=/qb-openssl" "-U" "_NGTCP2_HAD"
                      "-U" "_QB_HAD_QUIC"
                      MUST "the libngtcp2 this build directory found is gone and was found nowhere else -- NGTCP2_")
    # A build directory the earlier searches configured recorded what they found, not that they found a package: it
    # had an OpenSSL when its headers and libcrypto were each recorded or set, a libngtcp2 when its five entries were --
    # libraries without the rest never counted (_QB_HAD_OpenSSL and _NGTCP2_HAD removed stand for such a directory).
    set(_name "partial-recorded-before")
    set(_root "${WORK_DIR}/${_name}/root")
    plant_openssl("${_root}/usr/local" 3 a)
    file(REMOVE_RECURSE "${_root}/usr/local/include")
    plant_keg("${_root}/usr/local" 3)
    file(REMOVE "${_root}/usr/local/lib/libngtcp2_crypto_ossl${_ext}")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" HAS "OPENSSL=FALSE" "NGTCP2=FALSE")
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON"
                      "-U" "_QB_HAD_OpenSSL" "-U" "_NGTCP2_HAD"
                      MUST "OPENSSL_SSL_LIBRARY \\([^)]*\\) no longer exists: searched again"
                           "NGTCP2_LIBRARY \\([^)]*\\) no longer exists: searched again"
                      NOT "is gone" HAS "OPENSSL=FALSE" "NGTCP2=FALSE" "NGTCP2_VANISHED=")
    # ... headers and libssl without libcrypto never were an OpenSSL either (FindOpenSSL requires libcrypto):
    set(_name "no-libcrypto-recorded-before")
    set(_root "${WORK_DIR}/${_name}/root")
    plant_openssl("${_root}/usr/local" 3 a)
    file(REMOVE "${_root}/usr/local/lib/libcrypto${_ext}" "${_root}/usr/local/lib/libcrypto.a")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                      HAS "OPENSSL=FALSE" "OPENSSL_SSL_LIBRARY=.*/usr/local/lib/libssl.*")
    file(REMOVE "${_root}/usr/local/lib/libssl${_ext}" "${_root}/usr/local/lib/libssl.a")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-U" "_QB_HAD_OpenSSL"
                      MUST "OPENSSL_SSL_LIBRARY \\([^)]*\\) no longer exists: searched again" NOT "is gone"
                      HAS "OPENSSL=FALSE")
    # ... and an include directory set by hand, never recorded, counts as there: the libraries the search found, lost,
    # still stop such a directory.
    set(_name "hand-include-recorded-before")
    set(_root "${WORK_DIR}/${_name}/root")
    plant_openssl("${_root}/usr/local" 3 a)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/openssl" OK "-DFIND_OPENSSL=ON" "-DQB_WITH_QUIC=OFF"
                      "-DOPENSSL_INCLUDE_DIR=${_root}/usr/local/include" HAS "OPENSSL=TRUE")
    plant_keg("${_root}/Cellar/libngtcp2/keg-1" 3)
    plant_openssl("${_root}/qb-openssl" 3 b)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/ngtcp2" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      "-DCMAKE_PREFIX_PATH=/Cellar/libngtcp2/keg-1"
                      "-DNGTCP2_INCLUDE_DIR=${_root}/Cellar/libngtcp2/keg-1/include" HAS "NGTCP2=TRUE")
    file(REMOVE "${_root}/usr/local/lib/libssl${_ext}" "${_root}/usr/local/lib/libssl.a"
         "${_root}/usr/local/lib/libcrypto${_ext}" "${_root}/usr/local/lib/libcrypto.a"
         "${_root}/Cellar/libngtcp2/keg-1/lib/libngtcp2${_ext}"
         "${_root}/Cellar/libngtcp2/keg-1/lib/libngtcp2_crypto_ossl${_ext}")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/openssl" FAIL "-DFIND_OPENSSL=ON" "-U" "_QB_HAD_OpenSSL"
                      MUST "the OpenSSL this build directory found is gone and was found nowhere else")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/ngtcp2" FAIL "-U" "_NGTCP2_HAD"
                      MUST "the libngtcp2 this build directory found is gone and was found nowhere else -- NGTCP2_")
    # An include directory set by hand without its header is kept as given, and said once though the module runs twice.
    set(_name "ngtcp2-hand-include-twice")
    set(_root "${WORK_DIR}/${_name}/root")
    plant_keg("${_root}/usr/local" 3)
    plant_openssl("${_root}/qb-openssl" 3 a)
    file(MAKE_DIRECTORY "${_root}/hand")
    set(_said "NGTCP2_CRYPTO_INCLUDE_DIR \\([^)]*/hand\\) does not hold ngtcp2/ngtcp2_crypto\\.h")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" "-DFIND_NGTCP2_TWICE=ON"
                      "-DNGTCP2_CRYPTO_INCLUDE_DIR=${_root}/hand" MUST "${_said}" NOT "${_said}.*${_said}")
    # libngtcp2's core library is an input of the configure as well: removed alone, the next build re-runs it.
    set(_name "ngtcp2-core-removed")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_keg("${_root}/usr/local" 3)
    plant_openssl("${_root}/qb-openssl" 3 a)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl" HAS "NGTCP2=TRUE")
    build_fixture(${_name} "nothing changed" 1 "")
    file(REMOVE "${_root}/usr/local/lib/libngtcp2${_ext}")
    build_fixture(${_name} "libngtcp2's core library removed" FAILS ""
                  OUT "NGTCP2_LIBRARY \\(${_root_regex}/usr/local/lib/libngtcp2[^)]*\\) no longer exists: searched again")
    # ... and not when set by hand: a library a parent project or a user names is kept as given, and its rebuilds re-run
    # nothing.
    set(_name "ngtcp2-hand-library")
    set(_root "${WORK_DIR}/${_name}/root")
    plant_keg("${_root}/usr/local" 3)
    plant_openssl("${_root}/qb-openssl" 3 a)
    file(WRITE "${_root}/hand/libngtcp2${_ext}" "")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      "-DNGTCP2_LIBRARY=${_root}/hand/libngtcp2${_ext}" HAS "NGTCP2=TRUE")
    build_fixture(${_name} "nothing changed" 1 "")
    after_configure(${_name})
    file(WRITE "${_root}/hand/libngtcp2${_ext}" "rebuilt")
    build_fixture(${_name} "the library set by hand rebuilt" 1 "")
    # A libngtcp2 QUIC was off for -- it links another OpenSSL major than qb's -- is no loss once removed: the build
    # never linked it. FindNgtcp2 still reports it lost (its own record), as a consumer of an installed qb reads it.
    set(_name "majors-then-gone")
    set(_root "${WORK_DIR}/${_name}/root")
    plant_keg("${_root}/usr/local" 4)
    plant_openssl("${_root}/qb-openssl" 3 a)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      HAS "VERDICT=MAJORS" "QB_HAS_QUIC=FALSE")
    file(REMOVE_RECURSE "${_root}/usr/local")
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK NOT "is gone"
                      HAS "NGTCP2=FALSE" "QB_HAS_QUIC=FALSE" "NGTCP2_VANISHED=NGTCP2_[^\n]+")
    # A package removal that takes libngtcp2's header from an include directory that stays -- a shared prefix -- leaves
    # its include entry naming a directory without it: searched again, and found nowhere else, the libngtcp2 this build
    # directory had is gone, until it is found elsewhere. ngtcp2.h alone: each include entry's header is an input.
    set(_name "ngtcp2-shared-include")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_keg("${_root}/usr/local" 3)
    file(WRITE "${_root}/usr/local/include/other.h" "")
    plant_openssl("${_root}/qb-openssl" 3 a)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      HAS "NGTCP2=TRUE" "NGTCP2_INCLUDE_DIR=${_root_regex}/usr/local/include")
    file(REMOVE "${_root}/usr/local/include/ngtcp2/ngtcp2.h")
    # The header is an input of the configure: the next build re-runs it, which searches the header's directory again.
    build_fixture(${_name} "libngtcp2's headers removed from the shared prefix" FAILS ""
                  OUT "NGTCP2_INCLUDE_DIR \\(${_root_regex}/usr/local/include\\) no longer holds ngtcp2/ngtcp2\\.h: searched again"
                      "found nowhere else -- NGTCP2_INCLUDE_DIR \\(${_root_regex}/usr/local/include\\)")
    plant_keg("${_root}/usr" 3)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      HAS "NGTCP2=TRUE" "NGTCP2_INCLUDE_DIR=${_root_regex}/usr/include")
    # An upgrade that leaves libngtcp2 without its crypto helper: the entries found again are not named as gone.
    set(_name "partial-upgrade")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_keg("${_root}/Cellar/libngtcp2/keg-1" 3)
    plant("${_root}/lib/libssl" 3 3)
    plant("${_root}/lib/libcrypto" "" 3)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DCMAKE_PREFIX_PATH=/Cellar/libngtcp2/keg-1"
                      "-DOPENSSL_SSL_LIBRARY=${_root}/lib/libssl" "-DOPENSSL_CRYPTO_LIBRARY=${_root}/lib/libcrypto"
                      "-DOPENSSL_VERSION=3.5.9" HAS "NGTCP2=TRUE")
    build_fixture(${_name} "nothing changed" 1 "")
    plant_keg("${_root}/Cellar/libngtcp2/keg-2" 3)
    file(REMOVE "${_root}/Cellar/libngtcp2/keg-2/lib/libngtcp2_crypto_ossl${_ext}")
    point("${_root}/opt/homebrew/opt/libngtcp2" "${_root}/Cellar/libngtcp2/keg-2")
    file(REMOVE_RECURSE "${_root}/Cellar/libngtcp2/keg-1")
    build_fixture(${_name} "upgraded without its crypto helper, the old keg removed" FAILS ""
                  OUT "found nowhere else -- NGTCP2_CRYPTO_OSSL_LIBRARY \\(${_root_regex}/Cellar/libngtcp2/keg-1/"
                  NOTOUT "found nowhere else -- [^:]*NGTCP2_LIBRARY \\(" "found nowhere else -- [^:]*NGTCP2_INCLUDE_DIR \\(")
    # A path set by hand -- as a superbuild names a library it builds, then removes with its own clean -- is never
    # searched again: the configure re-runs, keeps it, says how to search again, and the next build re-runs nothing.
    set(_name "set-by-hand")
    set(_root "${WORK_DIR}/${_name}/root")
    escape("${_root}" _root_regex)
    plant_keg("${_root}/Cellar/libngtcp2/keg-1" 3)
    point("${_root}/opt/homebrew/opt/libngtcp2" "${_root}/Cellar/libngtcp2/keg-1")
    plant_openssl("${_root}/qb-openssl" 3 a)
    plant("${_root}/hand/helper" 3 3)
    plant("${_root}/hand/libssl" 3 3)
    plant("${_root}/hand/libcrypto" "" 3)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      "-DNGTCP2_CRYPTO_OSSL_LIBRARY=${_root}/hand/helper" "-DOPENSSL_SSL_LIBRARY=${_root}/hand/libssl"
                      "-DOPENSSL_CRYPTO_LIBRARY=${_root}/hand/libcrypto"
                      HAS "VERDICT=")
    file(REMOVE_RECURSE "${_root}/hand")
    build_fixture(${_name} "the libraries set by hand removed" 2 "" "NGTCP2=TRUE"
                  "NGTCP2_CRYPTO_OSSL_LIBRARY=${_root_regex}/hand/helper" "OPENSSL_SSL_LIBRARY=${_root_regex}/hand/libssl"
                  "OPENSSL_CRYPTO_LIBRARY=${_root_regex}/hand/libcrypto"
                  OUT "OPENSSL_CRYPTO_LIBRARY \\(${_root_regex}/hand/libcrypto\\) does not exist; this build directory's search did not find it .*: kept as given"
                      "OPENSSL_SSL_LIBRARY \\(${_root_regex}/hand/libssl\\) does not exist; this build directory's search did not find it .*: kept as given"
                      "NGTCP2_CRYPTO_OSSL_LIBRARY \\(${_root_regex}/hand/helper\\) does not exist; this build directory's search did not find it .*: kept as given")
    build_fixture(${_name} "nothing changed since" 2 "")
    # qb's OpenSSL is an input of the configure with no libngtcp2 at all.
    set(_name "openssl-only")
    set(_root "${WORK_DIR}/${_name}/root")
    set(_ssl "${_root}/qb-openssl")
    plant_openssl("${_ssl}" 3 a)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DOPENSSL_ROOT_DIR=/qb-openssl"
                      HAS "NGTCP2=FALSE" "OPENSSL=TRUE" "COMPARED=FALSE")
    build_fixture(${_name} "nothing changed" 1 "")
    after_configure(${_name})
    file(WRITE "${_ssl}/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"3.0.1\"\n")
    build_fixture(${_name} "opensslv.h rewritten" 2 "")
    plant("${_ssl}/lib/libssl.b" 3 3)
    backdate("${_ssl}/lib/libssl.b")
    point("${_ssl}/lib/libssl${_ext}" "${_ssl}/lib/libssl.b")
    file(REMOVE "${_ssl}/lib/libssl.a")
    build_fixture(${_name} "the libssl link moved onto an older file, the one read removed" 3 "")
    build_fixture(${_name} "nothing changed since" 3 "")
    # An entry set to NOTFOUND by hand, its recorded file still there, and a search narrowed to nowhere: nothing this
    # build directory had is gone, so nothing stops -- OpenSSL is not found.
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK
                      "-DOPENSSL_CRYPTO_LIBRARY=OPENSSL_CRYPTO_LIBRARY-NOTFOUND" "-DOPENSSL_ROOT_DIR=/nowhere"
                      NOT "is gone" HAS "OPENSSL=FALSE")
    # The searches take hints from pkg-config, whose answer FindPkgConfig keeps cached: a keg reachable through
    # pkg-config alone is found again only if the search asks it again (planted .pc files, PKG_CONFIG_LIBDIR naming
    # them alone) -- OpenSSL's and libngtcp2's.
    find_program(_pkg_config NAMES pkg-config pkgconf)
    if(_pkg_config)
        set(_name "pkg-config")
        set(_root "${WORK_DIR}/${_name}/root")
        escape("${_root}" _root_regex)
        function(plant_pc name keg libs) # <name>.pc naming <keg>, as the search sees it under the root
            file(WRITE "${_root}/pc/${name}.pc"
                 "prefix=${keg}\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\nName: ${name}\n"
                 "Description: planted\nVersion: 3.0.0\nLibs: -L\${libdir} ${libs}\nCflags: -I\${includedir}\n")
        endfunction()
        function(plant_kegs suffix) # an OpenSSL and a libngtcp2 that pkg-config alone names
            plant_keg("${_root}/kegs/ngtcp2-${suffix}" 3)
            plant_openssl("${_root}/kegs/openssl-${suffix}" 3 ${suffix})
            plant_pc(openssl /kegs/openssl-${suffix} "-lssl -lcrypto")
            plant_pc(libngtcp2 /kegs/ngtcp2-${suffix} "-lngtcp2")
            plant_pc(libngtcp2_crypto_ossl /kegs/ngtcp2-${suffix} "-lngtcp2_crypto_ossl")
        endfunction()
        plant_kegs(a)
        set(_env "PKG_CONFIG_LIBDIR=${_root}/pc" "PKG_CONFIG_PATH=" "PKG_CONFIG_SYSROOT_DIR=")
        configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DFIND_OPENSSL=ON" "-DUSE_PKG_CONFIG=ON"
                          "-DPKG_CONFIG_EXECUTABLE=${_pkg_config}"
                          HAS "OPENSSL=TRUE" "NGTCP2=TRUE"
                              "OPENSSL_SSL_LIBRARY=${_root_regex}/kegs/openssl-a/lib/libssl${_ext}"
                              "NGTCP2_CRYPTO_OSSL_LIBRARY=${_root_regex}/kegs/ngtcp2-a/lib/libngtcp2_crypto_ossl${_ext}")
        build_fixture(${_name} "nothing changed" 1 "")
        plant_kegs(b)
        file(REMOVE_RECURSE "${_root}/kegs/openssl-a" "${_root}/kegs/ngtcp2-a")
        entries(_at_b "${_root_regex}/kegs/ngtcp2-b")
        build_fixture(${_name} "upgrades pkg-config alone names, the old kegs removed" 2 "" "OPENSSL=TRUE" "NGTCP2=TRUE"
                      "OPENSSL_SSL_LIBRARY=${_root_regex}/kegs/openssl-b/lib/libssl${_ext}"
                      "OPENSSL_CRYPTO_LIBRARY=${_root_regex}/kegs/openssl-b/lib/libcrypto${_ext}" ${_at_b})
        build_fixture(${_name} "nothing changed since" 2 "")
        set(_env "")
    else()
        message(STATUS "quic-openssl-verdict: no pkg-config on this host: the pkg-config case is skipped")
    endif()
endif()
if(APPLE AND _generators)
    # The Cellar shape on macOS: the search finds the keg's own file, whose install name -- what a binary records and
    # dyld loads -- is the opt/ link. An upgrade that keeps the old keg moves only that link.
    list(GET _generators 0 _generator)
    use_generator("${_generator}")
    set(_name "install-name")
    set(_root "${WORK_DIR}/${_name}/root")
    set(_opt "${_root}/opt/homebrew/opt/libngtcp2")
    foreach(_keg 1 2 3)
        file(MAKE_DIRECTORY "${_root}/Cellar/libngtcp2/keg-${_keg}/lib")
        file(WRITE "${_root}/Cellar/libngtcp2/keg-${_keg}/lib/libngtcp2_crypto_ossl${_ext}.id"
             "${_opt}/lib/libngtcp2_crypto_ossl${_ext}\n")
    endforeach()
    plant_keg("${_root}/Cellar/libngtcp2/keg-1" 3)
    point("${_opt}" "${_root}/Cellar/libngtcp2/keg-1")
    plant("${_root}/lib/libssl" 3 3)
    plant("${_root}/lib/libcrypto" "" 3)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DCMAKE_PREFIX_PATH=/Cellar/libngtcp2/keg-1"
                      "-DOPENSSL_SSL_LIBRARY=${_root}/lib/libssl" "-DOPENSSL_CRYPTO_LIBRARY=${_root}/lib/libcrypto"
                      "-DOPENSSL_VERSION=3.5.9"
                      HAS "VERDICT=" "NGTCP2_CRYPTO_OSSL_LIBRARY=.*/Cellar/libngtcp2/keg-1/lib/.*")
    build_fixture(${_name} "nothing changed" 1 "")
    after_configure(${_name})
    plant_keg("${_root}/Cellar/libngtcp2/keg-2" 4)
    point("${_opt}" "${_root}/Cellar/libngtcp2/keg-2")
    build_fixture(${_name} "opt/ moved onto a new keg, the old one kept" 2 MAJORS)
    build_fixture(${_name} "nothing changed since" 2 MAJORS)
    plant_keg("${_root}/Cellar/libngtcp2/keg-3" 3)
    backdate("${_root}/Cellar/libngtcp2/keg-3/lib/libngtcp2_crypto_ossl${_ext}")
    point("${_opt}" "${_root}/Cellar/libngtcp2/keg-3")
    file(REMOVE_RECURSE "${_root}/Cellar/libngtcp2/keg-2")
    build_fixture(${_name} "opt/ moved onto an older keg, the one it named removed" 3 "")
    build_fixture(${_name} "nothing changed since" 3 "")
    # qb's own libssl in that shape, no libngtcp2: its check reads the install name the Cellar file records, and that
    # link moved onto a keg of another major re-runs the configure through the check's own inputs, which stops it.
    set(_name "install-name-openssl")
    set(_root "${WORK_DIR}/${_name}/root")
    set(_opt "${_root}/opt/openssl")
    foreach(_keg 1 2 3 4)
        file(MAKE_DIRECTORY "${_root}/Cellar/openssl/keg-${_keg}/lib")
        file(WRITE "${_root}/Cellar/openssl/keg-${_keg}/lib/libssl.dylib.id" "${_opt}/lib/libssl.dylib\n")
    endforeach()
    plant("${_root}/Cellar/openssl/keg-1/lib/libssl.dylib" 3 3)
    point("${_opt}" "${_root}/Cellar/openssl/keg-1")
    plant("${_root}/lib/libcrypto" "" 3)
    configure_fixture(${_name} "${WORK_DIR}/${_name}/build" OK "-DQB_WITH_QUIC=OFF"
                      "-DOPENSSL_SSL_LIBRARY=${_root}/Cellar/openssl/keg-1/lib/libssl.dylib"
                      "-DOPENSSL_CRYPTO_LIBRARY=${_root}/lib/libcrypto" "-DOPENSSL_VERSION=3.5.9"
                      "-DOPENSSL_INCLUDE_DIR=${_root}/include"
                      HAS "OPENSSL_COMPARED=TRUE" "OPENSSL_READ=3\\|3")
    build_fixture(${_name} "nothing changed" 1 "")
    after_configure(${_name})
    plant("${_root}/Cellar/openssl/keg-2/lib/libssl.dylib" 4 4)
    point("${_opt}" "${_root}/Cellar/openssl/keg-2")
    build_fixture(${_name} "opt/ moved onto a keg of OpenSSL 4, the old one kept" FAILS ""
                  OUT "OPENSSL_SSL_LIBRARY \\([^)]*/keg-1/lib/libssl\\.dylib\\) is a libssl of major 4")
    plant("${_root}/Cellar/openssl/keg-3/lib/libssl.dylib" 3 3)
    point("${_opt}" "${_root}/Cellar/openssl/keg-3")
    build_fixture(${_name} "opt/ moved onto a keg of OpenSSL 3" 2 "" "OPENSSL_READ=3\\|3")
    build_fixture(${_name} "nothing changed since" 2 "")
    # Onto a keg whose files keep older times, the one the link named removed: only the file read, gone, can tell.
    plant("${_root}/Cellar/openssl/keg-4/lib/libssl.dylib" 3 3)
    backdate("${_root}/Cellar/openssl/keg-4/lib/libssl.dylib")
    point("${_opt}" "${_root}/Cellar/openssl/keg-4")
    file(REMOVE_RECURSE "${_root}/Cellar/openssl/keg-3")
    build_fixture(${_name} "opt/ moved onto an older keg, the one it named removed" 3 "" "OPENSSL_READ=3\\|3")
    build_fixture(${_name} "nothing changed since" 3 "")
endif()
if(_failures)
    message(FATAL_ERROR "quic-openssl-verdict: ${_builds} builds, wrong ones:${_failures}")
endif()
string(REPLACE ";" ", " _generators_shown "${_generators}")
if(NOT _generators)
    set(_generators_shown "none: nothing is read on this host")
endif()
message(STATUS "quic-openssl-verdict: ${_cases} configures and ${_builds} builds right (rebuilt with: "
               "${_generators_shown})")
