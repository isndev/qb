/**
 * @file qb/io/system/ev_config.h
 * @brief Event system configuration for qb framework
 *
 * This file contains configuration settings for the event system
 * used by the qb framework's I/O subsystems.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
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
 * limitations under the License.
 */

#ifndef QB_IO_EV_CONFIG_H_
#define QB_IO_EV_CONFIG_H_

#include <cstdint>
#include <chrono>

namespace qb::io::event {

/**
 * @brief Event system configuration settings
 */
struct Config {
    // Default poll timeout in milliseconds
    static constexpr std::chrono::milliseconds DEFAULT_POLL_TIMEOUT{100};
    
    // Maximum number of events to process in a single poll
    static constexpr int MAX_EVENTS_PER_POLL = 64;
    
    // Whether to use edge-triggered notifications (if available)
    static constexpr bool USE_EDGE_TRIGGERED = true;
    
    // Whether to use one-shot notifications (if available)
    static constexpr bool USE_ONESHOT = false;
    
    // Maximum concurrent connections
    static constexpr int MAX_CONNECTIONS = 10000;
};

} // namespace qb::io::event

#endif // QB_IO_EV_CONFIG_H_ 