//
// Created by isndev on 12/4/18.
//

#ifndef CUBE_CUBE_H
#define CUBE_CUBE_H
# include <iostream>
# include <vector>
# include <unordered_map>
// include from cube
# include <cube/system/lockfree/mpsc.h>
# include "Event.h"
# include "CoreSet.h"

namespace cube {

    class Core;

    /*!
     * @class Main engine/Main.h cube/main.h
     * @ingroup Engine
     * @brief Engine main class
     * @details
     */
    class Main {
        friend class Core;
        constexpr static const uint64_t MaxRingEvents =
                ((std::numeric_limits<uint16_t>::max)()) / CUBE_LOCKFREE_CACHELINE_BYTES;
        //////// Types
        using MPSCBuffer = lockfree::mpsc::ringbuffer<CacheLine, MaxRingEvents, 0>;

        static std::atomic<uint64_t> sync_start;

    private:
        CoreSet _core_set;
        std::vector<MPSCBuffer *> _mail_boxes;
        std::unordered_map<uint8_t, Core *> _cores;

        bool send(Event const &event) const;
        MPSCBuffer &getMailBox(uint8_t const id) const;
        std::size_t getNbCore() const;
    public:

        Main() = delete;
        Main(std::unordered_set<uint8_t> const &core_set);
        ~Main();

        void start(bool async = true) const;
        void join() const;

    public:

        template<typename _Actor, typename ..._Init>
        ActorId addActor(std::size_t index, _Init &&...init);
        template<template<typename _Trait> typename _Actor, typename _Trait, typename ..._Init>
        ActorId addActor(std::size_t index, _Init &&...init);
    };

} // namespace cube

#endif //CUBE_CUBE_H
