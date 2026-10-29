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

cmake_minimum_required(VERSION 3.24) # the list commands below keep empty elements (CMP0007 NEW)
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
# Every step below is one pass over the whole listing (list FILTER / TRANSFORM): a Debug archive lists tens of
# thousands of weak C++ instantiations, and a loop that appended a name at a time copied the growing list at each
# one -- qbm-http's 61 322 names took the census past two minutes, and past its 60 s timeout, on a Debug
# (feature-gates) tree, against 0.7 s for nm itself.
set(_names)
if(TOOL_KIND STREQUAL "nm")
    # `<address> <type> <name>`; member headers (`ev.c.o:`) and blank lines have no type column.
    string(REPLACE "\n" ";" _names "${_out}")
    list(FILTER _names INCLUDE REGEX "^[0-9A-Fa-f]* *[A-Za-z] [^ ]+$")
    list(TRANSFORM _names REPLACE "^[0-9A-Fa-f]* *[A-Za-z] " "")
else()
    # Each index follows its `N public symbols` line: `        2B2 qb_ev_run`, an offset then one public symbol
    # the archive defines, up to the next `Archive member name`. A member header (`0 mode`, `4F4 size`) has the
    # same shape, so nothing is read outside an index.
    set(_rest "${_out}")
    while(_rest MATCHES "public symbols *\n(.*)$")
        set(_rest "${CMAKE_MATCH_1}")
        string(FIND "${_rest}" "Archive member name" _stop)
        if(_stop EQUAL -1)
            set(_index "${_rest}")
            set(_rest "")
        else()
            string(SUBSTRING "${_rest}" 0 ${_stop} _index)
            string(SUBSTRING "${_rest}" ${_stop} -1 _rest)
        endif()
        string(REPLACE "\n" ";" _index "${_index}")
        list(FILTER _index INCLUDE REGEX "^ +[0-9A-Fa-f]+ +[^ ]+$")
        list(TRANSFORM _index REPLACE "^ +[0-9A-Fa-f]+ +" "")
        list(APPEND _names ${_index})
    endwhile()
endif()
if(STRIP_UNDERSCORE)
    list(TRANSFORM _names REPLACE "^_" "")
endif()
list(REMOVE_DUPLICATES _names)
list(LENGTH _names _total)
if(_total EQUAL 0)
    message(FATAL_ERROR "check_symbol_prefix: ${TOOL} listed no defined symbol in ${ARCHIVE} -- the census read nothing")
endif()

# A prefix is a C identifier's head: it enters the regular expressions below as it is, so anything else is refused.
foreach(_prefix IN LISTS BANNED REQUIRED)
    if(NOT _prefix MATCHES "^[A-Za-z0-9_]+$")
        message(FATAL_ERROR "check_symbol_prefix: '${_prefix}' is not an identifier prefix")
    endif()
endforeach()
list(JOIN BANNED "|" _banned_re)
set(_bare ${_names})
list(FILTER _bare INCLUDE REGEX "^(${_banned_re})")
set(_under ${_names})
list(FILTER _under INCLUDE REGEX "^${REQUIRED}")
list(LENGTH _under _renamed)

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
