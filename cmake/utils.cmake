# Defines functions and macros useful for building Google Test and
# Google Mock.
#
# Note:
#
# - This file will be run twice when building Google Mock (once via
#   Google Test's CMakeLists.txt, and once via Google Mock's).
#   Therefore it shouldn't have any side effects other than defining
#   the functions and macros.
#
# - The functions/macros defined in this file may depend on Google
#   Test and Google Mock's option() definitions, and thus must be
#   called *after* the options have been defined.

# Tweaks CMake's default compiler/linker settings to suit Google Test's needs.
#
# This must be a macro(), as inside a function string() can only
# update variables in the function scope.
include(CMakeParseArguments)

macro(fix_default_compiler_settings_)
  if (MSVC)
    # For MSVC, CMake sets certain flags to defaults we want to override.
    # This replacement code is taken from sample in the CMake Wiki at
    # https://gitlab.kitware.com/cmake/community/wikis/FAQ#dynamic-replace.
    foreach (flag_var
            CMAKE_C_FLAGS CMAKE_C_FLAGS_DEBUG CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_MINSIZEREL CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
      #      message("DEBUG ${flag_var}=${${flag_var}}")
      if (NOT BUILD_SHARED_LIBS)
        # When Google Test is built as a shared library, it should also use
        # shared runtime libraries.  Otherwise, it may end up with multiple
        # copies of runtime library data in different modules, resulting in
        # hard-to-find crashes. When it is built as a static library, it is
        # preferable to use CRT as static libraries, as we don't have to rely
        # on CRT DLLs being available. CMake always defaults to using shared
        # CRT libraries, so we override that default here.
        # string(REPLACE "/MD" "-MT" ${flag_var} "${${flag_var}}")
      endif()
      # We prefer more strict warning checking for building Google Test.
      # Replaces /W3 with /W4 in defaults.
      string(REPLACE "/W3" "/W4" ${flag_var} "${${flag_var}}")

      # Prevent D9025 warning for targets that have exception handling
      # turned off (/EHs-c- flag). Where required, exceptions are explicitly
      # re-enabled using the cxx_exception_flags variable.
      string(REPLACE "/EHsc" "" ${flag_var} "${${flag_var}}")
    endforeach()
  endif()
endmacro()

# Defines the compiler/linker flags used to build Google Test and
# Google Mock.  You can tweak these definitions to suit your need.  A
# variable's value is empty before it's explicitly assigned to.
macro(config_compiler_and_linker)
  # Note: pthreads on MinGW is not supported, even if available
  # instead, we use windows threading primitives
  unset(${QB_PREFIX_UPPER}_HAS_PTHREAD)
  if (NOT cube_disable_pthreads AND NOT MINGW)
    # Defines CMAKE_USE_PTHREADS_INIT and CMAKE_THREAD_LIBS_INIT.
    find_package(Threads)
    if (CMAKE_USE_PTHREADS_INIT)
      set(${QB_PREFIX_UPPER}_HAS_PTHREAD ON)
    endif()
  endif()

  fix_default_compiler_settings_()
  if (MSVC)
    # Newlines inside flags variables break CMake's NMake generator.
    # TODO(vladl@google.com): Add -RTCs and -RTCu to debug builds.
    set(cxx_base_flags "-GS -W4 -WX -wd4251 -wd4275 -nologo -J -Zi")
    set(cxx_base_flags "${cxx_base_flags} -D_UNICODE -DUNICODE -DWIN32 -D_WIN32 -DNOMINMAX")
    set(cxx_base_flags "${cxx_base_flags} -DSTRICT -DWIN32_LEAN_AND_MEAN")
    set(cxx_exception_flags "-EHsc -D_HAS_EXCEPTIONS=1")
    set(cxx_no_exception_flags "-EHs-c- -D_HAS_EXCEPTIONS=0")
    set(cxx_no_rtti_flags "-GR-")
    # Suppress "unreachable code" warning
    # http://stackoverflow.com/questions/3232669 explains the issue.
    set(cxx_base_flags "${cxx_base_flags} -wd4702")
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(cxx_base_flags "-Wall -Wshadow -Werror")
    set(cxx_exception_flags "-fexceptions")
    set(cxx_no_exception_flags "-fno-exceptions")
  elseif (CMAKE_COMPILER_IS_GNUCXX)
    set(cxx_base_flags "-Wall -Wshadow -Werror")
    if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS 7.0.0)
      set(cxx_base_flags "${cxx_base_flags} -Wno-error=dangling-else")
    endif()
    set(cxx_exception_flags "-fexceptions")
    set(cxx_no_exception_flags "-fno-exceptions")
    # Until version 4.3.2, GCC doesn't define a macro to indicate
    # whether RTTI is enabled.  Therefore we define ${QB_PREFIX_UPPER}_HAS_RTTI
    # explicitly.
    set(cxx_no_rtti_flags "-fno-rtti -D${QB_PREFIX_UPPER}_HAS_RTTI=0")
    set(cxx_strict_flags
            "-Wextra -Wno-unused-parameter -Wno-missing-field-initializers")
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "SunPro")
    set(cxx_exception_flags "-features=except")
    # Sun Pro doesn't provide macros to indicate whether exceptions and
    # RTTI are enabled, so we define ${QB_PREFIX_UPPER}_HAS_* explicitly.
    set(cxx_no_exception_flags "-features=no%except -D${QB_PREFIX_UPPER}_HAS_EXCEPTIONS=0")
    set(cxx_no_rtti_flags "-features=no%rtti -D${QB_PREFIX_UPPER}_HAS_RTTI=0")
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "VisualAge" OR
          CMAKE_CXX_COMPILER_ID STREQUAL "XL")
    # CMake 2.8 changes Visual Age's compiler ID to "XL".
    set(cxx_exception_flags "-qeh")
    set(cxx_no_exception_flags "-qnoeh")
    # Until version 9.0, Visual Age doesn't define a macro to indicate
    # whether RTTI is enabled.  Therefore we define ${QB_PREFIX_UPPER}_HAS_RTTI
    # explicitly.
    set(cxx_no_rtti_flags "-qnortti -D${QB_PREFIX_UPPER}_HAS_RTTI=0")
  elseif (CMAKE_CXX_COMPILER_ID STREQUAL "HP")
    set(cxx_base_flags "-AA -mt")
    set(cxx_exception_flags "-D${QB_PREFIX_UPPER}_HAS_EXCEPTIONS=1")
    set(cxx_no_exception_flags "+noeh -D${QB_PREFIX_UPPER}_HAS_EXCEPTIONS=0")
    # RTTI can not be disabled in HP aCC compiler.
    set(cxx_no_rtti_flags "")
  endif()

  # Coverage
  if (COVERAGE_COMPILER_FLAGS)
    set(cxx_base_flags "${cxx_base_flags} ${COVERAGE_COMPILER_FLAGS}")
  endif()

  # The pthreads library is available and allowed?
  if (DEFINED ${QB_PREFIX_UPPER}_HAS_PTHREAD)
    set(${QB_PREFIX_UPPER}_HAS_PTHREAD_MACRO "-D${QB_PREFIX_UPPER}_HAS_PTHREAD=1")
  else()
    set(${QB_PREFIX_UPPER}_HAS_PTHREAD_MACRO "-D${QB_PREFIX_UPPER}_HAS_PTHREAD=0")
  endif()
  set(cxx_base_flags "${cxx_base_flags} ${${QB_PREFIX_UPPER}_HAS_PTHREAD_MACRO}")

  # For building cube's own tests and samples.
  set(cxx_exception "${cxx_base_flags} ${cxx_exception_flags}")
  set(cxx_no_exception
          "${CMAKE_CXX_FLAGS} ${cxx_base_flags} ${cxx_no_exception_flags}")
  set(cxx_default "${cxx_exception}")
  set(cxx_no_rtti "${cxx_default} ${cxx_no_rtti_flags}")

  # For building the cube libraries.
  set(cxx_strict "${cxx_default} ${cxx_strict_flags}")
  if (${QB_PREFIX_UPPER}_WITH_RTTI)
    set(cxx_default_lib "${cxx_default} ${cxx_strict_flags}")
  else()
    set(cxx_default_lib "${cxx_default} ${cxx_strict_flags} ${cxx_no_rtti_flags}")
  endif()
endmacro()

# Defines the cube & cube_main libraries.  User tests should link
# with one of them.
function(cxx_library_with_type name type cxx_flags)
  if (CMAKE_VERBOSE_MAKEFILE)
    message("Build library ${name} with flags: ${cxx_flags}")
  endif()
  # type can be either STATIC or SHARED to denote a static or shared library.
  # ARGN refers to additional arguments after 'cxx_flags'.
  add_library(${name} ${type} ${ARGN})
  if (NOT type STREQUAL "INTERFACE")
    set_target_properties(${name}
            PROPERTIES
            COMPILE_FLAGS "${cxx_flags}")
    # Generate debug library name with a postfix.
    set_target_properties(${name}
            PROPERTIES
            DEBUG_POSTFIX "-d")
    # Set the output directory for build artifacts
    set_target_properties(${name}
            PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
            PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    # make PDBs match library name
    get_target_property(pdb_debug_postfix ${name} DEBUG_POSTFIX)
    set_target_properties(${name}
            PROPERTIES
            PDB_NAME "${name}"
            PDB_NAME_DEBUG "${name}${pdb_debug_postfix}"
            COMPILE_PDB_NAME "${name}"
            COMPILE_PDB_NAME_DEBUG "${name}${pdb_debug_postfix}")

    if (BUILD_SHARED_LIBS OR type STREQUAL "SHARED")
      set_target_properties(${name}
              PROPERTIES
              COMPILE_FLAGS "${cxx_flags}")
      target_compile_definitions(${name} PUBLIC ${QB_PREFIX_UPPER}_DYNAMIC=1)
      if (NOT "${CMAKE_VERSION}" VERSION_LESS "2.8.11")
        target_compile_definitions(${name} INTERFACE ${QB_PREFIX_UPPER}_LINKED_AS_SHARED=1)
      endif()
    endif()
    if (DEFINED ${QB_PREFIX_UPPER}_HAS_PTHREAD)
      if ("${CMAKE_VERSION}" VERSION_LESS "3.1.0")
        set(threads_spec ${CMAKE_THREAD_LIBS_INIT})
      else()
        set(threads_spec Threads::Threads)
      endif()
      target_link_libraries(${name} PUBLIC ${threads_spec})
    endif()
  endif()
endfunction()

########################################################################
#
# Helper functions for creating build targets.

function(cxx_shared_library name cxx_flags)
  cxx_library_with_type(${name} SHARED "${cxx_flags}" ${ARGN})
endfunction()

function(cxx_library name cxx_flags)
  cxx_library_with_type(${name} "" "${cxx_flags}" ${ARGN})
endfunction()

macro(SUBDIRLIST result curdir)
  FILE(GLOB children RELATIVE ${curdir} ${curdir}/*)
  set(dirlist "")
  foreach(child ${children})
    if(IS_DIRECTORY ${curdir}/${child})
      LIST(APPEND dirlist ${child})
    endif()
  endforeach()
  set(${result} ${dirlist})
endmacro()

# qb_register_module(NAME name
#                    VERSION 1.0.0
#                    FLAGS ${cxx_default_lib}
#                    DEPENDENCIES libs...
#                    SOURCES sources...)
# register a named qb C++ module that depends on the given libraries and
# is built from the given source files with the given compiler flags.
function(qb_register_module)
  set(options NONE)
  set(oneValueArgs NAME VERSION FLAGS URL)
  set(multiValueArgs DEPENDENCIES SOURCES)
  cmake_parse_arguments(Module "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if (Module_NAME)
    set(Module_NAME "qbm-${Module_NAME}")
    message(STATUS "Load ${Module_NAME} Module")
    if (NOT ${Module_FLAGS})
      set(Module_FLAGS ${cxx_default_lib})
    endif()
    if (${Module_SOURCES})
      cxx_library_with_type(${Module_NAME} "" "${Module_FLAGS}" ${Module_SOURCE})
      target_link_libraries(${Module_NAME} ${QB_PREFIX}-core)
    else()
      cxx_library_with_type(${Module_NAME} "INTERFACE" "")
      target_link_libraries(${Module_NAME} INTERFACE ${QB_PREFIX}-core)
    endif()
    #    target_include_directories(${Module_NAME} ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(${Module_NAME} INTERFACE
            "$<BUILD_INTERFACE:${QB_DIRECTORY}/include;${QB_DIRECTORY}/modules;${CMAKE_SOURCE_DIR}/modules>"
            "$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}>")

    if (Module_DEPENDENCIES)
      string(REPLACE " " ";" LIB_LIST ${Module_DEPENDENCIES})
      foreach (lib ${LIB_LIST})
        target_link_libraries(${Module_NAME} ${lib})
      endforeach()
    endif()
  else()
    message(FATAL_ERROR "qb_register: Missing module NAME")
  endif()

endfunction()

function(qb_load_modules path)
  message(STATUS "Load modules on path : ${path}")
  SUBDIRLIST(list ${path})

  foreach(subdir ${list})
    add_subdirectory(${path}/${subdir} ${CMAKE_CURRENT_BINARY_DIR}/qb-module/${subdir})
  endforeach()
  include_directories(path)
endfunction()

# cxx_executable_with_flags(name cxx_flags libs srcs...)
#
# creates a named C++ executable that depends on the given libraries and
# is built from the given source files with the given compiler flags.
function(cxx_executable_with_flags name cxx_flags libs)
  if (CMAKE_VERBOSE_MAKEFILE)
    message("Build executable ${name} with flags: ${cxx_flags}")
  endif()
  add_executable(${name} ${ARGN})
  if (MSVC)
    # BigObj required for tests.
    set(cxx_flags "${cxx_flags} -bigobj")
  endif()
  if (cxx_flags)
    set_target_properties(${name}
            PROPERTIES
            COMPILE_FLAGS "${cxx_flags}")
  endif()
  if (BUILD_SHARED_LIBS)
    set_target_properties(${name}
            PROPERTIES
            COMPILE_DEFINITIONS "${QB_PREFIX_UPPER}_LINKED_AS_SHARED_LIBRARY=1")
  endif()
  # To support mixing linking in static and dynamic libraries, link each
  # library in with an extra call to target_link_libraries.
  string(REPLACE " " ";" LIB_LIST ${libs})
  foreach (lib ${LIB_LIST})
    target_link_libraries(${name} ${lib})
  endforeach()
endfunction()

# cxx_executable(name dir lib srcs...)
#
# creates a named target that depends on the given libs and is built
# from the given source files.  dir/name.cc is implicitly included in
# the source file list.
function(cxx_executable name libs)
  cxx_executable_with_flags(
          ${name} "${cxx_default_lib}" "${libs}" ${ARGN})
endfunction()

# Sets PYTHONINTERP_FOUND and PYTHON_EXECUTABLE.
find_package(PythonInterp)

# cxx_test_with_flags(name cxx_flags libs srcs...)
#
# creates a named C++ test that depends on the given libs and is built
# from the given source files with the given compiler flags.
function(cxx_test_with_flags name cxx_flags libs)
  cxx_executable_with_flags(${name} "${cxx_flags}" "${libs}" ${ARGN})
  if (MINGW)
    add_test(NAME ${name}
            COMMAND "powershell" "-Command" "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/RunTest.ps1" "$<TARGET_FILE:${name}>")
  else()
    add_test(NAME ${name}
            COMMAND "$<TARGET_FILE:${name}>")
  endif()
endfunction()

# cxx_test(name libs srcs...)
#
# creates a named test target that depends on the given libs and is
# built from the given source files.
function(cxx_test name libs)
  cxx_test_with_flags("${name}" "${cxx_default_lib}" "${libs}"
          ${ARGN})
endfunction()

# cxx_gtest(name libs srcs...)
#
# creates a named test target that depends on the given libs and is
# built from the given source files.
function(cxx_gtest name libs)
  cxx_test_with_flags("${name}" "${cxx_default_lib}" "${libs} gtest gtest_main"
          ${ARGN})
endfunction()

# qb_register_module_gtest(NAME module_name
#                          TESTNAME name
#                          FLAGS ${cxx_default_lib}
#                          DEPENDENCIES libs...
#                          SOURCES sources...)
# register a named qb C++ test module that depends on the given libraries and
# is built from the given source files with the given compiler flags.
function(qb_register_module_gtest)
  set(options NONE)
  set(oneValueArgs NAME TESTNAME FLAGS)
  set(multiValueArgs SOURCES DEPENDENCIES)
  cmake_parse_arguments(Module "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  if(NOT Module_NAME OR NOT Module_TESTNAME)
    message(FATAL_ERROR "qb failed to register module test, missing NAME or TESTNAME")
    return()
  endif()
  if (NOT Module_SOURCES)
    message(FATAL_ERROR "qb failed to register module test, missing SOURCES")
    return()
  endif()
  if (NOT ${Module_FLAGS})
    set(Module_FLAGS ${cxx_default_lib})
  endif()
  set(Module_NAME "qbm-${Module_NAME}-gtest-${Module_TESTNAME}")
  message(STATUS "Load ${Module_NAME} Test")
  cxx_test_with_flags(
          "${Module_NAME}"
          "${Module_FLAGS}"
          "${Module_DEPENDENCIES} ${QB_PREFIX}-core gtest gtest_main"
          ${Module_SOURCES}
  )
endfunction()

# cxx_benchmark(name libs srcs...)
#
# creates a named benchmark target that depends on the given libs and is
# built from the given source files.
function(cxx_benchmark name libs)
  #  if (MSVC)
  #    set(cxx_benchmark_flags "${cxx_default_lib} -MD")
  #  else()
  set(cxx_benchmark_flags "${cxx_default_lib}")
  #  endif()
  cxx_test_with_flags("${name}" "${cxx_benchmark_flags}" "${libs} benchmark"
          ${ARGN})
endfunction()

# py_test(name)
#
# creates a Python test with the given name whose main module is in
# test/name.py.  It does nothing if Python is not installed.
function(py_test name)
  if (PYTHONINTERP_FOUND)
    if ("${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}" VERSION_GREATER 3.1)
      if (CMAKE_CONFIGURATION_TYPES)
        # Multi-configuration build generators as for Visual Studio save
        # output in a subdirectory of CMAKE_CURRENT_BINARY_DIR (Debug,
        # Release etc.), so we have to provide it here.
        if (WIN32 OR MINGW)
          add_test(NAME ${name}
                  COMMAND powershell -Command ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/RunTest.ps1
                  ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}.py
                  --build_dir=${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG> ${ARGN})
        else()
          add_test(NAME ${name}
                  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}.py
                  --build_dir=${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG> ${ARGN})
        endif()
      else (CMAKE_CONFIGURATION_TYPES)
        # Single-configuration build generators like Makefile generators
        # don't have subdirs below CMAKE_CURRENT_BINARY_DIR.
        if (WIN32 OR MINGW)
          add_test(NAME ${name}
                  COMMAND powershell -Command ${CMAKE_CURRENT_BINARY_DIR}/RunTest.ps1
                  ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}.py
                  --build_dir=${CMAKE_CURRENT_BINARY_DIR} ${ARGN})
        else()
          add_test(NAME ${name}
                  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}.py
                  --build_dir=${CMAKE_CURRENT_BINARY_DIR} ${ARGN})
        endif()
      endif (CMAKE_CONFIGURATION_TYPES)
    else()
      # ${CMAKE_CURRENT_BINARY_DIR} is known at configuration time, so we can
      # directly bind it from cmake. ${CTEST_CONFIGURATION_TYPE} is known
      # only at ctest runtime (by calling ctest -c <Configuration>), so
      # we have to escape $ to delay variable substitution here.
      if (WIN32 OR MINGW)
        add_test(NAME ${name}
                COMMAND powershell -Command ${CMAKE_CURRENT_BINARY_DIR}/RunTest.ps1
                ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}.py
                --build_dir=${CMAKE_CURRENT_BINARY_DIR}/\${CTEST_CONFIGURATION_TYPE} ${ARGN})
      else()
        add_test(NAME ${name}
                COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/${name}.py
                --build_dir=${CMAKE_CURRENT_BINARY_DIR}/\${CTEST_CONFIGURATION_TYPE} ${ARGN})
      endif()
    endif()
  endif(PYTHONINTERP_FOUND)
endfunction()

# install_project(targets...)
#
# Installs the specified targets and configures the associated pkgconfig files.
function(install_project)
  if(INSTALL_${QB_PREFIX_UPPER})
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    # Install the project targets.
    install(TARGETS ${ARGN}
            EXPORT ${targets_export_name}
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
      # Install PDBs
      foreach(t ${ARGN})
        get_target_property(t_pdb_name ${t} COMPILE_PDB_NAME)
        get_target_property(t_pdb_name_debug ${t} COMPILE_PDB_NAME_DEBUG)
        get_target_property(t_pdb_output_directory ${t} PDB_OUTPUT_DIRECTORY)
        install(FILES
                "${t_pdb_output_directory}/\${CMAKE_INSTALL_CONFIG_NAME}/$<$<CONFIG:Debug>:${t_pdb_name_debug}>$<$<NOT:$<CONFIG:Debug>>:${t_pdb_name}>.pdb"
                DESTINATION ${CMAKE_INSTALL_LIBDIR}
                OPTIONAL)
      endforeach()
    endif()
    # Configure and install pkgconfig files.
    foreach(t ${ARGN})
      set(configured_pc "${generated_dir}/${t}.pc")
      configure_file("${PROJECT_SOURCE_DIR}/cmake/${t}.pc.in"
              "${configured_pc}" @ONLY)
      install(FILES "${configured_pc}"
              DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
    endforeach()
  endif()
endfunction()

function(print_target_properties tgt)
  if(NOT TARGET ${tgt})
    message("There is no target named '${tgt}'")
    return()
  endif()

  # this list of properties can be extended as needed
  set(CMAKE_PROPERTY_LIST SOURCE_DIR BINARY_DIR COMPILE_DEFINITIONS
          COMPILE_OPTIONS INCLUDE_DIRECTORIES LINK_LIBRARIES COMPILE_FLAGS)

  message("Configuration for target ${tgt}")

  foreach (prop ${CMAKE_PROPERTY_LIST})
    get_property(propval TARGET ${tgt} PROPERTY ${prop} SET)
    if (propval)
      get_target_property(propval ${tgt} ${prop})
      message (STATUS "${prop} = ${propval}")
    endif()
  endforeach(prop)

endfunction(print_target_properties)