/**
 * @file qb/io/system/ev_config.h
 * @brief Event system configuration settings for the QB IO library.
 *
 * This file contains compile-time configuration settings for the event system
 * used by the qb framework's I/O subsystems, particularly influencing
 * how the underlying event loop (e.g., libev) might be tuned or behave.
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
 * @ingroup IO
 */

#ifndef QB_IO_EV_CONFIG_H_
#define QB_IO_EV_CONFIG_H_

#include <cstdint>
#include <chrono>

namespace qb::io::event {

/**
 * @struct Config
 * @ingroup Async
 * @brief Provides compile-time configuration settings for the asynchronous event system.
 *
 * These settings can influence aspects like default polling timeouts, maximum events
 * processed per poll iteration, and potentially hints for underlying event mechanisms
 * like libev regarding edge-triggered or one-shot notifications if those were to be
 * explicitly controlled through these constants (though libev often handles these automatically).
 */
struct Config {
    /** 
     * @brief Default timeout for the event loop's polling mechanism.
     * Specifies how long the event loop will wait for events before timing out
     * if no events are immediately available. A sensible default to balance
     * responsiveness and CPU usage.
     */
    static constexpr std::chrono::milliseconds DEFAULT_POLL_TIMEOUT{100};
    
    /**
     * @brief Maximum number of events to retrieve and process in a single polling iteration.
     * Helps to prevent one very active set of file descriptors from starving others
     * by limiting the batch size of processed events before the loop might check other states
     * or re-poll.
     */
    static constexpr int MAX_EVENTS_PER_POLL = 64;
    
    /**
     * @brief Hint for whether to prefer edge-triggered notifications if the underlying event system supports it.
     * Edge-triggered (ET) means notifications are delivered only when a state change occurs
     * (e.g., new data arrives on a socket that was previously empty).
     * This is often used with non-blocking I/O and requires careful state management by the application.
     */
    static constexpr bool USE_EDGE_TRIGGERED = true;
    
    /**
     * @brief Hint for whether to prefer one-shot notifications if the underlying event system supports it.
     * One-shot means that after an event is delivered for a file descriptor, the watcher for that
     * descriptor is automatically disabled. It must be explicitly re-enabled if further events are desired.
     * This can simplify some types of event handling logic.
     */
    static constexpr bool USE_ONESHOT = false;
    
    /**
     * @brief A conceptual maximum number of concurrent connections or active file descriptors.
     * This is often a soft limit or a hint for sizing internal structures, rather than a hard
     * enforced limit. The actual system capacity will depend on OS limits, memory, and other factors.
     */
    static constexpr int MAX_CONNECTIONS = 10000;
};

} // namespace qb::io::event

#endif // QB_IO_EV_CONFIG_H_ 