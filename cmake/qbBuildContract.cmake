# qbBuildContract.cmake -- build contracts: a unit that must not compile (Huly QB-1248).
#
# Included from qbFunctions.cmake, last, beside qbRuntimeDlls.cmake and qbSymbolPrefix.cmake: appending keeps every
# line of qbFunctions.cmake that the docs cite where it was.
#
# A build contract holds a compile-time refusal -- a static_assert -- in place. Its unit must not compile: the ctest
# entry builds it on demand and passes when the build fails with ONE error, the one MESSAGE matches; a unit that
# compiles, or one refused for another reason too, fails it (the verdict is cmake/check_build_contract.cmake, run with
# `cmake -P`). Its control -- the same unit with CONTROL_DEFINITION defined, the refused shape replaced by a sound one --
# is the target of the entry's name: built and linked with the suite, so that the contract's unit is otherwise sound;
# compiled without coverage instrumentation -- it is no code of the suite's, and no report counts it; its link keeps the
# runtime the instrumented libraries need --; listed in the global property QB_BUILD_CONTRACT_TARGETS, which the
# coverage targets build before the run that runs the contracts, never in QB_TEST_TARGETS: it is no test executable.
#
#   qb_add_build_contract(MODULE <module> NAME <name> SOURCE <unit> LINK <target>... MESSAGE <regex>
#                         CONTROL_DEFINITION <macro> [LABELS <label>...])
#
# The entry and the control are <module>-test-unit-<name>, the refused target <module>-contract-<name> (out of ALL).
# MESSAGE holds no `;`, `[` or `]` (the verdict reads its output with them replaced). An argument that follows no
# keyword is refused; a word after LINK or LABELS is read as one of their values. The entry runs with CLICOLOR_FORCE=1,
# so that Ninja keeps the compiler's colours as Make does: in every lane off Windows the verdict reads coloured output
# (the twenty-second independent review found it read none) -- cl and clang-cl print none into a pipe, and qb asks
# none of them, so the Windows lanes read plain diagnostics, Ninja's `FAILED:` alone coloured. The nested build
# compiles the unit and whatever it depends on that is out of date, with CMAKE_BUILD_PARALLEL_LEVEL when it is set, in
# ctest's environment and LANGUAGE=C (the verdict's header says why) -- an environment which must be the compiler's (a
# developer prompt on Windows with Ninja); two at once would race on the generator's state, hence one lock,
# qb-nested-build, for every contract.

function(qb_add_build_contract)
    # PARSE_ARGV: a value is read as it was written, a `;` included, so that the refusal below sees it.
    cmake_parse_arguments(PARSE_ARGV 0 BC "" "MODULE;NAME;SOURCE;MESSAGE;CONTROL_DEFINITION" "LINK;LABELS")
    # DEFINED, never the value: `0` and `OFF` read false (the twenty-third independent review).
    if(DEFINED BC_UNPARSED_ARGUMENTS)
        qb_error_message("qb_add_build_contract: arguments that follow no keyword: '${BC_UNPARSED_ARGUMENTS}'")
    endif()
    foreach(_bc_required MODULE NAME SOURCE LINK MESSAGE CONTROL_DEFINITION)
        if(NOT DEFINED BC_${_bc_required} OR "${BC_${_bc_required}}" STREQUAL "")
            qb_error_message("qb_add_build_contract: ${_bc_required} is required, with a value")
        endif()
    endforeach()
    if(BC_MESSAGE MATCHES ";" OR BC_MESSAGE MATCHES "\\[" OR BC_MESSAGE MATCHES "\\]")
        qb_error_message("qb_add_build_contract: MESSAGE must hold no ';', '[' or ']'")
    endif()
    set(_bc_refused ${BC_MODULE}-contract-${BC_NAME})
    set(_bc_control ${BC_MODULE}-test-unit-${BC_NAME})
    add_executable(${_bc_refused} EXCLUDE_FROM_ALL ${BC_SOURCE})
    add_executable(${_bc_control} ${BC_SOURCE})
    target_compile_definitions(${_bc_control} PRIVATE ${BC_CONTROL_DEFINITION})
    foreach(_bc_target ${_bc_refused} ${_bc_control})
        # The suite's standard, warnings, sanitizer and coverage flags, compiling and linking: a control linked without
        # the sanitizer runtime does not link against an instrumented library.
        _qb_apply_target_properties(${_bc_target})
        qb_apply_test_werror(${_bc_target})
        target_link_libraries(${_bc_target} PRIVATE ${BC_LINK})
    endforeach()
    # The control's objects uninstrumented, after the coverage flags the line above gave it: the last one wins.
    if(QB_COVERAGE_COMPILE_OPTS AND QB_COVERAGE_KIND STREQUAL "gcov")
        target_compile_options(${_bc_control} PRIVATE -fno-profile-arcs -fno-test-coverage)
    elseif(QB_COVERAGE_COMPILE_OPTS AND QB_COVERAGE_KIND STREQUAL "llvm")
        target_compile_options(${_bc_control} PRIVATE -fno-profile-instr-generate -fno-coverage-mapping)
    endif()
    set_property(GLOBAL APPEND PROPERTY QB_BUILD_CONTRACT_TARGETS ${_bc_control})
    add_test(NAME ${_bc_control}
             COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DTARGET=${_bc_refused}" "-DCONFIG=$<CONFIG>"
                     "-DMESSAGE=${BC_MESSAGE}" -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_build_contract.cmake")
    set(_bc_labels tier:unit module:${BC_MODULE} ${BC_LABELS})
    set_tests_properties(${_bc_control} PROPERTIES LABELS "${_bc_labels}" TIMEOUT 300 RESOURCE_LOCK qb-nested-build
                                                   ENVIRONMENT "CLICOLOR_FORCE=1")
endfunction()
