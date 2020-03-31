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

#cxx_benchmark(${PROJECT_NAME}-benchmark-example  "${PROJECT_NAME}" benchmark_example.cpp)
cxx_benchmark(${PROJECT_NAME}-benchmark-ping-pong  "${PROJECT_NAME}" bm-ping-pong.cpp)
cxx_benchmark(${PROJECT_NAME}-benchmark-disruptor-latency  "${PROJECT_NAME}" bm-disruptor-latency.cpp)
cxx_benchmark(${PROJECT_NAME}-benchmark-pipeline-latency  "${PROJECT_NAME}" bm-pipeline-latency.cpp)
cxx_benchmark(${PROJECT_NAME}-benchmark-multicast-latency  "${PROJECT_NAME}" bm-multicast-latency.cpp)
cxx_benchmark(${PROJECT_NAME}-benchmark-ping-pong-latency  "${PROJECT_NAME}" bm-ping-pong-latency.cpp)