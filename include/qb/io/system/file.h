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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../helper.h"

#ifndef _WIN32

extern "C"
{
	int open(char const* pathname, int flags, ...);
}
#else
#include <io.h>
#endif // !_WIN32

#ifndef             QB_IO_SYS_FILE_H_
# define            QB_IO_SYS_FILE_H_

namespace           qb {
    namespace       io {
        namespace   sys {

            /*!
            * @class file sys/file.h qb/io/sys/file.h
            * @ingroup SYS
            */
            class QB_API file {
                int _handle;

            public:

                file();
                file(file const &) = default;
                file(int fd);
                file(std::string const &fname, int flags = O_RDWR);

                int ident() const;
                int fd() const;
                void open(std::string const &fname, int flags = O_RDWR);
                void open(int fd);
                int write(const char *data, std::size_t size);
                int read(char *data, std::size_t size);
                void close();
                bool good() const;

                // unused
                void setBlocking(bool) {}
            };

        } // namespace sys
    } // namespace io
} // namespace qb

#endif // QB_IO_SYS_FILE_H_
