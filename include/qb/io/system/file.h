/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
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

#include "../helper.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32

extern "C" {
int open(char const *pathname, int flags, ...);
}
#else
#    include <io.h>
#endif // !_WIN32

#ifndef QB_IO_SYS_FILE_H_
#    define QB_IO_SYS_FILE_H_

namespace qb::io::sys {

/*!
 * @class file sys/file.h qb/io/sys/file.h
 * @ingroup SYS
 */
class QB_API file {
    int _handle;

public:
    file();
    file(file const &) = default;
    explicit file(int fd);
    explicit file(std::string const &fname, int flags = O_RDWR);

    [[nodiscard]] int ident() const;
    [[nodiscard]] int fd() const;
    [[nodiscard]] bool good() const;
    void open(std::string const &fname, int flags = O_RDWR);
    void open(int fd);
    int write(const char *data, std::size_t size);
    int read(char *data, std::size_t size);
    void close();

    // unused
    [[maybe_unused]] void setBlocking(bool) {}
};

} // namespace qb::io::sys

#endif // QB_IO_SYS_FILE_H_
