
#ifndef CUBE_TIMEDCORE_H
#define CUBE_TIMEDCORE_H
#include "CoreBase.h"

namespace cube {
    namespace handler {

        template<std::size_t _CoreIndex, typename _ParentHandler, typename _SharedData>
        class TimedCoreHandler
                : public BaseCoreHandler<_CoreIndex,
                        _ParentHandler,
                        TimedCoreHandler<_CoreIndex, _ParentHandler, _SharedData>,
                        _SharedData> {
            friend _ParentHandler;

            void updateTime() {
                const auto now = Timestamp::nano();
                auto best = this->bestTime();
                _nano_timer = now - _nano_timer;
                if (reinterpret_cast<uint8_t const *>(&best)[0] == _CoreIndex) {
                    if (_nano_timer > best) {
                        reinterpret_cast<uint8_t *>(&_nano_timer)[0] = _CoreIndex;
                        _ParentHandler::parent_t::sync_start.store(_nano_timer);
                    }
                } else if (_nano_timer < best) {
                    reinterpret_cast<uint8_t *>(&_nano_timer)[0] = _CoreIndex;
                    _ParentHandler::parent_t::sync_start.store(_nano_timer);
                    //LOG_WARN << "SWITCH TO " << _CoreIndex << " TIME:" << _nano_timer << "best:" << best << "now:" << now;
                }
                _nano_timer = now;
            }

            uint64_t _nano_timer;
        public:
            using base_t = BaseCoreHandler<_CoreIndex, _ParentHandler, TimedCoreHandler, _SharedData>;

            TimedCoreHandler() = delete;

            TimedCoreHandler(_ParentHandler *parent)
                    : base_t(parent), _nano_timer() {}


            inline bool onInit() const {
                return true;
            }

            inline void onCallback() {
                updateTime();
            }

            uint64_t time() const {
                return _nano_timer;
            }

        };

    }

    // Start sequence element
    template<std::size_t _CoreIndex, typename _SharedData = void>
    struct TimedCore {
        template<typename _Parent>
        struct Type {
            typedef cube::handler::TimedCoreHandler<_CoreIndex,_Parent, _SharedData> type;
        };
    };

}
#endif //RISK_COMPUTATION_TIMEDCORE_H
