#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# The census of an embedded copy's link names (Huly QB-1139): no symbol the archive DEFINES and
# exports may carry the upstream library's own prefix. qb embeds libev (as qev) and qbm-http
# embeds llhttp; both used to export the upstream names, so a program that also linked the
# upstream library -- or anything that bundles it -- had two definitions of each name and the
# link line chose one silently. The copies now link as `qb_` + the upstream name. The witness
# test proves a collision cannot happen for the names it plants; this census proves no exported
# name was left out of the renaming, for the whole archive, on the toolchain that built it.
#
#   cmake -DARCHIVE=<lib> -DBANNED=<p1,p2> -DREQUIRED=<prefix> -DTOOL=<nm|dumpbin path>
#         -DTOOL_KIND=<nm|dumpbin> [-DSTRIP_UNDERSCORE=ON] -P check_symbol_prefix.cmake
#
# Registered by qb_add_symbol_prefix_test() (qbSymbolPrefix.cmake), which picks the tool.
#
# nm (GNU, LLVM, Apple) is read with `-g --defined-only`; dumpbin (MSVC) with `/LINKERMEMBER:1`,
# the archive's own index of the public symbols its members define -- not `/SYMBOLS`, which lists
# nothing for an object compiled with `/GL` (link-time code generation carries no COFF symbols). STRIP_UNDERSCORE removes the one leading `_` that
# Mach-O adds to a C name. A C++ symbol is mangled (`_Z...`, `?...`) and never matches a banned C
# prefix. Vacuity is refused twice: an archive that yields no defined symbol at all, and an
# archive with no symbol under REQUIRED (the renaming would then be absent, not complete).

foreach(_var ARCHIVE BANNED REQUIRED TOOL TOOL_KIND)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "check_symbol_prefix: ${_var} is required")
    endif()
endforeach()
# Comma-separated: a `;` would split the argument in a CTest command line.
string(REPLACE "," ";" BANNED "${BANNED}")
if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "check_symbol_prefix: no archive at ${ARCHIVE}")
endif()

if(TOOL_KIND STREQUAL "nm")
    execute_process(COMMAND "${TOOL}" -g --defined-only "${ARCHIVE}"
                    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
elseif(TOOL_KIND STREQUAL "dumpbin")
    execute_process(COMMAND "${TOOL}" /NOLOGO /LINKERMEMBER:1 "${ARCHIVE}"
                    OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
else()
    message(FATAL_ERROR "check_symbol_prefix: TOOL_KIND must be nm or dumpbin, not ${TOOL_KIND}")
endif()
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "check_symbol_prefix: ${TOOL} failed (${_rc}): ${_err}")
endif()

string(REPLACE "\r" "" _out "${_out}")
string(REPLACE "\n" ";" _lines "${_out}")
set(_names)
set(_in_index OFF)
foreach(_line IN LISTS _lines)
    set(_name "")
    if(TOOL_KIND STREQUAL "nm")
        # `<address> <type> <name>`; member headers (`ev.c.o:`) and blank lines have no type column.
        if(_line MATCHES "^[0-9A-Fa-f]* *[A-Za-z] ([^ ]+)$")
            set(_name "${CMAKE_MATCH_1}")
        endif()
    else()
        # The index follows its `N public symbols` line: `        2B2 qb_ev_run`, an offset then
        # one public symbol the archive defines. A member header (`0 mode`, `4F4 size`) has the
        # same shape, so nothing is read outside an index.
        if(_line MATCHES "public symbols *$")
            set(_in_index ON)
            continue()
        elseif(_line MATCHES "^Archive member name")
            set(_in_index OFF)
        elseif(_in_index AND _line MATCHES "^ +[0-9A-Fa-f]+ +([^ ]+)$")
            set(_name "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(_name STREQUAL "")
        continue()
    endif()
    if(STRIP_UNDERSCORE AND _name MATCHES "^_(.*)$")
        set(_name "${CMAKE_MATCH_1}")
    endif()
    list(APPEND _names "${_name}")
endforeach()
list(REMOVE_DUPLICATES _names)
list(LENGTH _names _total)
if(_total EQUAL 0)
    message(FATAL_ERROR "check_symbol_prefix: ${TOOL} listed no defined symbol in ${ARCHIVE} -- the census read nothing")
endif()

set(_bare)
set(_renamed 0)
foreach(_name IN LISTS _names)
    foreach(_prefix IN LISTS BANNED)
        string(LENGTH "${_prefix}" _plen)
        string(SUBSTRING "${_name}" 0 ${_plen} _head)
        if(_head STREQUAL _prefix)
            list(APPEND _bare "${_name}")
        endif()
    endforeach()
    string(LENGTH "${REQUIRED}" _rlen)
    string(SUBSTRING "${_name}" 0 ${_rlen} _rhead)
    if(_rhead STREQUAL REQUIRED)
        math(EXPR _renamed "${_renamed} + 1")
    endif()
endforeach()

if(_bare)
    list(LENGTH _bare _nbare)
    list(JOIN _bare " " _bare_text)
    message(FATAL_ERROR "check_symbol_prefix: ${_nbare} exported symbol(s) of ${ARCHIVE} keep the upstream "
                        "name and would collide with the upstream library linked in the same program: ${_bare_text}")
endif()
if(_renamed EQUAL 0)
    message(FATAL_ERROR "check_symbol_prefix: no exported symbol of ${ARCHIVE} starts with ${REQUIRED} -- "
                        "the renaming is absent, not complete")
endif()
message(STATUS "check_symbol_prefix: ${ARCHIVE}: ${_total} defined symbols, ${_renamed} under ${REQUIRED}, "
               "none under ${BANNED}")
