#
# qb - C++ Actor Framework
# Copyright (C) 2011-2020 isndev (www.qbaf.io). All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
#         limitations under the License.
#

cxx_gtest(qbm-${PROJECT_NAME}-gtest-session "qb-io qbm-http qbm-websocket" test-session-websocket.cpp)

if (OPENSSL_FOUND)
    file(COPY ${QB_SSL_RESOURCES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()