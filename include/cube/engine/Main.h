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
     * This is the Main engine class, initialized with desired CoreSet.
     */
    class Main {
        friend class Core;
        constexpr static const uint64_t MaxRingEvents =
                ((std::numeric_limits<uint16_t>::max)()) / CUBE_LOCKFREE_CACHELINE_BYTES;
        //////// Types
        using MPSCBuffer = lockfree::mpsc::ringbuffer<CacheLine, MaxRingEvents, 0>;

        static std::atomic<uint64_t> sync_start;
        static bool                  is_running;
        static void onSignal(int signal);

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

        /*!
         * @brief Start the engine
         * @param async has blocking execution
         * @note
         * If async = false the main thread will by used by a Core engine.
         */
        void start(bool async = true) const;

        /*!
         * @brief Stop the engine
         * @note
         * Same effect as receiving SIGINT Signal.
         */
        void stop() const;

        /*!
         * @brief Wait until engine terminates
         * @note
         * You can avoid calling this function if main was started with async=false.
         */
        void join() const;

    public:

        /*!
         * @brief Create new referenced _Actor
         * @tparam _Actor DerivedActor type
         * @param index Core index
         * @param args arguments to forward to the constructor of the _Actor
         * @return ActorId of the created _Actor
         * @details
         * create and initialize new _Actor on Core index.\n
         * example:
         * @code
         * auto id = this->template addActor<MyActor>(0, param1, param2);
         * @endcode
         * @attention
         * This function is available only when the engine is not running.
         */
        template<typename _Actor, typename ..._Args>
        ActorId addActor(std::size_t index, _Args &&...args);

        /*!
         * @brief Create new referenced _Actor<_Trait>
         * @tparam _Actor DerivedActor type
         * @tparam _Trait _Actor template argument
         * @param index Core index
         * @param args arguments to forward to the constructor of the _Actor<_Trait>
         * @return ActorId of the created _Actor<Trait>
         * @details
         * create and initialize new _Actor on Core index.\n
         * example:
         * @code
         * auto id = this->template addActor<MyActor, MyTrait>(0, param1, param2);
         * @endcode
         * @attention
         * This function is available only when the engine is not running.
         */
        template<template<typename _Trait> typename _Actor, typename _Trait, typename ..._Args>
        ActorId addActor(std::size_t index, _Args &&...args);

    };

} // namespace cube

#endif //CUBE_CUBE_H
