#
# qb - C++ Actor Framework
# Copyright (c) 2011-2026 qb - isndev (cpp.actor). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# -----------------------------------------------------------------------------
# qb Framework - THE install/export rule
#
# qb and every qbm module are the same shape of repository, so they get the same rule.
# One function emits it; qb/CMakeLists.txt and qb_register_module() each call it once.
#
# Until 3.0.0 this job was written TWICE -- by hand in qb/CMakeLists.txt, and again as
# _qb_module_install_rules() in qbFunctions.cmake -- and no single function could have
# served both. qb's install root was its include directory, copied verbatim; a module's
# was its whole REPOSITORY, re-rooted onto <includedir>/qbm/<name> and filtered by an
# eight-term blacklist (tests|readme|docs|scripts|cmake|examples|benchmarks|not-qb) that
# existed only because test fixtures, the readme books and a vendored fork's build
# material all sat inside the thing being installed. Putting the modules on the same
# src/-is-the-include-root rule as qb (dev/analysis/SOURCE-LAYOUT-3.0.md) is what deleted
# the blacklist, the re-rooting and the second implementation together.
# -----------------------------------------------------------------------------

if(QB_PACKAGE_INCLUDED)
    return()
endif()
set(QB_PACKAGE_INCLUDED TRUE)

# qb_package_include_root - THE RULE, in the one place that decides it.
#
# A repository's src/ IS its include root: its contents are exactly what a consumer types
# after `#include <`, and <includedir> is that same directory listing copied verbatim. So
# the build-tree root and the install-tree root are a PAIR, and both interfaces of every
# qb-shaped target come from here: qb_install_package() copies out_build onto out_install,
# and qb_register_module() hands the same two values to target_include_directories() as
# $<BUILD_INTERFACE:>/$<INSTALL_INTERFACE:>. They cannot drift, because one call produces
# both -- and drift in that pair is not hypothetical: it is why an installed qb once
# configured fine and then failed a consumer's first translation unit on
# "fatal error: 'ev/ev++.h' file not found".
function(qb_package_include_root source_dir out_build out_install)
    include(GNUInstallDirs)
    set(${out_build} "${source_dir}/src" PARENT_SCOPE)
    set(${out_install} "${CMAKE_INSTALL_INCLUDEDIR}" PARENT_SCOPE)
endfunction()

# qb_install_package - every install/export rule that makes <PACKAGE> find_package()-able.
#
#   PACKAGE          `qb`, `qbm-http`. Names the export set, the generated config files and
#                    <libdir>/cmake/<PACKAGE>.
#   NAMESPACE        `qb::` / `qbm::`. install(EXPORT) prefixes each target's EXPORT_NAME
#                    with it, which is why callers pin EXPORT_NAME: the package must spell
#                    its targets exactly as the build tree does, or a consumer switching
#                    from add_subdirectory to find_package breaks on the target name.
#   VERSION          written into <PACKAGE>ConfigVersion.cmake. Required, and resolved by the
#                    caller: a module's config template has to substitute the same value, so
#                    defaulting it here as well would be two defaults for one number.
#   COMPATIBILITY    write_basic_package_version_file() policy. qb ships SameMajorVersion;
#                    a module ships SameMinorVersion, because a prebuilt archive compiled
#                    against inline- and template-heavy qb does not honestly satisfy "any 3.x".
#   CONFIG_TEMPLATE  the *.cmake.in configured into <PACKAGE>Config.cmake. Its @VARS@ are
#                    expanded in the CALLER's scope, so set them before calling.
#   SOURCE_DIR       the repository root. Defaults to CMAKE_CURRENT_SOURCE_DIR. Its src/ is
#                    the include root and its LICENSE ships; nothing else about it is read.
#   HEADER_EXCLUDE   optional regex passed to install(DIRECTORY) as REGEX ... EXCLUDE. qb
#                    needs two (stduuid's vendored Catch2; the qev libevent compat headers
#                    unless QB_EV_LIBEVENT_COMPAT). That a module needs one at all is a
#                    finding, not a feature: the src/ layout means everything under
#                    src/qbm/<name>/ IS the public surface, so an exclusion here says a file
#                    living in the public tree is not public. qbm-pgsql has exactly one
#                    (field_handler.h, dead and non-compiling) and it is forwarded by
#                    qb_register_module(HEADER_EXCLUDE) -- see that function's header.
#   TARGETS          every target that travels in the export set. A bundled non-imported
#                    library that a public target links PUBLIC MUST be listed: its name lands
#                    in INTERFACE_LINK_LIBRARIES, and install(EXPORT) is a hard error on a
#                    name that is in no export set.
#   CMAKE_FILES      absolute paths to extra files installed beside the config: Find*.cmake
#                    modules, and a generated <PACKAGE>Dependencies.cmake. CMake exports
#                    ad-hoc IMPORTED targets BY NAME with no recreation, and a consumer has
#                    no way to invent them, so the package ships the code that does.
#   VENDOR_DIRS      glob expressions naming the vendored upstreams whose notices must ship.
function(qb_install_package)
    set(oneValueArgs PACKAGE NAMESPACE VERSION COMPATIBILITY CONFIG_TEMPLATE SOURCE_DIR HEADER_EXCLUDE)
    set(multiValueArgs TARGETS CMAKE_FILES VENDOR_DIRS)
    cmake_parse_arguments(P "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    if(NOT P_SOURCE_DIR)
        set(P_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    qb_package_include_root("${P_SOURCE_DIR}" _qb_pkg_hdr_from _qb_pkg_hdr_to)
    set(_qb_pkg_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/${P_PACKAGE}")
    set(_qb_pkg_licensedir "${CMAKE_INSTALL_DATAROOTDIR}/licenses/${P_PACKAGE}")

    # --- targets + export set -------------------------------------------------
    if(P_TARGETS)
        install(TARGETS ${P_TARGETS}
                EXPORT ${P_PACKAGE}Targets
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                INCLUDES DESTINATION ${_qb_pkg_hdr_to}
        )
    endif()
    install(EXPORT ${P_PACKAGE}Targets
            FILE ${P_PACKAGE}Targets.cmake
            NAMESPACE ${P_NAMESPACE}
            DESTINATION ${_qb_pkg_cmakedir}
    )

    # --- public headers -------------------------------------------------------
    # One directory copied onto another, and the same two strings both sides of the rule.
    # This is also what ships the vendored forks that physically live under the include
    # root (qb's src/qb/vendor/*, qbm-http's src/qbm/http/vendor/llhttp.h): there is no
    # second root to mirror and therefore no second rule to forget.
    #
    # *.tpp and *.inl are NETS, not dependencies -- as of 3.0 they match nothing. Both notes
    # that used to stand here recorded them as load-bearing, and both were true when written:
    # qbm-http shipped routing/router.tpp and qbm-pgsql shipped three .inl, and dropping either
    # pattern failed a consumer's FIRST translation unit on "file not found" while configuring
    # and installing perfectly. 3.0 merged all four into the headers that include them, so the
    # tree now holds zero of either.
    #
    # The reason they stayed has since EXPIRED IN PART, so record what is actually true rather
    # than leave a justification that reads better than it is. The note used to say "no gate in
    # this repo would catch it on the day such a file is added". That is now wrong for qb:
    # scripts/check-header-extensions.py fails on any .tpp/.inl and runs in qb's own CI
    # (.github/workflows/format-check.yml) as well as from the superproject's
    # dev/agent/verify.sh, where dev/agent/header-rules-negative-control.sh resurrects a .tpp
    # every run to prove it still fires. For a .tpp under qb/, these two patterns are now
    # genuinely vestigial.
    #
    # They stay anyway, on the one leg of the argument that still holds. This rule is shared
    # VERBATIM with qb_register_module(), so it is also every qbm module's header install --
    # and no qbm module runs the extension check in its own CI (each ships only doc-lint.yml).
    # A .inl added to qbm-pgsql is caught by verify.sh at the superproject, and by nothing at
    # all in the module's own pipeline; the failure it would cause is silent at configure AND
    # at install time and surfaces only in a downstream consumer's first TU. The measured cost
    # of keeping the patterns is nothing: FILES_MATCHING patterns that match no file install no
    # file. Do not "clean these up" -- deleting them removes a net and fixes nothing.
    set(_qb_pkg_hdr_exclude)
    if(P_HEADER_EXCLUDE)
        set(_qb_pkg_hdr_exclude REGEX "${P_HEADER_EXCLUDE}" EXCLUDE)
    endif()
    install(
            DIRECTORY "${_qb_pkg_hdr_from}/"
            DESTINATION ${_qb_pkg_hdr_to}
            FILES_MATCHING
            ${_qb_pkg_hdr_exclude}
            PATTERN "*.h"
            PATTERN "*.hpp"
            PATTERN "*.tpp"
            PATTERN "*.inl"
    )

    # --- licence + third-party notices ----------------------------------------
    # Not decoration: Apache-2.0 s.4 requires a copy of the License to reach every
    # recipient, and the MIT / BSD-2 / BSL-1.0 components bundled here all require their
    # notice to be *distributed with* the software -- which a static archive with a
    # vendored parser compiled into it exactly is. Nothing did this until it was written:
    # the header rule above uses FILES_MATCHING, which installs ONLY files matching a
    # pattern, so LICENSE and THIRD-PARTY-NOTICES sitting right beside the vendored headers
    # were silently skipped, and a verified install produced zero licence files of any kind
    # while shipping five vendored upstreams.
    #
    # share/licenses/<pkg>/ rather than share/doc: it is the layout distro packaging tools
    # already look in, and it keeps qb's notices from colliding with a module's.
    set(_qb_pkg_notices "${P_SOURCE_DIR}/LICENSE")
    if(EXISTS "${P_SOURCE_DIR}/THIRD-PARTY-NOTICES")
        list(APPEND _qb_pkg_notices "${P_SOURCE_DIR}/THIRD-PARTY-NOTICES")
    endif()
    install(FILES ${_qb_pkg_notices} DESTINATION "${_qb_pkg_licensedir}")

    # Each vendored unit keeps its licence text beside its own code, so glob rather than
    # hard-code: a unit added later ships its notice without anyone remembering to edit a
    # list, and scripts/check-vendor-attribution.py fails the guard battery if a unit has
    # none to ship. The globs are deliberately one level deep, which is also what keeps
    # stduuid's vendored Catch2 out: it is the fork's test-only material, HEADER_EXCLUDE
    # already drops its headers, and shipping its licence would advertise a dependency the
    # consumer never received.
    foreach(_qb_pkg_glob IN LISTS P_VENDOR_DIRS)
        file(GLOB _qb_pkg_vendor_dirs LIST_DIRECTORIES true "${_qb_pkg_glob}")
        foreach(_qb_pkg_vendor_dir IN LISTS _qb_pkg_vendor_dirs)
            if(NOT IS_DIRECTORY "${_qb_pkg_vendor_dir}")
                continue()
            endif()
            get_filename_component(_qb_pkg_vendor_name "${_qb_pkg_vendor_dir}" NAME)
            file(GLOB _qb_pkg_vendor_notices
                    "${_qb_pkg_vendor_dir}/LICENSE" "${_qb_pkg_vendor_dir}/LICENSE-*"
                    "${_qb_pkg_vendor_dir}/LICENSE.*" "${_qb_pkg_vendor_dir}/LICENSE_*"
                    "${_qb_pkg_vendor_dir}/THIRD-PARTY-NOTICES")
            if(_qb_pkg_vendor_notices)
                install(FILES ${_qb_pkg_vendor_notices}
                        DESTINATION "${_qb_pkg_licensedir}/third-party/${_qb_pkg_vendor_name}")
            endif()
        endforeach()
    endforeach()

    # --- package config -------------------------------------------------------
    write_basic_package_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/${P_PACKAGE}ConfigVersion.cmake"
            VERSION ${P_VERSION}
            COMPATIBILITY ${P_COMPATIBILITY}
    )
    configure_package_config_file(
            "${P_CONFIG_TEMPLATE}"
            "${CMAKE_CURRENT_BINARY_DIR}/${P_PACKAGE}InstallConfig.cmake"
            INSTALL_DESTINATION ${_qb_pkg_cmakedir}
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${P_PACKAGE}InstallConfig.cmake"
            DESTINATION ${_qb_pkg_cmakedir}
            RENAME ${P_PACKAGE}Config.cmake
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${P_PACKAGE}ConfigVersion.cmake"
            DESTINATION ${_qb_pkg_cmakedir}
    )
    foreach(_qb_pkg_extra IN LISTS P_CMAKE_FILES)
        install(FILES "${_qb_pkg_extra}" DESTINATION ${_qb_pkg_cmakedir})
    endforeach()

    qb_status_message("  install: ${P_PACKAGE} -> find_package(${P_PACKAGE} CONFIG) / ${P_NAMESPACE}*")
endfunction()
