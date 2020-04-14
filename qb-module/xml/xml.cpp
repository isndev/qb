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

#include "xml.h"
#include <xml/src/pugixml.cpp>

class xml_proxy_writer : public pugi::xml_writer {
    qb::allocator::pipe<char> &prot;

public:
    xml_proxy_writer(qb::allocator::pipe<char> &parent)
        : prot(parent) {}

    virtual void
    write(const void *data, size_t size) override final {
        prot.put(static_cast<const char *>(data), size);
    }
};

namespace qb::allocator {

template <>
pipe<char> &
pipe<char>::put<qb::xml::document>(const qb::xml::document &x) {
    xml_proxy_writer w = {*this};

    x.save(w, "");
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<qb::xml::node>(const qb::xml::node &x) {
    xml_proxy_writer w = {*this};

    x.print(w);
    return *this;
}

template <>
pipe<char> &
pipe<char>::put<qb::xml::attribute>(const qb::xml::attribute &x) {
    return (*this << '\"' << x.name() << "\"=\"" << x.value() << "\"");
}

template <>
pipe<char> &
pipe<char>::put<qb::xml::text>(const qb::xml::text &x) {
    return (*this << x.get());
}

} // namespace qb::allocator