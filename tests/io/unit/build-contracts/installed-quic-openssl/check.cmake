# qb-io-test-unit-installed-quic-openssl (Huly QB-1183, QB-1256). An installed qb built with QUIC re-runs
# find_dependency(Ngtcp2) at its consumer, and libngtcp2 may have moved onto another OpenSSL major since qb was
# installed: the generated qbConfig.cmake must then report qb not found, naming both majors and the remedy -- QUIC is
# compiled into qb, the consumer cannot turn it off -- as it must for an OpenSSL of another major than qb's. This
# generates qbConfig.cmake from cmake/qbConfig.cmake.in as an install does (configure_package_config_file, with the
# values of a qb built with SSL and QUIC against OpenSSL 3.5.9), then has a consumer find it, per case, beside the
# FindNgtcp2.cmake an install ships -- its functions, and readings planted where a real one reads a library -- with
# stand-in Threads, OpenSSL and nlohmann_json modules. The consumer's OpenSSL is the one compared: one major there and in
# libngtcp2 passes the QUIC verdict even when qb was built against another (the OpenSSL-major check before it refuses
# that, on the version FindOpenSSL reports: a newer minor of qb's major passes), and a consumer's own FindNgtcp2 that
# won the search reads nothing, so says nothing. The remedy is the side's that is not on qb's major: this project's
# OpenSSL entries, or libngtcp2's. Its libssl and libcrypto are read as
# qb's configure reads its own -- off Windows, planted libraries read through a stand-in tool: what they read wins over
# the headers' version, and a libcrypto of another major than its libssl is two majors; so is a libngtcp2 whose
# libcrypto is not its libssl's major. Libraries read off qb's major, where the headers are on it, are refused too.
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
    if(CASE_WIN32) # the consumer's platform as qbConfig.cmake reads it: FindOpenSSL caches other names there
        set(WIN32 TRUE)
        set(CMAKE_SYSTEM_NAME Windows)
    elseif(CASE_SYSTEM) # a system whose libraries' file names need not carry their release's major
        set(CMAKE_SYSTEM_NAME "${CASE_SYSTEM}")
    else() # as a project has it
        set(CMAKE_SYSTEM_NAME "${CMAKE_HOST_SYSTEM_NAME}")
    endif()
    if(NOT CASE_NAME)
        set(CASE_NAME qb)
    endif()
    # A consumer whose policies predate CMP0124 (3.21), whose scope keeps a foreach's variable: the strictest to leave
    # clean.
    cmake_policy(VERSION 3.20)
    set(_components "")
    if(CASE_COMPONENTS)
        set(_components OPTIONAL_COMPONENTS ${CASE_COMPONENTS})
    endif()
    find_package(${CASE_NAME} CONFIG ${_components} PATHS "${WORK_DIR}/${CASE_PREFIX}/lib/cmake/qb" NO_DEFAULT_PATH)
    message(STATUS "qb found: [${${CASE_NAME}_FOUND}]")
    # What a report leaves in the consumer's scope: its module path as it was, and none of qbConfig.cmake's variables.
    get_cmake_property(_left VARIABLES)
    list(FILTER _left INCLUDE REGEX "^(_qb_|_QB_|_IMPORT_PREFIX$|_comp$|_comp_deps$)")
    list(FILTER _left EXCLUDE REGEX "^_QB_(HAD|FOUND)_") # the records of its searches, cache entries in a project
    message(STATUS "left: [${_left}] module path: [${CMAKE_MODULE_PATH}]")
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

# A second prefix, of a qb whose OpenSSL version was not recorded: measured against this project's headers' major.
set(OPENSSL_VERSION "")
configure_package_config_file("${QB_CMAKE_MODULE_DIR}/qbConfig.cmake.in"
                              "${WORK_DIR}/prefix-nover/lib/cmake/qb/qbConfig.cmake"
                              INSTALL_DESTINATION lib/cmake/qb INSTALL_PREFIX "${WORK_DIR}/prefix-nover")
set(OPENSSL_VERSION 3.5.9)
# qbFoundLibraries.cmake, which an install puts beside qbConfig.cmake for its searches (Huly QB-1201), and the
# FindNgtcp2.cmake it puts there: here its functions, and the readings planted for the case (a real one reads them from
# libngtcp2_crypto_ossl, which no lane is guaranteed to have).
foreach(_prefix prefix prefix-nover)
    file(COPY "${QB_CMAKE_MODULE_DIR}/qbFoundLibraries.cmake" DESTINATION "${WORK_DIR}/${_prefix}/lib/cmake/qb")
    file(WRITE "${WORK_DIR}/${_prefix}/lib/cmake/qb/FindNgtcp2.cmake"
         "set(NGTCP2_FIND_FUNCTIONS_ONLY ON)\ninclude(\"${QB_CMAKE_MODULE_DIR}/FindNgtcp2.cmake\")\n"
         "unset(NGTCP2_FIND_FUNCTIONS_ONLY)\nset(Ngtcp2_FOUND TRUE)\nset(NGTCP2_CRYPTO_OSSL_LIBRARY /planted/libngtcp2_crypto_ossl)\n"
         "set(Ngtcp2_OPENSSL_MAJOR \"\${CASE_NGTCP2}\")\nset(Ngtcp2_OPENSSL_LIBRARY \"\")\n"
         "set(Ngtcp2_OPENSSL_CRYPTO_MAJOR \"\${CASE_NGTCP2_CRYPTO}\")\nset(Ngtcp2_OPENSSL_CRYPTO_LIBRARY \"\")\n")
endforeach()
# A consumer's own FindNgtcp2, first on its module path: it finds libngtcp2 and reads nothing.
file(WRITE "${WORK_DIR}/consumer-own/FindNgtcp2.cmake" "set(Ngtcp2_FOUND TRUE)\n")
# The consumer's OpenSSL: CASE_SSL as its headers' version, and the libraries CASE_SSL_LIB / CASE_CRYPTO_LIB -- by
# default none a reader could open.
file(WRITE "${WORK_DIR}/consumer-modules/FindOpenSSL.cmake"
     "set(OpenSSL_FOUND TRUE)\nset(OPENSSL_FOUND TRUE)\nset(OPENSSL_VERSION \"\${CASE_SSL}\")\n"
     "set(OPENSSL_INCLUDE_DIR /planted/include)\n"
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
# The OpenSSL remedy as the host prints it: a consumer on Windows (WIN32 in a script too) caches LIB_EAY and SSL_EAY.
set(_ssl_remedy " +cmake -U OPENSSL_INCLUDE_DIR -U OPENSSL_SSL_LIBRARY -U OPENSSL_CRYPTO_LIBRARY -DOPENSSL_ROOT_DIR=<it> <build directory>")
set(_ssl_remedy_windows " +cmake -U OPENSSL_INCLUDE_DIR -U \"LIB_EAY\\*\" -U \"SSL_EAY\\*\" -DOPENSSL_ROOT_DIR=<it> <build directory>")
if(WIN32)
    set(_ssl_remedy "${_ssl_remedy_windows}")
endif()
set(_ngtcp2_remedy " +cmake -U NGTCP2_INCLUDE_DIR -U NGTCP2_LIBRARY -U NGTCP2_CRYPTO_OSSL_LIBRARY -U NGTCP2_CRYPTO_INCLUDE_DIR -U NGTCP2_CRYPTO_OSSL_INCLUDE_DIR -DNgtcp2_ROOT=<it> <build directory>")
# consume(<label> <consumer's headers> <libngtcp2's libssl major, "" read nothing> <own FindNgtcp2> FOUND|SSL|QUIC|LIBS
#         [NGTCP2_CRYPTO <major>] [SSL_LIB <file>] [CRYPTO_LIB <file>] [WIN32 | SYSTEM <CMAKE_SYSTEM_NAME>]
#         [NAME <package name>] [PREFIX prefix-nover] [COMPONENTS <optional component>...] [MUST <regex>...]
#         [LINE <regex>...] [NOTLINE <regex>...] [NOT <regex>...]): qb found, or not found for OpenSSL's major (SSL) or
# for QUIC's two majors (QUIC) or for this project's libraries read off qb's major (LIBS), under the name asked for; MUST and NOT read the joined output, LINE and NOTLINE a whole
# line of it as printed (the remedy, which CMake prints unwrapped, and the prose around it, which it wraps). Either way
# the consumer's module path is as it was, and none of the variables qbConfig.cmake sets is left behind.
function(consume label ssl ngtcp2 own want)
    cmake_parse_arguments(_c "WIN32" "NGTCP2_CRYPTO;SSL_LIB;CRYPTO_LIB;NAME;SYSTEM;PREFIX" "COMPONENTS;MUST;LINE;NOTLINE;NOT"
                          ${ARGN})
    if(NOT _c_PREFIX)
        set(_c_PREFIX prefix)
    endif()
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
                            "-DOWN_FINDNGTCP2=${own}" "-DCASE_WIN32=${_c_WIN32}" "-DCASE_NAME=${_c_NAME}"
                            "-DCASE_SYSTEM=${_c_SYSTEM}" "-DCASE_PREFIX=${_c_PREFIX}" "-DCASE_COMPONENTS=${_c_COMPONENTS}"
                            -P "${CMAKE_CURRENT_LIST_FILE}"
                    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    string(REGEX REPLACE "[ \t\r]*\n[ \t]*" " " _flat "${_out}")
    math(EXPR _n "${_cases} + 1")
    set(_cases ${_n} PARENT_SCOPE)
    set(_got FOUND)
    if("${_flat}" MATCHES "qb found: \\[(0|FALSE)\\]")
        set(_got "not found")
        if("${_flat}" MATCHES "this qb was built against OpenSSL 3\\.5\\.9, and this project found OpenSSL ")
            set(_got SSL)
        elseif("${_flat}" MATCHES "two OpenSSL majors in one process crash at the first QUIC handshake")
            set(_got QUIC)
        elseif("${_flat}" MATCHES "this qb was built against OpenSSL 3\\.5\\.9, and this project's OpenSSL libraries are another major")
            set(_got LIBS)
        endif()
    elseif(NOT "${_flat}" MATCHES "qb found: \\[(1|TRUE)\\]")
        set(_got "nothing said")
    endif()
    set(_wrong "")
    if(NOT _rc EQUAL 0)
        set(_wrong "the consumer's configure failed (${_rc})")
    elseif(NOT _got STREQUAL want)
        set(_wrong "${_got}, not ${want}")
    elseif(want STREQUAL "QUIC" AND NOT "${_flat}" MATCHES "built against OpenSSL 3\\.5\\.9\\) \\(Huly QB-1183, QB-1256\\)")
        set(_wrong "a QUIC verdict that does not name the OpenSSL qb was built against")
    elseif(want STREQUAL "FOUND" AND "${_flat}" MATCHES "CMake Warning")
        set(_wrong "found, with a warning")
    elseif(NOT "${_flat}" MATCHES "left: \\[\\] module path: \\[[^]]*consumer-modules(;[^];]*consumer-own)?\\]")
        set(_wrong "variables left, or a module path not restored")
    endif()
    if(NOT _wrong)
        foreach(_must IN LISTS _c_MUST)
            if(NOT "${_flat}" MATCHES "${_must}")
                set(_wrong "no '${_must}'")
            endif()
        endforeach()
        foreach(_line IN LISTS _c_LINE)
            if(NOT "\n${_out}\n" MATCHES "\n${_line}\n")
                set(_wrong "no line '${_line}'")
            endif()
        endforeach()
        foreach(_line IN LISTS _c_NOTLINE)
            if("\n${_out}\n" MATCHES "\n${_line}\n")
                set(_wrong "a line '${_line}'")
            endif()
        endforeach()
        foreach(_not IN LISTS _c_NOT)
            if("${_flat}" MATCHES "${_not}")
                set(_wrong "'${_not}'")
            endif()
        endforeach()
    endif()
    if(_wrong)
        set(_failures "${_failures}\n  ${label}: ${_wrong}\n    output: ${_flat}" PARENT_SCOPE)
    endif()
endfunction()
consume("libngtcp2 moved onto OpenSSL 4 after the install" 3 4 FALSE QUIC
        MUST "links OpenSSL 4, and this project links the libssl of OpenSSL 3 " "Point Ngtcp2_ROOT at a libngtcp2 built against OpenSSL 3 "
        LINE "${_ngtcp2_remedy}" NOT "OPENSSL_ROOT_DIR=" "Ngtcp2_ROOT=<it> <build directory> .*Ngtcp2_ROOT=<it>")
consume("libngtcp2 and the consumer on the OpenSSL qb was built against" 3 3 FALSE FOUND)
consume("a newer minor of the OpenSSL qb was built against" 3.6.1 3 FALSE FOUND)
consume("components asked for, a dependency last" 3 3 FALSE FOUND COMPONENTS io core)
consume("the consumer on OpenSSL 4, libngtcp2 too, qb built against 3" 4 4 FALSE SSL
        MUST "this qb was built against OpenSSL 3\\.5\\.9, and this project found OpenSSL 4 " "Point OPENSSL_ROOT_DIR at an OpenSSL 3 "
             "or rebuild qb against OpenSSL 4\\. +This qb has QUIC: the libngtcp2 found must link that OpenSSL's major too\\."
        LINE "${_ssl_remedy}" NOT "two OpenSSL majors")
consume("the consumer on OpenSSL 1.1, older than qb's" 1.1.1 3 FALSE SSL
        MUST "this qb was built against OpenSSL 3\\.5\\.9, and this project found OpenSSL 1\\.1\\.1 " "Point OPENSSL_ROOT_DIR at an OpenSSL 3 "
        LINE "${_ssl_remedy}")
consume("the consumer on OpenSSL 4, libngtcp2 and qb on 3" 4 3 FALSE SSL LINE "${_ssl_remedy}" NOT "two OpenSSL majors")
consume("the consumer on OpenSSL 4 on Windows" 4 "" FALSE SSL WIN32 LINE "${_ssl_remedy_windows}")
# QB, on a file system that ignores case: reported under the name asked for.
if(EXISTS "${WORK_DIR}/prefix/lib/cmake/qb/QBCONFIG.CMAKE")
    consume("asked for as QB" 4 4 FALSE SSL NAME QB MUST "package \"QB\" is considered to be NOT FOUND")
    consume("asked for as QB, libngtcp2 on OpenSSL 4" 3 4 FALSE QUIC NAME QB MUST "package \"QB\" is considered to be NOT FOUND")
endif()
consume("a libngtcp2 nothing could read" 3 "" FALSE FOUND NGTCP2_CRYPTO "")
consume("the consumer's own FindNgtcp2" 3 4 TRUE FOUND)
consume("libngtcp2's libcrypto on another major than its libssl" 3 3 FALSE QUIC NGTCP2_CRYPTO 4
        MUST "links the libssl of OpenSSL 3 and the libcrypto of OpenSSL 4, and this project links the libssl of OpenSSL 3 "
        LINE "${_ngtcp2_remedy}")
consume("libngtcp2's libssl unread, its libcrypto on another major" 3 "" FALSE QUIC NGTCP2_CRYPTO 4
        MUST "links OpenSSL 4, and this project" LINE "${_ngtcp2_remedy}")
if(_posix)
    plant("${WORK_DIR}/libs/libssl.3" ssl 3)
    plant("${WORK_DIR}/libs/libcrypto.3" crypto 3)
    plant("${WORK_DIR}/libs/libssl.4" ssl 4)
    plant("${WORK_DIR}/libs/libcrypto.4" crypto 4)
    string(REGEX REPLACE "([][+.*()^$?|\\\\{}])" "\\\\\\1" _libs "${WORK_DIR}/libs") # as a regex
    # The QUIC verdict reads the libraries, not the headers' version -- which the OpenSSL-major check reads, as
    # FindOpenSSL reports it: headers of qb's major pass that check, and libraries read off qb's major are refused here,
    # libngtcp2 on their major (both remedies) or on qb's (the OpenSSL one; two majors in one process besides).
    consume("the libraries read off qb's major, libngtcp2 on theirs" 3 4 FALSE LIBS
            SSL_LIB "${WORK_DIR}/libs/libssl.4" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.4"
            MUST "the libssl of OpenSSL 4 \\(${_libs}/libssl\\.4\\) and the libcrypto of OpenSSL 4 \\(${_libs}/libcrypto\\.4\\), read from the files, where its headers \\(OPENSSL_INCLUDE_DIR /planted/include\\) say 3 "
                 "Point OPENSSL_ROOT_DIR at an OpenSSL 3 with" "Or rebuild qb against the OpenSSL those libraries are\\."
            LINE "${_ssl_remedy}" "${_ngtcp2_remedy}" NOT "two OpenSSL majors")
    consume("the libraries read off qb's major, libngtcp2 on qb's" 3 3 FALSE QUIC
            SSL_LIB "${WORK_DIR}/libs/libssl.4" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.4"
            MUST "this project links the libssl of OpenSSL 4 .* and the libcrypto of OpenSSL 4 "
            LINE "${_ssl_remedy}" NOT "Ngtcp2_ROOT")
    consume("the libraries read, against libngtcp2's" 3 4 FALSE QUIC
            SSL_LIB "${WORK_DIR}/libs/libssl.3" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.3"
            MUST "the libssl of OpenSSL 3 \\(${_libs}/libssl\\.3\\)" LINE "${_ngtcp2_remedy}")
    # This project's libcrypto on another major than qb's: the remedy is its OpenSSL entries, not libngtcp2's.
    consume("the consumer's libcrypto on another major than its libssl" 3 3 FALSE QUIC
            SSL_LIB "${WORK_DIR}/libs/libssl.3" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.4"
            MUST "the libssl of OpenSSL 3 .* and the libcrypto of OpenSSL 4 " "Point OPENSSL_ROOT_DIR at an OpenSSL 3 with"
            LINE "${_ssl_remedy}" NOT "Ngtcp2_ROOT")
    # This project's libssl on another major than qb's, its libcrypto on qb's: its OpenSSL entries again.
    consume("the consumer's libssl on another major than its libcrypto" 3 3 FALSE QUIC
            SSL_LIB "${WORK_DIR}/libs/libssl.4" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.3"
            MUST "the libssl of OpenSSL 4 .* and the libcrypto of OpenSSL 3 " LINE "${_ssl_remedy}" NOT "Ngtcp2_ROOT")
    # This project's headers unread (no version reported): the side off qb's major is still measured against qb's.
    consume("this project's headers unread" "" 4 FALSE QUIC
            SSL_LIB "${WORK_DIR}/libs/libssl.3" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.3"
            LINE "${_ngtcp2_remedy}" NOT "OPENSSL_ROOT_DIR=")
    # This project's headers unread, its libraries off qb's major: measured against qb's major, not the headers'.
    consume("this project's headers unread, its libraries off qb's major" "" 4 FALSE LIBS
            SSL_LIB "${WORK_DIR}/libs/libssl.4" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.4"
            MUST "\\(OPENSSL_INCLUDE_DIR /planted/include\\) give no version -- " LINE "${_ssl_remedy}" "${_ngtcp2_remedy}")
    # A qb whose OpenSSL version was not recorded: measured against this project's headers', none read here.
    consume("a qb whose OpenSSL was not recorded, this project's headers unread" "" 4 FALSE FOUND PREFIX prefix-nover
            SSL_LIB "${WORK_DIR}/libs/libssl.4" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.4")
    # A base system that numbers its libraries apart from the release (FreeBSD's libssl.so.30 is OpenSSL 3): the
    # readings are compared with each other alone -- alike, nothing said; apart, both sides' remedies.
    plant("${WORK_DIR}/libs/libssl.30" ssl 30)
    plant("${WORK_DIR}/libs/libcrypto.30" crypto 30)
    consume("a base system's numbering, both sides alike" 3.0.15 30 FALSE FOUND SYSTEM FreeBSD
            SSL_LIB "${WORK_DIR}/libs/libssl.30" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.30")
    consume("a base system's numbering, libngtcp2 apart" 3.0.15 35 FALSE QUIC SYSTEM FreeBSD
            SSL_LIB "${WORK_DIR}/libs/libssl.30" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.30"
            LINE "${_ssl_remedy}" "${_ngtcp2_remedy}")
    consume("a base system's numbering, libngtcp2's file numbered as qb's release" 3.0.15 3 FALSE QUIC SYSTEM FreeBSD
            SSL_LIB "${WORK_DIR}/libs/libssl.30" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.30"
            LINE "${_ssl_remedy}" "${_ngtcp2_remedy}")
    # Both sides off qb's major: both remedies, each command on a line of its own, the prose between them wrapped.
    consume("the consumer's libcrypto and libngtcp2 on another major than qb's" 3 4 FALSE QUIC
            SSL_LIB "${WORK_DIR}/libs/libssl.3" CRYPTO_LIB "${WORK_DIR}/libs/libcrypto.4"
            LINE "${_ssl_remedy}" "${_ngtcp2_remedy}"
            NOTLINE " *Point Ngtcp2_ROOT at a libngtcp2 built against OpenSSL 3 with this one's five entries removed:")
endif()

# The remedies, run as printed (QB-1256): the consumer project beside this file, configured against an installed prefix
# whose FindNgtcp2.cmake is qb's own, finds a planted OpenSSL and libngtcp2 by the real searches; the command the reason
# prints on a line of its own is taken from the output, its <it> and <build directory> filled in, and run with nothing
# else -- it finds what it points at, and qb. A newer minor of qb's OpenSSL major is found as before. POSIX, where the
# stand-in tool reads the planted libraries; on Windows, which reads none, the OpenSSL remedy only, emulated
# (CMAKE_SYSTEM_NAME): FindOpenSSL caches the libraries there as LIB_EAY and SSL_EAY.
set(_projects 0)
if(_posix)
    set(_real "${WORK_DIR}/real")
    configure_package_config_file("${QB_CMAKE_MODULE_DIR}/qbConfig.cmake.in" "${_real}/prefix/lib/cmake/qb/qbConfig.cmake"
                                  INSTALL_DESTINATION lib/cmake/qb INSTALL_PREFIX "${_real}/prefix")
    file(COPY "${QB_CMAKE_MODULE_DIR}/FindNgtcp2.cmake" "${QB_CMAKE_MODULE_DIR}/qbFoundLibraries.cmake"
         DESTINATION "${_real}/prefix/lib/cmake/qb")
    set(QB_HAS_QUIC FALSE) # a qb without QUIC, for the Windows remedy: nothing to read there
    configure_package_config_file("${QB_CMAKE_MODULE_DIR}/qbConfig.cmake.in"
                                  "${_real}/prefix-ssl/lib/cmake/qb/qbConfig.cmake"
                                  INSTALL_DESTINATION lib/cmake/qb INSTALL_PREFIX "${_real}/prefix-ssl")
    file(COPY "${QB_CMAKE_MODULE_DIR}/qbFoundLibraries.cmake" DESTINATION "${_real}/prefix-ssl/lib/cmake/qb")
    set(QB_HAS_QUIC TRUE)
    file(WRITE "${_real}/standins/FindThreads.cmake" "set(Threads_FOUND TRUE)\n")
    file(WRITE "${_real}/standins/Findnlohmann_json.cmake"
         "set(nlohmann_json_FOUND TRUE)\nset(nlohmann_json_VERSION 3.11.3)\n")
    set(_ext .so)
    if(APPLE)
        set(_ext .dylib)
    endif()
    function(plant_listing file ssl crypto) # a library whose listing names libssl of <ssl> and libcrypto of <crypto>
        set(_listing "")
        foreach(_name IN ITEMS ssl crypto)
            if("${${_name}}" STREQUAL "")
                continue()
            endif()
            if(APPLE)
                string(APPEND _listing "\t/opt/o${${_name}}/lib/lib${_name}.${${_name}}.dylib (compatibility version 1.0.0)\n")
            else()
                string(APPEND _listing " 0x0000000000000001 (NEEDED)             Shared library: [lib${_name}.so.${${_name}}]\n")
            endif()
        endforeach()
        get_filename_component(_dir "${file}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dir}")
        file(WRITE "${file}" "${_listing}")
    endfunction()
    # plant_openssl(<prefix> <version> <libssl's major> <libcrypto's major>): OpenSSL <version>'s headers, its libssl and
    # libcrypto read as those majors
    function(plant_openssl prefix version ssl crypto)
        file(WRITE "${prefix}/include/openssl/ssl.h" "")
        file(WRITE "${prefix}/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"${version}\"\n")
        plant_listing("${prefix}/lib/libssl${_ext}" "${ssl}" "${crypto}")
        plant_listing("${prefix}/lib/libcrypto${_ext}" "" "${crypto}")
    endfunction()
    function(plant_ngtcp2 prefix major) # a libngtcp2 whose crypto helper links OpenSSL <major>
        foreach(_header ngtcp2.h ngtcp2_crypto.h ngtcp2_crypto_ossl.h)
            file(WRITE "${prefix}/include/ngtcp2/${_header}" "")
        endforeach()
        file(WRITE "${prefix}/lib/libngtcp2${_ext}" "")
        plant_listing("${prefix}/lib/libngtcp2_crypto_ossl${_ext}" ${major} ${major})
    endfunction()
    # project(<case> <build dir> OK|FAIL [AS_PRINTED] <argument>... [MUST <regex>...] [NOT <regex>...]
    #         [LINE <line regex>...] [HAS <verdict line regex>...]) -- the consumer configured: its exit, its output
    # (joined for MUST and NOT, as printed for LINE) and its verdict.txt. AS_PRINTED: cmake with the arguments given and
    # nothing of this harness's, on a build directory a configure of the case made; the output kept in <case>/output.txt.
    function(consumer_project name build expect)
        set(_args -S "${CMAKE_CURRENT_LIST_DIR}/consumer" -B "${build}" --no-warn-unused-cli
                  "-DPLANT_ROOT=${_real}/${name}/root" "-DSTANDINS=${_real}/standins" "-DRESULT_DIR=${_real}/${name}")
        if(GENERATOR)
            list(APPEND _args -G "${GENERATOR}")
        endif()
        if(MAKE_PROGRAM)
            list(APPEND _args "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
        endif()
        if(APPLE)
            list(APPEND _args "-DNGTCP2_OTOOL_EXECUTABLE=${WORK_DIR}/tool")
        else()
            list(APPEND _args "-DCMAKE_READELF=${WORK_DIR}/tool")
        endif()
        set(_list ${ARGN})
        if(ARGC GREATER 3 AND "${ARGV3}" STREQUAL "AS_PRINTED")
            set(_args "")
            list(REMOVE_AT _list 0)
        endif()
        set(_kind DEFINE)
        set(_checks "")
        foreach(_arg IN LISTS _list)
            if(_arg MATCHES "^(MUST|NOT|LINE|HAS)$")
                set(_kind "${_arg}")
            elseif(_kind STREQUAL "DEFINE")
                list(APPEND _args "${_arg}")
            else()
                list(APPEND _checks "${_kind}" "${_arg}")
            endif()
        endforeach()
        file(REMOVE "${_real}/${name}/verdict.txt")
        execute_process(COMMAND "${CMAKE_COMMAND}" ${_args} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
        file(WRITE "${_real}/${name}/output.txt" "${_out}")
        string(REGEX REPLACE "[ \t\r]*\n[ \t]*" " " _flat "${_out}")
        set(_verdict "")
        if(EXISTS "${_real}/${name}/verdict.txt")
            file(READ "${_real}/${name}/verdict.txt" _verdict)
        endif()
        math(EXPR _n "${_projects} + 1")
        set(_projects ${_n} PARENT_SCOPE)
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
            elseif(_kind STREQUAL "LINE" AND NOT "\n${_out}\n" MATCHES "\n${_check}\n")
                string(APPEND _wrong "\n    no line '${_check}' in its output")
            elseif(_kind STREQUAL "HAS" AND NOT "\n${_verdict}" MATCHES "\n${_check}\n")
                string(APPEND _wrong "\n    verdict.txt has no line '${_check}'")
            endif()
        endwhile()
        if(_wrong)
            set(_failures "${_failures}\n  ${name}:${_wrong}\n    verdict.txt: ${_verdict}\n    output: ${_flat}" PARENT_SCOPE)
        endif()
    endfunction()
    # printed_command(<case> <root> <build dir> <out_var>): the command the case's last configure printed on a line of
    # its own, its <it> (<root>) and <build directory> filled in -- what a user who copies it runs. Nothing printed:
    # arguments cmake refuses, so the configure fails.
    function(printed_command name root build out_var)
        file(READ "${_real}/${name}/output.txt" _out)
        if(NOT "\n${_out}\n" MATCHES "\n +cmake (-U [^\n]*)\n")
            set(${out_var} "--no-remedy-command-printed" PARENT_SCOPE)
            return()
        endif()
        string(REPLACE "<build directory>" "@BUILD@" _line "${CMAKE_MATCH_1}")
        string(REPLACE "<it>" "@IT@" _line "${_line}")
        separate_arguments(_line UNIX_COMMAND "${_line}")
        list(TRANSFORM _line REPLACE "@IT@" "${root}")
        list(TRANSFORM _line REPLACE "^@BUILD@$" "${build}")
        set(${out_var} ${_line} PARENT_SCOPE)
    endfunction()
    set(_found "QB=(1|TRUE)")
    set(_qb "-DQB_DIR=${_real}/prefix/lib/cmake/qb")

    # An OpenSSL of another major than qb's, found by the default search: qb not found; the OpenSSL remedy finds qb's.
    set(_name "openssl-major")
    set(_root "${_real}/${_name}/root")
    plant_openssl("${_root}/usr/local" 4.0.0 4 4)
    plant_ngtcp2("${_root}/usr/local" 3)
    plant_openssl("${_root}/o3" 3.5.9 3 3)
    consumer_project(${_name} "${_real}/${_name}/build" FAIL "${_qb}"
                     MUST "this qb was built against OpenSSL 3\\.5\\.9, and this project found OpenSSL 4\\.0\\.0 \\(OPENSSL_INCLUDE_DIR [^,]*/usr/local/include,"
                     LINE "${_ssl_remedy}" NOT "two OpenSSL majors")
    printed_command(${_name} /o3 "${_real}/${_name}/build" _printed)
    consumer_project(${_name} "${_real}/${_name}/build" OK AS_PRINTED ${_printed} NOT "CMake Warning"
                     HAS "${_found}" "OPENSSL_VERSION=3\\.5\\.9" "OPENSSL_INCLUDE_DIR=.*/o3/include")
    # A newer minor of qb's major: found, nothing said.
    set(_name "openssl-minor")
    set(_root "${_real}/${_name}/root")
    plant_openssl("${_root}/usr/local" 3.6.0 3 3)
    plant_ngtcp2("${_root}/usr/local" 3)
    consumer_project(${_name} "${_real}/${_name}/build" OK "${_qb}" NOT "CMake Warning" "NOT FOUND"
                     HAS "${_found}" "OPENSSL_VERSION=3\\.6\\.0" "NGTCP2_OPENSSL=3\\|3")
    # libngtcp2 on another OpenSSL major than qb's and this project's: the remedy is a libngtcp2 on theirs.
    set(_name "ngtcp2-major")
    set(_root "${_real}/${_name}/root")
    plant_openssl("${_root}/usr/local" 3.5.9 3 3)
    plant_ngtcp2("${_root}/usr/local" 4)
    plant_ngtcp2("${_root}/n3" 3)
    consumer_project(${_name} "${_real}/${_name}/build" FAIL "${_qb}"
                     MUST "links OpenSSL 4, and this project links the libssl of OpenSSL 3 " LINE "${_ngtcp2_remedy}"
                     NOT "OPENSSL_ROOT_DIR=<it>")
    printed_command(${_name} /n3 "${_real}/${_name}/build" _printed)
    consumer_project(${_name} "${_real}/${_name}/build" OK AS_PRINTED ${_printed} NOT "CMake Warning"
                     HAS "${_found}" "NGTCP2_CRYPTO_OSSL_LIBRARY=.*/n3/lib/libngtcp2_crypto_ossl[^/]*" "NGTCP2_OPENSSL=3\\|3")
    # The same, from a project whose policies predate CMP0074 (CMake 3.12): Ngtcp2_ROOT still reaches the search.
    set(_name "ngtcp2-major-old-policies")
    set(_root "${_real}/${_name}/root")
    plant_openssl("${_root}/usr/local" 3.5.9 3 3)
    plant_ngtcp2("${_root}/usr/local" 4)
    plant_ngtcp2("${_root}/n3" 3)
    consumer_project(${_name} "${_real}/${_name}/build" FAIL "${_qb}" "-DOLD_POLICIES=ON" LINE "${_ngtcp2_remedy}")
    printed_command(${_name} /n3 "${_real}/${_name}/build" _printed)
    consumer_project(${_name} "${_real}/${_name}/build" OK AS_PRINTED ${_printed} NOT "CMake Warning"
                     HAS "${_found}" "NGTCP2_CRYPTO_OSSL_LIBRARY=.*/n3/lib/libngtcp2_crypto_ossl[^/]*" "POLICY_CMP0074=")
    # This project's libcrypto on another major than qb's, its headers on qb's: the remedy is its OpenSSL entries.
    set(_name "libcrypto-major")
    set(_root "${_real}/${_name}/root")
    plant_openssl("${_root}/usr/local" 3.5.9 3 4)
    plant_ngtcp2("${_root}/usr/local" 3)
    plant_openssl("${_root}/o3" 3.5.9 3 3)
    consumer_project(${_name} "${_real}/${_name}/build" FAIL "${_qb}"
                     MUST "and the libcrypto of OpenSSL 4 " LINE "${_ssl_remedy}" NOT "Ngtcp2_ROOT=<it>")
    printed_command(${_name} /o3 "${_real}/${_name}/build" _printed)
    consumer_project(${_name} "${_real}/${_name}/build" OK AS_PRINTED ${_printed} NOT "CMake Warning"
                     HAS "${_found}" "OPENSSL_INCLUDE_DIR=.*/o3/include")
    # Windows: the OpenSSL remedy removes LIB_EAY* and SSL_EAY* -- MSVC's _DEBUG and _RELEASE among them -- and finds
    # qb's OpenSSL in the same build directory.
    foreach(_variant "windows;OFF" "msvc;ON")
        list(GET _variant 0 _kind)
        list(GET _variant 1 _msvc)
        set(_name "openssl-major-${_kind}")
        set(_root "${_real}/${_name}/root")
        foreach(_prefix "o4w;4.0.0" "o3w;3.5.9")
            list(GET _prefix 0 _dir)
            list(GET _prefix 1 _version)
            file(WRITE "${_root}/${_dir}/include/openssl/ssl.h" "")
            file(WRITE "${_root}/${_dir}/include/openssl/opensslv.h" "# define OPENSSL_VERSION_STR \"${_version}\"\n")
            file(WRITE "${_root}/${_dir}/lib/libssl.lib" "")
            file(WRITE "${_root}/${_dir}/lib/libcrypto.lib" "")
        endforeach()
        consumer_project(${_name} "${_real}/${_name}/build" FAIL "-DQB_DIR=${_real}/prefix-ssl/lib/cmake/qb"
                         "-DCMAKE_SYSTEM_NAME=Windows" "-DMSVC=${_msvc}" "-DOPENSSL_ROOT_DIR=/o4w"
                         MUST "this qb was built against OpenSSL 3\\.5\\.9, and this project found OpenSSL 4\\.0\\.0 "
                         LINE "${_ssl_remedy_windows}")
        printed_command(${_name} /o3w "${_real}/${_name}/build" _printed)
        consumer_project(${_name} "${_real}/${_name}/build" OK AS_PRINTED ${_printed} NOT "CMake Warning"
                         HAS "${_found}" "OPENSSL_VERSION=3\\.5\\.9" "OPENSSL_SSL_LIBRARY=.*/o3w/lib/libssl\\.lib")
    endforeach()
else()
    message(STATUS "installed-quic-openssl: nothing reads a planted library here: the remedies are not run")
endif()
if(_failures)
    message(FATAL_ERROR "installed-quic-openssl: ${_cases} consumers and ${_projects} projects, wrong ones:${_failures}")
endif()
message(STATUS "installed-quic-openssl: ${_cases} consumers and ${_projects} project configures right")
