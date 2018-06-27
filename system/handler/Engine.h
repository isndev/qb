
#ifndef CUBE_MAIN_H
# define CUBE_MAIN_H
# include "system/handler/BaseHandler.h"

namespace cube {
    namespace handler {

        template<typename ..._Core>
        class Engine : public BaseHandler<typename _Core::template Type<Engine<_Core...>>::type...> {
        public:
            //////// Static Const Data
            static const std::size_t linked_core;
            static const std::size_t total_core;
            static std::atomic<uint64_t> sync_start;

            //////// Types
            using parent_t = Engine;
            using base_t = BaseHandler<typename _Core::template Type<Engine>::type...>;

            /////////////////////////////////////////////////////
            Engine()
                    : base_t((typename _Core::template Type<Engine>::type::parent_ptr_t) (this)...) {
                sync_start.store(0);
                LOG_INFO << "Init Engine with " << total_core << " PhysicalCore(s)";
            }
            /////////////////////////////////////////////////////

            inline bool send(Event const &event) {
                bool ret = false;
                if (unlikely(!this->each_or([&event, &ret](auto &item) -> bool {
                    return item.receive_from_different_core(event, ret);
                }))) {
                    ret = true;
                    LOG_WARN
                    << "Core(" << event.source << ") failed to send event to nonexistent Core(" << event.dest._index
                    << ")";
                }
                return ret;
            }

            // Start Sequence Usage
            template<std::size_t _CoreIndex, typename ..._Init>
            bool setSharedData(_Init &&...init) {
                return this->template __init_shared<_CoreIndex>(std::forward<_Init>(init)...);
            }

            void start() {
                this->__init__shared_data();
                this->__init__actors();
                this->__start();
            }

            void join() {
                this->__join();
            }

        };

        // not constexpr because of windows issue
        template<typename ..._Core>
        const std::size_t handler::Engine<_Core...>::linked_core = 0;
        template<typename ..._Core>
        const std::size_t handler::Engine<_Core...>::total_core = nb_core<Engine, _Core...>();
        template<typename ..._Core>
        std::atomic<uint64_t> handler::Engine<_Core...>::sync_start(0);

    }

    // Aliases for main engine
    template<typename ..._Core>
    using Engine = handler::Engine<_Core...>;
    template<typename ..._Core>
    using Main = handler::Engine<_Core...>;
}

#endif //CUBE_MAIN_H
