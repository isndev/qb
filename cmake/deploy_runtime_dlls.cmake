# deploy_runtime_dlls.cmake
#
# Copies runtime DLLs to a destination directory.
# Designed to be called via `cmake -P` from a POST_BUILD custom command.
#
# Parameters (passed with -D on the cmake command line):
#   DLL_LIST   Semicolon-separated list of DLL paths (may be empty — no-op in that case)
#   DEST_DIR   Destination directory
#
# Why this script exists instead of a direct `cmake -E copy_if_different $<TARGET_RUNTIME_DLLS>`:
# When $<TARGET_RUNTIME_DLLS:target> is empty (e.g. all deps are static libs),
# `cmake -E copy_if_different <dest>` receives no source files and exits with code 1.
# This script is a safe no-op when DLL_LIST is empty.

if(NOT DLL_LIST)
    return()
endif()

foreach(dll IN LISTS DLL_LIST)
    if(EXISTS "${dll}")
        get_filename_component(dll_name "${dll}" NAME)
        set(dest_file "${DEST_DIR}/${dll_name}")
        if(NOT EXISTS "${dest_file}" OR "${dll}" IS_NEWER_THAN "${dest_file}")
            message(STATUS "[qb] Deploying DLL: ${dll_name} -> ${DEST_DIR}")
            file(COPY "${dll}" DESTINATION "${DEST_DIR}")
        endif()
    endif()
endforeach()
