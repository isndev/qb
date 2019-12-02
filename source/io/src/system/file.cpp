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
//#include <cstdio>
//#ifdef _WIN32
//#include <io.h>
//#endif
namespace           qb {
    namespace       io {
        namespace   sys {


            file::file() : _handle(FD_INVALID) {}

            file::file(int fd) : _handle(fd) {}

            file::file(std::string const &fname, int flags)
				: _handle(FD_INVALID) {
				open(fname, flags);
			}

            int file::ident() const {
                return _handle;
            }

            int file::fd() const {
                return _handle;
            }

            void file::open(std::string const &fname, int flags) {
				close();
#ifdef _WIN32
				_handle = ::_open(fname.c_str(), flags);
#else
                _handle = ::open(fname.c_str(), flags);
#endif
            }

            void file::open(int fd) {
				close();
                _handle = fd;
            }

            int file::write(const char *data, std::size_t size) {
#ifdef _WIN32
				return ::_write(_handle, data, static_cast<unsigned int>(size));
#else
				return ::write(_handle, data, static_cast<unsigned int>(size));
#endif
            }

            int file::read(char *data, std::size_t size) {
#ifdef _WIN32
				return ::_read(_handle, data, static_cast<unsigned int>(size));
#else
				return ::read(_handle, data, static_cast<unsigned int>(size));
#endif
            }

            void file::close() {
                if (good()) {
#ifdef _WIN32
					auto ret = ::_close(_handle);
#else
					auto ret = ::close(_handle);
#endif

                    if (!ret)
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
