#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The default logger's startup diagnostic answers whether its file was OPENED (Huly QB-327). Every process that links
# qb-io with QB_WITH_LOGGING opens `./qb.1.log` during static initialisation, before main(), and warns on stderr when it
# cannot. The warning used to ask whether that PATH existed: a directory named qb.1.log exists, the open fails, every
# log line is discarded, and nothing was said.
#
# Run by ctest as qb-io-test-unit-logger-startup-diagnostic (registered when QB_WITH_LOGGING is ON):
#   cmake -DPROBE=<any qb-io executable> -DWORK_DIR=<scratch dir> -P <this file>
# The probe is started fresh in a prepared working directory per case, with --gtest_list_tests so it does nothing else.
#
if(NOT PROBE OR NOT WORK_DIR)
    message(FATAL_ERROR "logger-startup-diagnostic: PROBE and WORK_DIR are required")
endif()

set(_warning "WARNING: qb logging is enabled")
set(_failures "")

# run(<case> <dir>): start the probe in <dir>; sets _rc, _err. GTEST_OUTPUT is unset: a listing writes its report
# there, relative to <dir>, and the read-only case would fail on that write rather than on the log file.
macro(_qb_probe_in case dir)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=GTEST_OUTPUT "${PROBE}" --gtest_list_tests
                    WORKING_DIRECTORY "${dir}" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        list(APPEND _failures "${case}: the probe exited with ${_rc}:\n${_err}")
    endif()
endmacro()

file(REMOVE_RECURSE "${WORK_DIR}")

# 1. A writable, empty directory: the file is created, nothing is said.
set(_dir "${WORK_DIR}/writable")
file(MAKE_DIRECTORY "${_dir}")
_qb_probe_in("writable directory" "${_dir}")
if(_err MATCHES "${_warning}")
    list(APPEND _failures "writable directory: a warning for a log file that was opened:\n${_err}")
endif()
if(NOT EXISTS "${_dir}/qb.1.log" OR IS_DIRECTORY "${_dir}/qb.1.log")
    list(APPEND _failures "writable directory: ./qb.1.log was not created")
endif()

# 2. ./qb.1.log is a DIRECTORY: the path exists, the open fails -- the case the existence check could not see.
set(_dir "${WORK_DIR}/obstructed")
file(MAKE_DIRECTORY "${_dir}/qb.1.log")
_qb_probe_in("qb.1.log is a directory" "${_dir}")
if(NOT _err MATCHES "${_warning}")
    list(APPEND _failures "qb.1.log is a directory: the log file was not opened and no warning was printed")
endif()

# 3. A directory without write permission: the open fails. POSIX only (no mode bits to clear on Windows), and only
#    unprivileged (root writes anyway): otherwise the case is reported as not run, never passed silently.
if(CMAKE_HOST_UNIX)
    execute_process(COMMAND id -u OUTPUT_VARIABLE _uid OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_uid STREQUAL "0")
        message(STATUS "logger-startup-diagnostic: read-only directory case NOT RUN (root ignores mode bits)")
    else()
        set(_dir "${WORK_DIR}/read-only")
        file(MAKE_DIRECTORY "${_dir}")
        file(CHMOD "${_dir}" PERMISSIONS OWNER_READ OWNER_EXECUTE)
        _qb_probe_in("read-only directory" "${_dir}")
        file(CHMOD "${_dir}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
        if(NOT _err MATCHES "${_warning}")
            list(APPEND _failures "read-only directory: the log file was not opened and no warning was printed")
        endif()
    endif()
else()
    message(STATUS "logger-startup-diagnostic: read-only directory case NOT RUN (no POSIX mode bits on this host)")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
if(_failures)
    string(REPLACE ";" "\n" _failures "${_failures}")
    message(FATAL_ERROR "logger-startup-diagnostic (Huly QB-327):\n${_failures}")
endif()
message(STATUS "logger-startup-diagnostic: the startup warning follows the open, not the path")
