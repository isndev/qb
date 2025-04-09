/**
 * @file qb/core/Pipe.tpp
 * @brief Template implementation for the Pipe class
 *
 * This file contains the template implementation of the Pipe class methods defined
 * in Pipe.h. It provides the actual implementation of event construction and pushing
 * through the communication channel between actors.
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

#ifndef QB_PROXYPIPE_TPL
#define QB_PROXYPIPE_TPL

namespace qb {

template <typename T, typename... _Args>
T &
Pipe::push(_Args &&...args) const noexcept {
    constexpr std::size_t BUCKET_SIZE = allocator::getItemSize<T, EventBucket>();
    auto &data  = pipe->template allocate_back<T>(std::forward<_Args>(args)...);
    data.id     = data.template type_to_id<T>();
    data.dest   = dest;
    data.source = source;
    if constexpr (std::is_base_of_v<ServiceEvent, T>) {
        data.forward = source;
        std::swap(data.id, data.service_event_id);
    }

    data.bucket_size = BUCKET_SIZE;
    return data;
}

template <typename T, typename... _Args>
T &
Pipe::allocated_push(std::size_t size, _Args &&...args) const noexcept {
    size += sizeof(T);
    size = size / sizeof(EventBucket) + static_cast<bool>(size % sizeof(EventBucket));
    auto &data = *(new (reinterpret_cast<T *>(pipe->allocate_back(size)))
                       T(std::forward<_Args>(args)...));

    data.id     = data.template type_to_id<T>();
    data.dest   = dest;
    data.source = source;
    if constexpr (std::is_base_of_v<ServiceEvent, T>) {
        data.forward = source;
        std::swap(data.id, data.service_event_id);
    }

    data.bucket_size = static_cast<uint16_t>(size);
    return data;
}

} // namespace qb

#endif // QB_PROXYPIPE_TPL