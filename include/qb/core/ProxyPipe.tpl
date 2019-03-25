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

#include "ProxyPipe.h"

#ifndef QB_PROXYPIPE_TPL
#define QB_PROXYPIPE_TPL

namespace qb {

    template<typename T, typename ..._Args>
    T &ProxyPipe::push(_Args &&...args) const noexcept {
        constexpr std::size_t BUCKET_SIZE = allocator::getItemSize<T, CacheLine>();
        auto &data = pipe->template allocate_back<T>(std::forward<_Args>(args)...);
        data.id = type_id<T>();
        data.dest = dest;
        data.source = source;
        if constexpr (std::is_base_of<ServiceEvent, T>::value) {
            data.forward = source;
            std::swap(data.id, data.service_event_id);
        }

        data.bucket_size = BUCKET_SIZE;
        return data;
    }

    template<typename T, typename ..._Args>
    T &ProxyPipe::allocated_push(std::size_t size, _Args &&...args) const noexcept {
        size += sizeof(T);
        size = size / sizeof(CacheLine) + static_cast<bool>(size % sizeof(CacheLine));
        auto &data = *(new(reinterpret_cast<T *>(pipe->allocate_back(size))) T(
                std::forward<_Args>(args)...));

        data.id = type_id<T>();
        data.dest = dest;
        data.source = source;
        if constexpr (std::is_base_of<ServiceEvent, T>::value) {
            data.forward = source;
            std::swap(data.id, data.service_event_id);
        }

        data.bucket_size = static_cast<uint16_t>(size);
        return data;
    }

} // namespace qb

#endif //QB_PROXYPIPE_TPL