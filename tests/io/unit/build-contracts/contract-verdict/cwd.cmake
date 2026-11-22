# Writes to FILE the directory it runs in as CMake reads it: script mode sets CMAKE_CURRENT_BINARY_DIR to the working
# directory as the system resolved it -- on POSIX its physical path, symlinks and `..` resolved by the kernel; Windows
# keeps a junction in it, as it does for every path (CMake 4 takes $PWD when that names the same directory: check.cmake
# unsets it). check.cmake runs it in its output directory to read the path the outputs were written through -- written
# to a file, not printed, so that no console code page, colour or other output of the child can alter it.
#   cmake -DFILE=<file> -P cwd.cmake     (in the directory to read)
if(NOT FILE)
    message(FATAL_ERROR "cwd.cmake: FILE is required")
endif()
file(WRITE "${FILE}" "${CMAKE_CURRENT_BINARY_DIR}")
