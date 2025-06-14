#
# qb - C++ Actor Framework
# Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
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
# limitations under the License.
#

# -----------------------------------------------------------------------------
# qb Core Unit Tests - CMake Configuration
#
# This file configures the unit tests for the qb Core component.
# It defines test targets for specific components using Google Test framework.
# -----------------------------------------------------------------------------

# Create unit test for the event router component
# This uses the custom cxx_gtest macro defined in the parent project
cxx_gtest(${PROJECT_NAME}-gtest-unit-event-router "${PROJECT_NAME}" test-event-router.cpp)

# Create unit test for the timestamp component
cxx_gtest(${PROJECT_NAME}-gtest-unit-timestamp "${PROJECT_NAME}" test-timestamp.cpp)

# Create unit test for the string component
cxx_gtest(${PROJECT_NAME}-gtest-unit-string "${PROJECT_NAME}" test-string.cpp)