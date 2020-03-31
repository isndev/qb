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

#ifndef             QB_IO_PROT_JSON_H_
# define            QB_IO_PROT_JSON_H_
# include <qb/modules/json/single_include/nlohmann/json.hpp>
# include "../stream.h"

namespace qb {
    namespace io {
        namespace protocol {

            template<typename _IO_>
            class json : public _IO_ {
            public:
                using _IO_::publish;
                struct message_type {
                    const char *data;
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
                            nlohmann::json::parse(std::string_view(data, size - 1), nullptr, false)
                    };
                }

                char *publish(nlohmann::json const &message) {
                    const auto to_str = message.dump();
                    return static_cast<_IO_ &>(*this).publish(to_str.c_str(), to_str.size() + 1);
                }
            };

        } // namespace protocol
    } // namespace io
} // namespace qb

#endif // QB_IO_PROT_JSON_H_
