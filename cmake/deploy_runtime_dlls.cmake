# deploy_runtime_dlls.cmake
#
# Copies runtime DLLs to a destination directory.
# Designed to be called via `cmake -P`, either from a POST_BUILD custom command or from the
# single shared deployer target that qb_add_test() / qb_add_benchmark() depend on.
#
# Parameters (passed with -D on the cmake command line):
#   DLL_LIST   Semicolon-separated list of DLL *files* (may be empty — no-op in that case)
#   DLL_DIRS   Semicolon-separated list of *directories*; every *.dll in each is copied
#   DEST_DIR   Destination directory
#
# Why this script exists instead of a direct `cmake -E copy_if_different $<TARGET_RUNTIME_DLLS>`:
# When $<TARGET_RUNTIME_DLLS:target> is empty (e.g. all deps are static libs),
# `cmake -E copy_if_different <dest>` receives no source files and exits with code 1.
# This script is a safe no-op when both inputs are empty.
#
# Why DLL_DIRS exists at all — measured, not assumed:
#   `$<TARGET_RUNTIME_DLLS>` only collects imported targets of type SHARED_LIBRARY whose
#   IMPORTED_LOCATION names the .dll. Every vcpkg dependency that arrives through a CMake
#   Find module arrives as UNKNOWN_LIBRARY pointing at the *import library* instead:
#       OpenSSL::SSL    TYPE=UNKNOWN_LIBRARY  IMPORTED_LOCATION_RELEASE=.../lib/libssl.lib
#       OpenSSL::Crypto TYPE=UNKNOWN_LIBRARY  IMPORTED_LOCATION_RELEASE=.../lib/libcrypto.lib
#       ZLIB::ZLIB      TYPE=UNKNOWN_LIBRARY  IMPORTED_LOCATION_RELEASE=.../lib/z.lib
#   so the generator expression is not merely empty by accident here, it is empty *by
#   construction* and always will be. With VCPKG_APPLOCAL_DEPS=OFF (set because vcpkg's
#   per-target applocal races itself into ERROR_SHARING_VIOLATION) that left NOTHING deploying
#   the DLLs: every test executable died at load with `z.dll: cannot open shared object file`,
#   and ctest did not fail — it HUNG on the loader's modal dialog. DLL_DIRS is what actually
#   carries the payload; DLL_LIST stays for genuinely-SHARED imported targets.

if(NOT DEST_DIR)
    message(FATAL_ERROR "deploy_runtime_dlls: DEST_DIR is required")
endif()

set(_dlls "")
if(DLL_LIST)
    list(APPEND _dlls ${DLL_LIST})
endif()

foreach(dir IN LISTS DLL_DIRS)
    if(IS_DIRECTORY "${dir}")
        file(GLOB _from_dir "${dir}/*.dll")
        list(APPEND _dlls ${_from_dir})
    endif()
endforeach()

if(NOT _dlls)
    return()
endif()

list(REMOVE_DUPLICATES _dlls)
file(MAKE_DIRECTORY "${DEST_DIR}")

foreach(dll IN LISTS _dlls)
    if(EXISTS "${dll}")
        get_filename_component(dll_name "${dll}" NAME)
        set(dest_file "${DEST_DIR}/${dll_name}")
        if(NOT EXISTS "${dest_file}" OR "${dll}" IS_NEWER_THAN "${dest_file}")
            message(STATUS "[qb] Deploying DLL: ${dll_name} -> ${DEST_DIR}")
            file(COPY "${dll}" DESTINATION "${DEST_DIR}")
        endif()
    endif()
endforeach()
