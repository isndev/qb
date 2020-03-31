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

#ifndef             QB_IO_TRANSPORT_STCP_H
# define            QB_IO_TRANSPORT_STCP_H
# include "../tcp/ssl/socket.h"
# include "../stream.h"

namespace qb {
    namespace io {
        namespace transport {

        class stcp : public stream<io::tcp::ssl::socket> {
            public:
                // Derived class should define :
                // using message_type = const char *;
                // int getMessageSize();
                // message_type getMessage();
            };

        } // namespace transport
    } // namespace io
} // namespace qb

#endif // QB_IO_TRANSPORT_STCP_H
