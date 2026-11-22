#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
#
# A build contract's verdict (Huly QB-1248): the target must not build, and its build must fail with ONE error, the one
# MESSAGE matches -- a unit that compiles has lost the refusal; one refused for another reason, or for that reason and
# another, no longer reads as the refusal it holds (the reviews of Huly QB-1248 found an error printed beside the
# message twice, each a defect of the refusal's own).
#
#   cmake -DBUILD_DIR=<build tree> -DTARGET=<refused target> [-DCONFIG=<config>] -DMESSAGE=<regex>
#         -P check_build_contract.cmake
#
# Registered by qb_add_build_contract() (qbBuildContract.cmake). The nested build runs in ctest's environment with one
# variable set, LANGUAGE=C: gettext's message language, which outranks LC_ALL, LC_MESSAGES and LANG for that alone and
# touches no charset (glibc ignores it under a C, POSIX or C.UTF-8 message locale, or one not installed -- where nothing
# is translated anyway), so that GCC, the linker and make print their severity word in English -- GCC is the one
# toolchain of the suite whose `error` is translated (the twenty-fourth independent review: a French GCC under Ninja
# failed every contract on a correct refusal). Nothing else is forced: VSLANG=1033 made a tree configured with a
# non-English MSVC under Ninja lose the header dependencies of every object the nested build compiled -- Ninja reads
# them by the prefix cl printed at configure time (the twenty-third) -- and MSVC never translates its category
# (`error C2338`; Microsoft's diagnostic format). Clang translates nothing. The output is printed whole, for the log,
# then read with its terminal sequences removed -- qb asks GCC and Clang for colours on every target off Windows
# (qbConfig.cmake), which Make and a forced Ninja keep: the colours put an escape sequence right before `error` (the
# twenty-second independent review: every contract failed under Make).
# A line's severity is its first diagnostic word -- `error`, `fatal error`, `warning`, `note`, `remark`, GCC's
# `sorry, unimplemented`, with MSVC's code after it (`error C2338`, `error LNK2019`, `error D8016`) -- followed by a
# colon, after the start of the line, a blank or a colon: a line is an error when that word is `error`, `fatal error` or
# `sorry, unimplemented`. A note naming `ns::error::x` and a warning whose text holds `error:` are none; neither are
# ninja's `FAILED:`, clang's `1 error generated.` and a source excerpt printed with its gutter (`  405 | ...`, code that
# may hold a label -- one printed without it, by a Clang before 17 or under -fno-diagnostics-show-line-numbers, is read,
# and can only fail a contract). A second error in the same line as the first is not read.
# Every output is held to the one error. A toolchain whose severity word the verdict cannot read -- one translated
# where LANGUAGE does not reach -- fails every contract, with the lines it printed: a red the log explains, not a pass
# on an output no line of which was read (the twenty-fourth: a judgement on the message alone, kept for such an output,
# let a unit refused for another reason pass through the message in a source excerpt). What a read output can still
# hide is said above: a second error on the line of the first.
# `cmake -E env ... --`: a cmake whose path holds `=` is not read as one more variable (the twenty-fifth).

cmake_minimum_required(VERSION 3.24)
foreach(_var BUILD_DIR TARGET MESSAGE)
    if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
        message(FATAL_ERROR "check_build_contract: ${_var} is required")
    endif()
endforeach()
set(_bc_config)
if(DEFINED CONFIG AND NOT "${CONFIG}" STREQUAL "")
    set(_bc_config --config "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env LANGUAGE=C --
                        "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}" ${_bc_config}
                RESULT_VARIABLE _bc_rc OUTPUT_VARIABLE _bc_out ERROR_VARIABLE _bc_out)
message("${_bc_out}")
if(_bc_rc EQUAL 0)
    message(FATAL_ERROR "check_build_contract: ${TARGET} BUILT -- the refusal it holds is gone")
endif()

# The terminal sequences first (a CSI holds `;`): ECMA-48's CSI -- the colours -- and OSC -- a link, which GCC prints
# under -fdiagnostics-urls=always and the suite never asks --, then a carriage return.
string(ASCII 27 _bc_esc)
string(ASCII 7 _bc_bel)
string(REGEX REPLACE "${_bc_esc}\\[[0-?]*[ -/]*[@-~]" "" _bc_text "${_bc_out}")
string(REGEX REPLACE "${_bc_esc}\\][^${_bc_bel}${_bc_esc}]*(${_bc_bel}|${_bc_esc}\\\\)" "" _bc_text "${_bc_text}")
string(REPLACE "\r" "" _bc_text "${_bc_text}")
# Then nothing that can split or join a list element: `;` and the square brackets -- a `;` inside `[...]` stays in its
# element -- are replaced, and the lines become the elements.
string(REPLACE ";" "," _bc_text "${_bc_text}")
string(REPLACE "[" "(" _bc_text "${_bc_text}")
string(REPLACE "]" ")" _bc_text "${_bc_text}")
string(REPLACE "\n" ";" _bc_lines "${_bc_text}")
set(_bc_errors)
foreach(_bc_line IN LISTS _bc_lines)
    if(_bc_line MATCHES "^ *[0-9]* *\\|")
        continue() # GCC's and Clang's source excerpt and caret lines: code, which may hold a label named `error`
    endif()
    string(REGEX MATCH "(^|[ :])(fatal error|error|warning|note|remark|sorry, unimplemented)( [A-Z]+[0-9]+)? ?:( |$)"
           _bc_severity "${_bc_line}")
    if(_bc_severity MATCHES "error|sorry")
        list(APPEND _bc_errors "${_bc_line}")
    endif()
endforeach()
list(LENGTH _bc_errors _bc_count)
if(_bc_count EQUAL 0)
    message(FATAL_ERROR "check_build_contract: ${TARGET} did not build, and no error line was read -- not a refusal "
                        "the verdict can tell (an output in a form, or a language, it does not read: see above)")
endif()
if(NOT _bc_count EQUAL 1)
    list(JOIN _bc_errors "\n  " _bc_list)
    message(FATAL_ERROR "check_build_contract: ${TARGET} failed with ${_bc_count} errors, one expected:\n  ${_bc_list}")
endif()
if(NOT _bc_errors MATCHES "${MESSAGE}")
    message(FATAL_ERROR "check_build_contract: ${TARGET} was refused for another reason: ${_bc_errors}")
endif()
message(STATUS "check_build_contract: ${TARGET} refused, its one error the expected one: ${_bc_errors}")
