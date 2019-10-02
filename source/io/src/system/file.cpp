/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 *         limitations under the License.
 */

#include            <algorithm>
#include            <qb/io/system/file.h>
#include <cstdio>

namespace           qb {
    namespace       io {
        namespace   sys {


            file::file() : _handle(FD_INVALID) {}

            file::file(int fd) : _handle(fd) {}

            file::file(std::string const &fname, int flags)
                    : _handle(::open(fname.c_str(), flags)) {}

            int file::ident() const {
                return _handle;
            }

            void file::open(std::string const &fname, int flags) {
                _handle = ::open(fname.c_str(), flags);
            }

            void file::open(int fd) {
                _handle = fd;
            }

            int file::write(const char *data, std::size_t size) {
                return ::write(_handle, data, size);
            }

            int file::read(char *data, std::size_t size) {
                return ::read(_handle, data, size);
            }

            void file::close() {
                if (good()) {
                    if (::close(_handle))
                        _handle = FD_INVALID;
                    else
                        std::cerr << "Failed to close file" << std::endl;
                }
            }

            bool file::good() const {
                return _handle != FD_INVALID;
            }

        } // namespace sys
    } // namespace io
} // namespace qb
