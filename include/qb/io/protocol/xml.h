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

#ifndef             QB_IO_PROT_XML_H_
# define            QB_IO_PROT_XML_H_
# include <qb/modules/xml/src/pugixml.hpp>

namespace qb {
    namespace io {
        namespace protocol {

            template<typename _IO_>
            class xml : public _IO_ {
            public:
                using _IO_::publish;
                struct message_type {
                    char *data;
                    pugi::xml_document xml;
                } _message;

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

                message_type &getMessage(int size) {
                    auto &buffer = this->_in_buffer;
                    _message.xml.reset();
                    _message.data = buffer.data() + buffer.begin();
                    _message.xml.load_buffer_inplace(_message.data, size);

                    return _message;
                }

                class xml_proxy_writer : public pugi::xml_writer
                {
                    xml &prot;
                public:
                    xml_proxy_writer(xml &parent) : prot(parent) {}

                    virtual void write(const void* data, size_t size) override final
                    {
                        prot.publish(static_cast<const char *>(data), size);
                        *prot._out_buffer.allocate_back(1) = '\0';
                    }
                } _publisher = {*this};

                char *publish(pugi::xml_document const &message) {
                    const auto data = this->_out_buffer.data() + this->_out_buffer.end();
                    message.save(_publisher, "");

                    return data;
                }
            };

        } // namespace protocol
    } // namespace io
} // namespace qb

#endif // QB_IO_PROT_XML_H_
