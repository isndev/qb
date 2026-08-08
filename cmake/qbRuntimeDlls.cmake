# qbRuntimeDlls.cmake — Windows runtime-DLL deployment for test and benchmark executables.
#
# Included from qbFunctions.cmake; a no-op everywhere but Windows.
#
# The problem it solves, measured rather than assumed:
#
#   `$<TARGET_RUNTIME_DLLS:tgt>` only collects imported targets whose TYPE is SHARED_LIBRARY.
#   Every vcpkg dependency that arrives through a CMake **Find module** arrives as
#   UNKNOWN_LIBRARY instead, with IMPORTED_LOCATION naming the *import library*:
#
#       OpenSSL::SSL     TYPE=UNKNOWN_LIBRARY  IMPORTED_LOCATION_RELEASE=.../lib/libssl.lib
#       OpenSSL::Crypto  TYPE=UNKNOWN_LIBRARY  IMPORTED_LOCATION_RELEASE=.../lib/libcrypto.lib
#       ZLIB::ZLIB       TYPE=UNKNOWN_LIBRARY  IMPORTED_LOCATION_RELEASE=.../lib/z.lib
#
#   So the generator expression is not empty by accident on this platform, it is empty *by
#   construction* and always will be. Combined with VCPKG_APPLOCAL_DEPS=OFF — set because
#   vcpkg's per-target applocal step races itself into ERROR_SHARING_VIOLATION — nothing
#   deployed anything at all: every test executable died in the loader with
#   `z.dll: cannot open shared object file`, and **ctest did not fail, it HUNG**, because the
#   loader raises a modal dialog. A hang with 0% CPU is the worst failure shape there is.
#
# Why ONE target per destination and not a POST_BUILD per executable:
#
#   ~300 test targets writing the same DLLs into the same directory under `ninja -j` is the
#   very ERROR_SHARING_VIOLATION race that got VCPKG_APPLOCAL_DEPS turned off. Re-creating it
#   here would only move the failure. A single writer, ordered ahead of the executables by
#   add_dependencies(), has neither problem and is also far less work.

# qb_ensure_runtime_dll_deployer(<dest_dir> <out_target_var>)
#
# Returns, in <out_target_var>, the name of the shared deployer target for <dest_dir>, creating
# it on first request and reusing it afterwards. Empty string on non-Windows, or when there is
# no vcpkg tree to deploy from.
function(qb_ensure_runtime_dll_deployer DEST_DIR OUT_TARGET)
    set(${OUT_TARGET} "" PARENT_SCOPE)
    if(NOT WIN32)
        return()
    endif()

    string(MAKE_C_IDENTIFIER "qb_deploy_dlls_${DEST_DIR}" _tgt)
    if(TARGET ${_tgt})
        set(${OUT_TARGET} "${_tgt}" PARENT_SCOPE)
        return()
    endif()

    set(_dirs "")
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
        # A generator expression, so the multi-config Visual Studio generator picks per
        # configuration at BUILD time rather than at configure time. Getting this wrong is not
        # subtle on Windows: vcpkg keeps debug DLLs (z**d**.dll, an MDd-linked OpenSSL) under
        # debug/bin, and a Debug executable that finds the release z.dll fails to load exactly
        # like one that finds nothing.
        list(APPEND _dirs "$<IF:$<CONFIG:Debug>,${_root}/debug/bin,${_root}/bin>")
    endif()

    if(NOT _dirs)
        return()
    endif()

    # A multi-config generator appends the configuration to RUNTIME_OUTPUT_DIRECTORY, so under
    # Visual Studio the executables are in <dest>/Release while <dest> itself holds none of them.
    # Deploying to <dest> there put the DLLs one directory ABOVE every consumer of them. That was
    # survivable only by accident: qb_add_test() gives each test WORKING_DIRECTORY "<dest>", and
    # Windows searches the current directory, so 350 of 351 still loaded. The one test registered
    # by a hand-written add_test() -- qbm/http/tests/CMakeLists.txt, no WORKING_DIRECTORY -- did
    # not, and hung in the loader's modal dialog for its full 300 s TIMEOUT rather than failing.
    # Putting the DLLs where the executables actually are removes the dependence on CWD entirely.
    set(_dest "${DEST_DIR}")
    if(CMAKE_CONFIGURATION_TYPES)
        set(_dest "${DEST_DIR}/$<CONFIG>")
    endif()

    add_custom_target(${_tgt}
        COMMAND ${CMAKE_COMMAND}
            "-DDLL_DIRS=$<JOIN:${_dirs},;>"
            "-DDEST_DIR=${_dest}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/deploy_runtime_dlls.cmake"
        COMMENT "[qb] Deploying runtime DLLs -> ${_dest}"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    set_target_properties(${_tgt} PROPERTIES FOLDER "qb/internal")
    set(${OUT_TARGET} "${_tgt}" PARENT_SCOPE)
endfunction()
