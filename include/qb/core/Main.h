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

#ifndef QB_MAIN_H
#define QB_MAIN_H
# include <iostream>
# include <vector>
# include <unordered_map>
// include from qb
# include <qb/system/lockfree/mpsc.h>
# include "Event.h"
# include "CoreSet.h"

namespace qb {

    class VirtualCore;

    /*!
     * @class Main core/Main.h qb/main.h
     * @ingroup Core
     * @brief Core main class
     * @details
     * This is the Main engine class, initialized with desired CoreSet.
     */
    class Main {
        friend class VirtualCore;
        constexpr static const uint64_t MaxRingEvents =
                (((std::numeric_limits<uint16_t>::max)()) / QB_LOCKFREE_CACHELINE_BYTES);
        //////// Types
        using MPSCBuffer = lockfree::mpsc::ringbuffer<CacheLine, MaxRingEvents, 0>;

        static std::atomic<uint64_t> sync_start;
        static bool                  is_running;
        static void onSignal(int signal);

    private:
        CoreSet _core_set;
        std::vector<MPSCBuffer *> _mail_boxes;
        std::unordered_map<uint8_t, VirtualCore *> _cores;

        void __init__();
        bool send(Event const &event) const;
        MPSCBuffer &getMailBox(uint8_t const id) const;
        std::size_t getNbCore() const;
    public:

        /*!
         * @class CoreBuilder core/Main.h qb/main.h
         * @brief Helper to build Actors in VirtualCore
         */
        class CoreBuilder {
        public:
            using ActorIdList = std::vector<ActorId>;
        private:
            friend class Main;

            const uint16_t _index;
            Main &_main;
            ActorIdList _ret_ids;
            bool _valid;

            CoreBuilder(Main &main, uint16_t const index)
                    : _index(index)
                    , _main(main)
                    , _valid(true)
            {}

            CoreBuilder() = delete;
        public:
            CoreBuilder(CoreBuilder const &rhs);

            /*!
             * @brief Create new _Actor
             * @tparam _Actor DerivedActor type
             * @param args arguments to forward to the constructor of the _Actor
             * @return itself
             * @details
             * create new _Actor on attached VirtualCore, function can be chained.\n
             * example:
             * @code
             * auto builder = main.core(0); // get builder of VirtualCore id 0
             * builder.addActor<MyActor>(param1, param2)
             *        .addActor<MyActor>(param1, param2)
             *        // ...
             *        ;
             * @endcode
             * @attention
             * This function is not available when the engine is running.
             */
            template<typename _Actor, typename ..._Args>
            CoreBuilder &addActor(_Args &&...args);

            bool valid() const;
            operator bool() const;

            /*!
             * @brief Get list of created ActorId by the CoreBuilder
             * @return Created ActorId list
             */
            ActorIdList const &idList() const;
        };

        Main() = delete;
        explicit Main(CoreSet const &core_set);
        explicit Main(std::unordered_set<uint8_t> const &core_set);
        ~Main();

        /*!
         * @brief Start the engine
         * @param async has blocking execution
         * @note
         * If async = false the main thread will by used by a VirtualCore engine.
         */
        void start(bool async = true) const;

        static bool hasError();

        /*!
         * @brief Stop the engine
         * @note
         * Same effect as receiving SIGINT Signal.
         */
        static void stop();

        /*!
         * @brief Wait until engine terminates
         * @note
         * You can not avoid calling this function even if main has started with async=false.
         */
        void join() const;

    public:

        /*!
         * @brief Create new _Actor
         * @tparam _Actor DerivedActor type
         * @param index VirtualCore index
         * @param args arguments to forward to the constructor of the _Actor
         * @return ActorId of the created _Actor
         * @details
         * create new _Actor on VirtualCore index.\n
         * example:
         * @code
         * auto id = addActor<MyActor>(0, param1, param2);
         * @endcode
         * @attention
         * This function is not available when the engine is running.
         */
        template<typename _Actor, typename ..._Args>
        ActorId addActor(std::size_t index, _Args &&...args);

        /*!
         * @brief Get CoreBuilder from index
         * @param index VirtualCore index
         * @return CoreBuilder
         * @attention
         * @code
         * auto builder1 = main.core(0);
         * auto builder2 = main.core(0);
         * // even both elements build the same VirtualCore
         * // builder1 != builder2
         * @endcode
         */
        CoreBuilder core(uint16_t const index);

    };

} // namespace qb

#endif //QB_MAIN_H
