//
// Created by isndev on 12/8/18.
//

#ifndef CUBE_PROXYPIPE_H
#define CUBE_PROXYPIPE_H
# include "../../allocator/pipe.h"
# include "ActorId.h"
# include "Event.h"

namespace cube {
    using Pipe = allocator::pipe<CacheLine>;

    class ProxyPipe {
        ActorId dest;
        ActorId source;
        Pipe *pipe;

        template<typename T = cube::CacheLine>
        T *allocate(std::size_t &size) {
            if (size % sizeof(cube::CacheLine))
                size = size * sizeof(T) / sizeof(CacheLine) + 1;
            else
                size /= sizeof(CacheLine);

            return reinterpret_cast<T *>(pipe->allocate_back(size));
        }

    public:
        ProxyPipe() = default;
        ProxyPipe(ProxyPipe const &) = default;
        ProxyPipe &operator=(ProxyPipe const &) = default;

        ProxyPipe(Pipe &pipe, ActorId dest, ActorId source)
                : pipe(&pipe), dest(dest), source(source) {}

        template<typename T, typename ..._Init>
        T &push(_Init &&...init) {
            auto &data = pipe->template allocate_back<T>(std::forward<_Init>(init)...);
            data.id = type_id<T>();
            data.dest = dest;
            data.source = source;
            if constexpr (std::is_base_of<ServiceEvent, T>::value) {
                data.forward = source;
                std::swap(data.id, data.service_event_id);
            }

            data.state = 0;
            data.bucket_size = sizeof(T) / CUBE_LOCKFREE_CACHELINE_BYTES;
            return data;
        }

        template<typename T, typename ..._Init>
        T &allocated_push(std::size_t size, _Init &&...init) {
            size += sizeof(T);
            auto &data = *(new(reinterpret_cast<T *>(this->template allocate<char>(size))) T(
                    std::forward<_Init>(init)...));

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

        ActorId getDest() const {
            return dest;
        }

        ActorId getSource() const {
            return source;
        }

    };

}

#endif //CUBE_PROXYPIPE_H
