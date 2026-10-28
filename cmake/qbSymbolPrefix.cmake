# qbSymbolPrefix.cmake — the census of an embedded copy's link names (Huly QB-1139).
#
# Included from qbFunctions.cmake, last, beside qbRuntimeDlls.cmake: appending keeps every line of
# qbFunctions.cmake that the docs cite where it was.
#
# qb embeds libev (as qev) and qbm-http embeds llhttp. Both copies used to export the upstream
# library's own C names, so a program that ALSO linked the upstream library -- a system libev, a
# Node-style http_parser, anything that bundles one -- held two definitions of each name, and the
# link line chose between them: silently on ld64 (a foreign archive first made qb's loop call the
# foreign ev_loop_new, measured), a `multiple definition` / LNK2005 refusal elsewhere. The copies
# now link as `qb_` + the upstream name. A witness test per copy proves the collision is gone for
# the names it plants; this census proves no exported name escaped the renaming, for the whole
# archive, on the toolchain that built it.
#
#   qb_add_symbol_prefix_test(NAME <ctest name> TARGET <static library target>
#                             BANNED <prefix>... REQUIRED <prefix> [LABELS <label>...])
#
# The tool is the toolchain's nm (GNU, LLVM, Apple: CMAKE_NM), else dumpbin beside the MSVC
# linker. A toolchain with neither, and a target that is not a static archive (a DLL carries no
# COFF symbol table to read), register NO census -- said once at configure time -- rather than a
# vacuous one. The decision itself is cmake/check_symbol_prefix.cmake, run with `cmake -P`.

function(qb_add_symbol_prefix_test)
    cmake_parse_arguments(SP "" "NAME;TARGET;REQUIRED" "BANNED;LABELS" ${ARGN})
    if(NOT SP_NAME OR NOT SP_TARGET OR NOT SP_REQUIRED OR NOT SP_BANNED)
        qb_error_message("qb_add_symbol_prefix_test: NAME, TARGET, BANNED and REQUIRED are required")
    endif()
    if(NOT QB_BUILD_TESTS)
        return()
    endif()
    get_target_property(_sp_type ${SP_TARGET} TYPE)
    if(NOT _sp_type STREQUAL "STATIC_LIBRARY")
        qb_status_message("${SP_NAME} not registered: ${SP_TARGET} is a ${_sp_type}, the census reads static archives")
        return()
    endif()

    set(_sp_tool "")
    set(_sp_kind "")
    if(CMAKE_NM)
        set(_sp_tool "${CMAKE_NM}")
        set(_sp_kind nm)
    elseif(MSVC)
        get_filename_component(_sp_linker_dir "${CMAKE_LINKER}" DIRECTORY)
        find_program(QB_DUMPBIN_EXECUTABLE dumpbin HINTS "${_sp_linker_dir}")
        if(QB_DUMPBIN_EXECUTABLE)
            set(_sp_tool "${QB_DUMPBIN_EXECUTABLE}")
            set(_sp_kind dumpbin)
        endif()
    endif()
    if(NOT _sp_tool)
        qb_status_message("${SP_NAME} not registered: this toolchain has neither nm nor dumpbin")
        return()
    endif()

    # Comma-separated: a `;` would split the argument on the CTest command line.
    list(JOIN SP_BANNED "," _sp_banned)
    if(APPLE)
        set(_sp_strip ON) # Mach-O adds one leading `_` to every C name
    else()
        set(_sp_strip OFF)
    endif()
    add_test(NAME ${SP_NAME}
             COMMAND "${CMAKE_COMMAND}" "-DARCHIVE=$<TARGET_FILE:${SP_TARGET}>" "-DBANNED=${_sp_banned}"
                     "-DREQUIRED=${SP_REQUIRED}" "-DTOOL=${_sp_tool}" "-DTOOL_KIND=${_sp_kind}"
                     "-DSTRIP_UNDERSCORE=${_sp_strip}"
                     -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_symbol_prefix.cmake")
    set_tests_properties(${SP_NAME} PROPERTIES LABELS "${SP_LABELS}" TIMEOUT 60)
endfunction()
