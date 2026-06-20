# Optional vcpkg toolchain.
#
# vcpkg is required on Windows (the project's C/C++ dependencies are not system-
# provided there) but optional on Linux/macOS, which build against system /
# Homebrew packages. CMakePresets points its toolchain at this file so a SINGLE
# preset set works on every platform:
#
#   * if the VCPKG_ROOT environment variable points at a vcpkg checkout, we
#     chainload its toolchain (manifest mode then installs from vcpkg.json);
#   * otherwise this is a no-op and CMake's normal package discovery is used.
#
# Set VCPKG_ROOT once per machine (vcpkg's own recommended setup) to opt in;
# leave it unset to build against system packages. A wrong/stale VCPKG_ROOT is
# tolerated (the EXISTS check falls back to system discovery).
#
# An explicit -DCMAKE_TOOLCHAIN_FILE on the command line overrides this preset
# default entirely, so power users keep full control.

if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    include("$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()
