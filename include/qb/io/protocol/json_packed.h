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

#ifndef             QB_IO_PROT_JSON_PACKED_H_
# define            QB_IO_PROT_JSON_PACKED_H_
# include <qb/modules/json/single_include/nlohmann/json.hpp>
# include "../stream.h"

namespace qb {
    namespace io {
        namespace protocol {

            template<typename _IO_>
            class json_packed : public _IO_ {
            public:
                struct message_type {
                    char *data;
                    nlohmann::json json;
                };

                int getMessageSize() {
                    auto &buffer = this->_in_buffer;
                    auto i = buffer.begin();
                    while (i < buffer.end()) {
                        if (buffer.data()[i] == '\0')
                            return i - buffer.begin() + 1;
                        ++i;
                    }
                    return 0;
                }

                message_type getMessage(int size) {
                    auto &buffer = this->_in_buffer;
                    auto data = buffer.data() + buffer.begin();

                    return {
                            data,
                            nlohmann::json::from_msgpack(data)
                    };
                }

                char *publish(nlohmann::json const &message) {
                    const auto packed = nlohmann::json::to_msgpack(message);
                    return static_cast<_IO_ &>(*this).publish(static_cast<char const *>(packed.data()), packed.size());
                }
            };

        } // namespace protocol
    } // namespace io
} // namespace qb

#endif // QB_IO_PROT_JSON_PACKED_H_
