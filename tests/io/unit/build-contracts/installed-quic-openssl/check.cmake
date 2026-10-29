# qb-io-test-unit-installed-quic-openssl (Huly QB-1183). An installed qb built with QUIC re-runs find_dependency(Ngtcp2)
# at its consumer, and libngtcp2 may have moved onto another OpenSSL major since qb was installed: the generated
# qbConfig.cmake must then warn, naming both majors -- QUIC is compiled into qb, the consumer cannot turn it off. This
# generates qbConfig.cmake from cmake/qbConfig.cmake.in as an install does (configure_package_config_file, with the
# values of a qb built with SSL and QUIC against OpenSSL 3.5.9), then has a consumer find it, per case, beside the
# FindNgtcp2.cmake an install ships -- its functions, and readings planted where a real one reads a library -- with
# stand-in Threads, OpenSSL and nlohmann_json modules. The consumer's OpenSSL is the one compared: one major there and in
# libngtcp2 is silent even when qb was built against another (the OpenSSL-major check above it warns about that), and a
# consumer's own FindNgtcp2 that won the search reads nothing, so says nothing. Its libssl and libcrypto are read as
# qb's configure reads its own -- off Windows, planted libraries read through a stand-in tool: what they read wins over
# the headers' version, and a libcrypto of another major than its libssl is two majors; so is a libngtcp2 whose
# libcrypto is not its libssl's major.
#
#   cmake -DQB_CMAKE_MODULE_DIR=<qb>/cmake -DWORK_DIR=<scratch directory> -P check.cmake

cmake_minimum_required(VERSION 3.24)

if(NOT QB_CMAKE_MODULE_DIR OR NOT EXISTS "${QB_CMAKE_MODULE_DIR}/qbConfig.cmake.in" OR NOT WORK_DIR)
    message(FATAL_ERROR "installed-quic-openssl: QB_CMAKE_MODULE_DIR (qb/cmake) and WORK_DIR are required")
endif()

if(DEFINED CASE_SSL) # one consumer's find_package(qb), run by the loop below in a process of its own
    set(CMAKE_MODULE_PATH "${WORK_DIR}/consumer-modules")
    if(OWN_FINDNGTCP2)
        list(APPEND CMAKE_MODULE_PATH "${WORK_DIR}/consumer-own")
    endif()
    if(CASE_TOOL AND APPLE)
        set(NGTCP2_OTOOL_EXECUTABLE "${CASE_TOOL}")
    elseif(CASE_TOOL)
        set(CMAKE_READELF "${CASE_TOOL}")
    endif()
    find_package(qb CONFIG PATHS "${WORK_DIR}/prefix/lib/cmake/qb" NO_DEFAULT_PATH)
    return()
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
include(CMakePackageConfigHelpers)
set(QB_FRAMEWORK_VERSION 0.0.0)
set(QB_FRAMEWORK_VERSION_MAJOR 0)
set(QB_FRAMEWORK_VERSION_MINOR 0)
set(QB_FRAMEWORK_VERSION_PATCH 0)
set(QB_HAS_SSL TRUE)
set(QB_HAS_QUIC TRUE)
set(QB_HAS_COMPRESSION FALSE)
set(QB_HAS_ZSTD FALSE)
set(QB_HAS_BROTLI FALSE)
set(QB_HAS_ARGON2 FALSE)
set(OPENSSL_VERSION 3.5.9)
set(QB_ABI_UNORDERED_MAP planted)
configure_package_config_file("${QB_CMAKE_MODULE_DIR}/qbConfig.cmake.in" "${WORK_DIR}/prefix/lib/cmake/qb/qbConfig.cmake"
                              INSTALL_DESTINATION lib/cmake/qb INSTALL_PREFIX "${WORK_DIR}/prefix")
file(READ "${WORK_DIR}/prefix/lib/cmake/qb/qbConfig.cmake" _generated)
if(NOT _generated MATCHES "find_dependency\\(Ngtcp2\\)")
    message(FATAL_ERROR "installed-quic-openssl: the generated qbConfig.cmake has no QUIC block to test")
endif()

# The FindNgtcp2.cmake an install puts beside qbConfig.cmake: here its functions, and the readings planted for the case
# (a real one reads them from libngtcp2_crypto_ossl, which no lane is guaranteed to have).
file(WRITE "${WORK_DIR}/prefix/lib/cmake/qb/FindNgtcp2.cmake"
     "set(NGTCP2_FIND_FUNCTIONS_ONLY ON)\ninclude(\"${QB_CMAKE_MODULE_DIR}/FindNgtcp2.cmake\")\n"
     "unset(NGTCP2_FIND_FUNCTIONS_ONLY)\nset(Ngtcp2_FOUND TRUE)\nset(NGTCP2_CRYPTO_OSSL_LIBRARY /planted/libngtcp2_crypto_ossl)\n"
     "set(Ngtcp2_OPENSSL_MAJOR \"\${CASE_NGTCP2}\")\nset(Ngtcp2_OPENSSL_LIBRARY \"\")\n"
     "set(Ngtcp2_OPENSSL_CRYPTO_MAJOR \"\${CASE_NGTCP2_CRYPTO}\")\nset(Ngtcp2_OPENSSL_CRYPTO_LIBRARY \"\")\n")
# A consumer's own FindNgtcp2, first on its module path: it finds libngtcp2 and reads nothing.
file(WRITE "${WORK_DIR}/consumer-own/FindNgtcp2.cmake" "set(Ngtcp2_FOUND TRUE)\n")
# The consumer's OpenSSL: CASE_SSL as its headers' version, and the libraries CASE_SSL_LIB / CASE_CRYPTO_LIB -- by
# default none a reader could open.
file(WRITE "${WORK_DIR}/consumer-modules/FindOpenSSL.cmake"
     "set(OpenSSL_FOUND TRUE)\nset(OPENSSL_FOUND TRUE)\nset(OPENSSL_VERSION \"\${CASE_SSL}\")\n"
     "set(OPENSSL_SSL_LIBRARY \"\${CASE_SSL_LIB}\")\nset(OPENSSL_CRYPTO_LIBRARY \"\${CASE_CRYPTO_LIB}\")\n")
# Off Windows, a stand-in for the platform's tool that prints a planted library's content as its listing.
set(_posix FALSE)
if(NOT WIN32 AND EXISTS /bin/sh)
    set(_posix TRUE)
    file(WRITE "${WORK_DIR}/tool" "#!/bin/sh\necho \"$2:\"\n[ \"$1\" = -D ] && exit 0\ncat \"$2\"\n")
    file(CHMOD "${WORK_DIR}/tool" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif()
function(plant file name major) # a library whose listing names lib<name> of OpenSSL <major>, in the host's format
    if(APPLE)
        file(WRITE "${file}" "\t/opt/o${major}/lib/lib${name}.${major}.dylib (compatibility version ${major}.0.0)\n")
    else()
        file(WRITE "${file}" " 0x000000000000000e (SONAME)             Library soname: [lib${name}.so.${major}]\n")
    endif()
endfunction()
file(WRITE "${WORK_DIR}/consumer-modules/FindThreads.cmake" "set(Threads_FOUND TRUE)\n")
file(WRITE "${WORK_DIR}/consumer-modules/Findnlohmann_json.cmake"
     "set(nlohmann_json_FOUND TRUE)\nset(nlohmann_json_VERSION 3.11.3)\n")

set(_cases 0)
set(_failures "")
# consume(<label> <consumer's headers> <libngtcp2's libssl major, "" read nothing> <own FindNgtcp2> <warning wanted>
#         [NGTCP2_CRYPTO <major>] [SSL_LIB <file>] [CRYPTO_LIB <file>] [MUST <regex>...])
function(consume label ssl ngtcp2 own want)
    cmake_parse_arguments(_c "" "NGTCP2_CRYPTO;SSL_LIB;CRYPTO_LIB" "MUST" ${ARGN})
    if(NOT DEFINED _c_NGTCP2_CRYPTO)
        set(_c_NGTCP2_CRYPTO "${ngtcp2}")
    endif()
    foreach(_lib SSL_LIB CRYPTO_LIB)
        if(NOT _c_${_lib})
            set(_c_${_lib} "/planted/${_lib}")
        endif()
    endforeach()
    set(_tool "")
    if(_posix)
        set(_tool "${WORK_DIR}/tool")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DQB_CMAKE_MODULE_DIR=${QB_CMAKE_MODULE_DIR}" "-DWORK_DIR=${WORK_DIR}"
                            "-DCASE_SSL=${ssl}" "-DCASE_NGTCP2=${ngtcp2}" "-DCASE_NGTCP2_CRYPTO=${_c_NGTCP2_CRYPTO}"
                            "-DCASE_SSL_LIB=${_c_SSL_LIB}" "-DCASE_CRYPTO_LIB=${_c_CRYPTO_LIB}" "-DCASE_TOOL=${_tool}"
                            "-DOWN_FINDNGTCP2=${own}" -P "${CMAKE_CURRENT_LIST_FILE}"
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    string(REGEX REPLACE "[ \t\r]*\n[ \t]*" " " _flat "${_out}")
    math(EXPR _n "${_cases} + 1")
    set(_cases ${_n} PARENT_SCOPE)
    set(_warned FALSE)
    if("${_flat}" MATCHES "two OpenSSL majors in one process crash at the first QUIC handshake")
        set(_warned TRUE)
    endif()
    set(_wrong "")
    if(NOT _rc EQUAL 0)
        set(_wrong "the consumer's configure failed (${_rc})")
    elseif(want AND NOT _warned)
        set(_wrong "no warning")
    elseif(NOT want AND _warned)
        set(_wrong "a warning")
    elseif(want AND NOT "${_flat}" MATCHES "built against OpenSSL 3\\.5\\.9")
        set(_wrong "a warning that does not name the OpenSSL qb was built against")
    endif()
    if(want AND NOT _wrong)
        foreach(_must IN LISTS _c_MUST)
            if(NOT "${_flat}" MATCHES "${_must}")
                set(_wrong "a warning without '${_must}'")
            endif()
        endforeach()
    endif()
    if(_wrong)
        set(_failures "${_failures}\n  ${label}: ${_wrong}\n    output: ${_flat}" PARENT_SCOPE)
    endif()
endfunction()
consume("libngtcp2 moved onto OpenSSL 4 after the install" 3 4 FALSE TRUE
        MUST "links OpenSSL 4, and this project links the libssl of OpenSSL 3 ")
consume("libngtcp2 and the consumer on the OpenSSL qb was built against" 3 3 FALSE FALSE)
consume("the consumer on libngtcp2's OpenSSL 4, qb built against 3" 4 4 FALSE FALSE)
consume("the consumer on OpenSSL 4, libngtcp2 and qb on 3" 4 3 FALSE TRUE
        MUST "links OpenSSL 3, and this project links the libssl of OpenSSL 4 ")
consume("a libngtcp2 nothing could read" 3 "" FALSE FALSE NGTCP2_CRYPTO "")
consume("the consumer's own FindNgtcp2" 3 4 TRUE FALSE)
consume("libngtcp2's libcrypto on another major than its libssl" 3 3 FALSE TRUE NGTCP2_CRYPTO 4
        MUST "links the libssl of OpenSSL 3 and the libcrypto of OpenSSL 4, and this project links the libssl of OpenSSL 3 ")
consume("libngtcp2's libssl unread, its libcrypto on another major" 3 "" FALSE TRUE NGTCP2_CRYPTO 4
        MUST "links OpenSSL 4, and this project")
if(_posix)
    plant("${WORK_DIR}/libs/libssl.3" ssl 3)
    plant("${WORK_DIR}/libs/libcrypto.3" crypto 3)
    plant("${WORK_DIR}/libs/libssl.4" ssl 4)
    plant("${WORK_DIR}/libs/libcrypto.4" crypto 4)
    string(REGEX REPLACE "([][+.*()^$?|\\\\{}])" "\\\\\\1" _libs "${WORK_DIR}/libs") # as a regex
    consume("the libraries read, not the headers' version" 4 3 FALSE FALSE
            SSL_LIB "${WORK_DIR}/libs/libssl.3" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.3")
    consume("the libraries read, against libngtcp2's" 3 4 FALSE TRUE
            SSL_LIB "${WORK_DIR}/libs/libssl.3" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.3"
            MUST "the libssl of OpenSSL 3 \\(${_libs}/libssl\\.3\\)")
    consume("the consumer's libcrypto on another major than its libssl" 4 4 FALSE TRUE
            SSL_LIB "${WORK_DIR}/libs/libssl.4" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.3"
            MUST "the libssl of OpenSSL 4 .* and the libcrypto of OpenSSL 3 ")
endif()
if(_failures)
    message(FATAL_ERROR "installed-quic-openssl: ${_cases} consumers, wrong ones:${_failures}")
endif()
message(STATUS "installed-quic-openssl: ${_cases} consumers right")
