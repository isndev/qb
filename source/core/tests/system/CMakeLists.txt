#
# qb - C++ Actor Framework
# Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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

cxx_gtest(${PROJECT_NAME}-gtest-system-main "${PROJECT_NAME}" test-main.cpp)
cxx_gtest(${PROJECT_NAME}-gtest-system-io "${PROJECT_NAME}" test-io.cpp)
cxx_gtest(${PROJECT_NAME}-gtest-system-actor-add "${PROJECT_NAME}" test-actor-add.cpp)
cxx_gtest(${PROJECT_NAME}-gtest-system-actor-callback "${PROJECT_NAME}" test-actor-callback.cpp)
cxx_gtest(${PROJECT_NAME}-gtest-system-actor-dependency "${PROJECT_NAME}" test-actor-dependency.cpp)
cxx_gtest(${PROJECT_NAME}-gtest-system-actor-event "${PROJECT_NAME}" test-actor-event.cpp)
cxx_gtest(${PROJECT_NAME}-gtest-system-actor-service-event "${PROJECT_NAME}" test-actor-service-event.cpp)