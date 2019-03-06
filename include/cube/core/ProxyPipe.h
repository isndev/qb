//
// Created by isndev on 12/8/18.
//

#ifndef QB_PROXYPIPE_H
#define QB_PROXYPIPE_H
# include <cube/system/allocator/pipe.h>
# include "ActorId.h"
# include "Event.h"

namespace qb {
    using Pipe = allocator::pipe<CacheLine>;

    /*!
     * @brief Object returned by Actor::getPipe()
     * @class ProxyPipe ProxyPipe.h cube/actor.h
     * @details
     * to define
     */
    class ProxyPipe {
        Pipe *pipe;
        ActorId dest;
        ActorId source;

    public:
        ProxyPipe() = default;
        ProxyPipe(ProxyPipe const &) = default;
        ProxyPipe &operator=(ProxyPipe const &) = default;

        ProxyPipe(Pipe &i_pipe, ActorId i_dest, ActorId i_source)
                : pipe(&i_pipe), dest(i_dest), source(i_source) {}

        /*!
         *
         * @tparam T
         * @tparam _Args
         * @param args
         * @return
         */
        template<typename T, typename ..._Args>
        T &push(_Args &&...args);

        /*!
         *
         * @tparam T
         * @tparam _Args
         * @param size
         * @param args
         * @return
         */
        template<typename T, typename ..._Args>
        T &allocated_push(std::size_t size, _Args &&...args);

        /*!
         *
         * @return
         */
        inline ActorId getDestination() const {
            return dest;
        }

        /*!
         *
         * @return
         */
        inline ActorId getSource() const {
            return source;
        }

    };

} // namespace qb

#endif //QB_PROXYPIPE_H
