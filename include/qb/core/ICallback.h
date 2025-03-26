/**
 * @file qb/core/ICallback.h
 * @brief Callback interface for the QB Actor Framework
 * 
 * This file defines the ICallback interface which provides a mechanism for actors
 * to receive periodic callbacks during each VirtualCore loop iteration. This allows
 * actors to perform background tasks, periodic operations, or polling without relying
 * on event-driven behavior.
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
 * @ingroup Core
 */

#ifndef QB_ICALLBACK_H
#define QB_ICALLBACK_H

namespace qb {

/*!
 * @interface ICallback core/ICallback.h qb/icallback.h
 * @ingroup Core
 * @brief Interface for actor callbacks
 * @details
 * ICallback provides an interface for implementing actor callbacks that are executed
 * during each VirtualCore loop iteration. This allows actors to perform periodic tasks
 * or background operations.
 * 
 * Example usage:
 * @code
 * class MyActor : public Actor, public ICallback {
 *     void onCallback() override {
 *         // Perform periodic tasks here
 *     }
 * };
 * @endcode
 */
class ICallback {
public:
    /*!
     * @brief Virtual destructor
     */
    virtual ~ICallback() = default;

    /*!
     * @brief Callback function executed during each VirtualCore loop iteration
     * @details
     * This pure virtual function must be implemented by derived classes.
     * It will be called during each VirtualCore loop iteration.
     */
    virtual void onCallback() = 0;
};

using icallback = ICallback;

} // namespace qb
#endif // QB_ICALLBACK_H

