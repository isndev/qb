/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
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

#ifdef QB_LOGGER
#include "nanolog/nanolog.cpp"
#endif
#include "logger.cpp"
#include "pipe.cpp"
#include "uri.cpp"
#include "system/sys__socket.cpp"
#include "system/file.cpp"
#include "tcp/listener.cpp"
#include "tcp/socket.cpp"

#include <qb/uuid.h>
namespace qb {
uuid generate_random_uuid() {
    static uuids::uuid_random_generator gen{[]() {
        static auto seed_data = std::array<int, std::mt19937::state_size> {
            [](){
                std::random_device rd;
                std::array<int, std::mt19937::state_size> new_seed;
                std::generate(std::begin(new_seed), std::end(new_seed), std::ref(rd));
                return new_seed;
            }()};
        static std::seed_seq seq(std::begin(seed_data), std::end(seed_data));
        static std::mt19937 generator(seq);
        return &generator;
    }()};
    return gen();
}
} // namespace qb

#ifdef QB_IO_WITH_SSL
#    include "tcp/ssl/init.cpp"
#    include "tcp/ssl/listener.cpp"
#    include "tcp/ssl/socket.cpp"
#    include "crypto.cpp"
#endif
#ifdef QB_IO_WITH_ZLIB
#include "compression.cpp"
#endif
#include "async/listener.cpp"
#include "udp/socket.cpp"
#include "stream.cpp"