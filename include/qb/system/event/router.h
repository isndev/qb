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


#include <qb/system/container/unordered_map.h>
#include <qb/utility/type_traits.h>
#include <qb/utility/branch_hints.h>

#ifndef QB_EVENT_ROUTER_H
#define QB_EVENT_ROUTER_H

namespace qb {
    namespace router {
//        struct EventExample {
//            using id_type = uint16_t;
//            using id_handler_type = uint32_t;
//
//            template<typename T>
//            constexpr id_type type_to_id();
//
//            id_type id;
//            id_handler_type dest;
//            id_handler_type source;
//        };

        namespace internal {

            class EventPolicy {
            public:
                EventPolicy() = default;

                ~EventPolicy() = default;

            protected:
                template<typename _Handler, typename _Event>
                inline void invoke(_Handler &handler, _Event &event) const {
                    if constexpr (has_member_func_is_alive<_Event>::value) {
                        if (handler.is_alive())
                            handler.on(event);
                    } else
                        handler.on(event);
                }

                template<typename _Event>
                inline void dispose(_Event &event) const noexcept {
                    if constexpr (!std::is_trivially_destructible_v<_Event>) {
                        if constexpr (has_member_func_is_alive<_Event>::value) {
                            if (!event.is_alive())
                                event.~_Event();
                        } else
                            event.~_Event();
                    }
                }
            };

        }

        template<typename _RawEvent, typename _Handler>
        class sesh : public internal::EventPolicy {
            using _EventId = typename _RawEvent::id_type;
            using _HandlerId = typename _RawEvent::id_handler_type;

            _Handler &_handler;
        public:

            sesh() = delete;

            explicit sesh(_Handler &handler) noexcept : _handler(handler) {}

            template<bool _CleanEvent = true>
            void route(_RawEvent &event) {
                invoke(_handler, event);
                if constexpr (_CleanEvent)
                    dispose(event);
            }
        };

        template<typename _RawEvent, typename _Handler = void>
        class semh : public internal::EventPolicy {
            using _EventId = typename _RawEvent::id_type;
            using _HandlerId = typename _RawEvent::id_handler_type;

            qb::unordered_map<_HandlerId, _Handler &> _subscribed_handlers;
        public:
            semh() = default;

            ~semh() = default;

            template<bool _CleanEvent = true>
            void route(_RawEvent &event) noexcept {
                if constexpr (has_member_func_is_broadcast<_HandlerId>::value) {
                    if (event.getDestination().is_broadcast()) {
                        for (auto &it : _subscribed_handlers)
                            invoke(it.second, event);

                        if constexpr (_CleanEvent)
                            dispose(event);

                        return;
                    }
                }

                const auto &it = _subscribed_handlers.find(event.dest);
                if (likely(it != _subscribed_handlers.cend()))
                    invoke(it->second, event);

                if constexpr (_CleanEvent)
                    dispose(event);
            }

            void subscribe(_Handler &handler) noexcept {
                _subscribed_handlers.erase(handler.id());
                _subscribed_handlers.insert({handler.id(), handler});
            }

            void unsubscribe(_HandlerId const &id) noexcept {
                _subscribed_handlers.erase(id);
            }

        };

        template<typename _RawEvent>
        class semh<_RawEvent, void> : public internal::EventPolicy {
            using _EventId = typename _RawEvent::id_type;
            using _HandlerId = typename _RawEvent::id_handler_type;

            class IHandlerResolver {
            public:
                virtual ~IHandlerResolver() noexcept {}

                virtual void resolve(_RawEvent &event) = 0;
            };

            template<typename _Handler>
            class HandlerResolver
                    : public IHandlerResolver
				    , public sesh<_RawEvent, _Handler> {
            public:
                HandlerResolver() = delete;

                HandlerResolver(_Handler &handler) noexcept : sesh<_RawEvent, _Handler>(handler) {}

                void resolve(_RawEvent &event) final {
                    sesh<_RawEvent, _Handler>::template route<false>(event);
                }
            };

            qb::unordered_map<_HandlerId, IHandlerResolver &> _subscribed_handlers;

        public:

            semh() = default;

            ~semh() noexcept {
                for (const auto &it : _subscribed_handlers)
                    delete &it.second;
            }

            template<bool _CleanEvent = false>
            void route(_RawEvent &event) const noexcept {
                if constexpr (has_member_func_is_broadcast<_HandlerId>::value) {
                    if (event.getDestination().is_broadcast()) {
                        for (const auto &it : _subscribed_handlers)
                            it.second.resolve(event);

                        if constexpr (_CleanEvent)
                            dispose(event);

                        return;
                    }
                }

                const auto &it = _subscribed_handlers.find(event.getDestination());
                if (likely(it != _subscribed_handlers.cend()))
                    it->second.resolve(event);

                if constexpr (_CleanEvent)
                    dispose(event);
            }

            template<typename _Handler>
            void subscribe(_Handler &handler) noexcept {
                const auto &it = _subscribed_handlers.find(handler.id());
                if (it != _subscribed_handlers.cend())
                    delete &it->second;
                _subscribed_handlers.insert({handler.id(), *new HandlerResolver<_Handler>(handler)});
            }

            void unsubscribe(_HandlerId const &id) noexcept {
                const auto &it = _subscribed_handlers.find(id);
                if (it != _subscribed_handlers.end()) {
                    delete &it->second;
                    _subscribed_handlers.erase(it);
                }
            }
        };

        template<typename _RawEvent, typename _Handler, bool _CleanEvent = true>
        class mesh {
        public:
            using _EventId = typename _RawEvent::id_type;
            using _HandlerId = typename _RawEvent::id_handler_type;
        private:

            class IEventResolver {
            public:
                virtual ~IEventResolver() noexcept {}

                virtual void resolve(_Handler &handler, _RawEvent &event) const = 0;
            };

            template<typename _Event>
            class EventResolver
                    : public IEventResolver, public internal::EventPolicy {
            public:
                EventResolver() = default;

                void resolve(_Handler &handler, _RawEvent &event) const final {
                    auto &revent = reinterpret_cast<_Event &>(event);
                    invoke(handler, revent);
                    if constexpr (_CleanEvent)
                        dispose(revent);
                }
            };

            _Handler &_handler;
            qb::unordered_map<_EventId, IEventResolver &> _registered_events;

        public:

            mesh() = delete;

            mesh(_Handler &handler) noexcept : _handler(handler) {}

            ~mesh() noexcept {
                unsubscribe();
            }

            void route(_RawEvent &event) {
                // /!\ Why do not not protecting this ?
                // because a dynamic event pushed and not registred
                // has 100% risk of leaking memory
                // Todo : may be should add a define to prevent user from this
                // const auto &it = _registered_events.find(event.getID());
                // if (likely(it != _registered_events.cend()))
                //    it->second.resolve(event);
                _registered_events.at(event.getID()).resolve(_handler, event);
            }

            template<typename _Event>
            void subscribe() {
                const auto &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
                if (it == _registered_events.cend()) {
                    _registered_events.insert({_RawEvent::template type_to_id<_Event>(), *new EventResolver<_Event>});
                }
            }

            template<typename _Event>
            void unsubscribe() {
                const auto &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
                if (it != _registered_events.cend()) {
                    delete &it->second;
                    _registered_events.erase(it);
                }
            }

            void unsubscribe() {
                for (const auto &it : _registered_events)
                    delete &it.second;
                _registered_events.clear();
            }
        };

        template<typename _RawEvent, bool _CleanEvent = true, typename _Handler = void>
        class memh {
        public:
            using _EventId = typename _RawEvent::id_type;
            using _HandlerId = typename _RawEvent::id_handler_type;
        private:

            class IEventResolver {
            public:
                virtual ~IEventResolver() noexcept {}

                virtual void resolve(_RawEvent &event) = 0;

                virtual void unsubscribe(_HandlerId const &id) = 0;
            };

            template<typename _Event>
            class EventResolver
                    : public IEventResolver, public semh<_Event, _Handler> {
				using _HandlerId = typename _RawEvent::id_handler_type;
            public:
                EventResolver() noexcept : semh<_Event, _Handler>() {}

                void resolve(_RawEvent &event) final {
                    auto &revent = reinterpret_cast<_Event &>(event);
                    semh<_Event, _Handler>::template route<_CleanEvent>(revent);
                }

                void unsubscribe(_HandlerId const &id) final {
                    semh<_Event, _Handler>::unsubscribe(id);
                }
            };

            qb::unordered_map<_EventId, IEventResolver &> _registered_events;

        public:

            memh() = default;

            ~memh() noexcept {
                for (const auto &it : _registered_events)
                    delete &it.second;
            }

            void route(_RawEvent &event) const {
                // /!\ Look notice in of mesh router above
                _registered_events.at(event.getID()).resolve(event);
            }

            template<typename _Event>
            void subscribe(_Handler &handler) {
                const auto &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
                if (it == _registered_events.cend()) {
                    auto &resolver = *new EventResolver<_Event>;
                    resolver.subscribe(handler);
                    _registered_events.insert({_RawEvent::template type_to_id<_Event>(), resolver});
                } else {
                    dynamic_cast<EventResolver<_Event> &>(it->second).subscribe(handler);
                }
            }

            template<typename _Event>
            void unsubscribe(_Handler &handler) const {
                auto const &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
                if (it != _registered_events.cend())
                    it->second.unsubscribe(handler.id());
            }

            void unsubscribe(_Handler const &handler) const {
                unsubscribe(handler.id());
            }

            void unsubscribe(_HandlerId const &id) const {
                for (auto const &it : _registered_events) {
                    it.second.unsubscribe(id);
                }
            }
        };

        template<typename _RawEvent, bool _CleanEvent>
        class memh<_RawEvent, _CleanEvent, void> {
        public:
            using _EventId = typename _RawEvent::id_type;
            using _HandlerId = typename _RawEvent::id_handler_type;
        private:

            class IEventResolver {
            public:
                virtual ~IEventResolver() noexcept {}

                virtual void resolve(_RawEvent &event) const = 0;

                virtual void unsubscribe(_HandlerId const &id) = 0;
            };

            template<typename _Event>
            class EventResolver
                    : public IEventResolver, public semh<_Event> {
            public:
                EventResolver() noexcept : semh<_Event>() {}

                void resolve(_RawEvent &event) const final {
                    auto &revent = reinterpret_cast<_Event &>(event);
                    semh<_Event>::template route<_CleanEvent>(revent);
                }

                void unsubscribe(typename _RawEvent::id_handler_type const &id) final {
                    semh<_Event>::unsubscribe(id);
                }
            };

            qb::unordered_map<_EventId, IEventResolver &> _registered_events;

        public:

            memh() = default;

            ~memh() noexcept {
                for (const auto &it : _registered_events)
                    delete &it.second;
            }

            void route(_RawEvent &event) const {
                // /!\ Look notice in of mesh router above
                _registered_events.at(event.getID()).resolve(event);
            }

            template<typename _Event, typename _Handler>
            void subscribe(_Handler &handler) {
                const auto &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
                if (it == _registered_events.cend()) {
                    auto &resolver = *new EventResolver<_Event>;
                    resolver.subscribe(handler);
                    _registered_events.insert({_RawEvent::template type_to_id<_Event>(), resolver});
                } else {
                    dynamic_cast<EventResolver<_Event> &>(it->second).subscribe(handler);
                }
            }

            template<typename _Event, typename _Handler>
            void unsubscribe(_Handler const &handler) const {
                auto const &it = _registered_events.find(_RawEvent::template type_to_id<_Event>());
                if (it != _registered_events.cend())
                    it->second.unsubscribe(handler.id());
            }

            template<typename _Handler>
            void unsubscribe(_Handler const &handler) const {
                unsubscribe(handler.id());
            }

            void unsubscribe(_HandlerId const &id) const {
                for (auto const &it : _registered_events) {
                    it.second.unsubscribe(id);
                }
            }

        };

    } // namespace router
} // namespace qb

#endif //QB_EVENT_ROUTER_H
