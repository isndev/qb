#include "ProxyPipe.h"

#ifndef CUBE_PROXYPIPE_TPL
#define CUBE_PROXYPIPE_TPL

namespace cube {

    template<typename T, typename ..._Args>
    T &ProxyPipe::push(_Args &&...args) {
        constexpr std::size_t BUCKET_SIZE = allocator::getItemSize<T, CacheLine>();
        auto &data = pipe->template allocate_back<T>(std::forward<_Args>(args)...);
        data.id = type_id<T>();
        data.dest = dest;
        data.source = source;
        if constexpr (std::is_base_of<ServiceEvent, T>::value) {
            data.forward = source;
            std::swap(data.id, data.service_event_id);
        }

        data.state = 0;
        data.bucket_size = BUCKET_SIZE;
        return data;
    }

    template<typename T, typename ..._Args>
    T &ProxyPipe::allocated_push(std::size_t size, _Args &&...args) {
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

        data.state = 0;
        data.bucket_size = size;
        return data;
    }

} // namespace cube

#endif //CUBE_PROXYPIPE_TPL