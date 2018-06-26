
#ifndef CUBE_TIMEDCORE_H
#define CUBE_TIMEDCORE_H
#include "PhysicalCore.h"

namespace cube {

    template<std::size_t _CoreIndex, typename _ParentHandler, typename _SharedData>
    class TimedCoreHandler
            : public BaseCoreHandler<_CoreIndex,
                    _ParentHandler,
                    TimedCoreHandler<_CoreIndex, _ParentHandler, _SharedData>,
                    _SharedData>
    {
        friend _ParentHandler;

        void updateTimer() {
            const auto now = Timestamp::nano();
            auto best = getBestTime();
            _nano_timer = now - _nano_timer;
            if (reinterpret_cast<uint8_t const *>(&best)[sizeof(_nano_timer) - 1] == _CoreIndex) {
                if (_nano_timer > best) {
                    reinterpret_cast<uint8_t *>(&_nano_timer)[sizeof(_nano_timer) - 1] = _CoreIndex;
                    _ParentHandler::parent_t::sync_start.store(_nano_timer);
                }
            } else if (_nano_timer < best) {
                reinterpret_cast<uint8_t *>(&_nano_timer)[sizeof(_nano_timer) - 1] = _CoreIndex;
                _ParentHandler::parent_t::sync_start.store(_nano_timer);
            }
            _nano_timer = now;
        }

        std::uint64_t _nano_timer;
    public:
        using base_t = BaseCoreHandler<_CoreIndex, _ParentHandler, TimedCoreHandler,_SharedData>;

        TimedCoreHandler() = delete;
        TimedCoreHandler(_ParentHandler *parent)
        : base_t(parent)
        {}


        inline bool onInit() const {
            return true;
        }

        inline void onCallback(){
            updateTimer();
        }

        uint64_t getTime() const {
            return _nano_timer;
        }

        uint64_t getBestTime() const {
            return _ParentHandler::parent_t::sync_start.load();
        }

        uint32_t getBestCore() const {
            const auto best_time = getBestTime();
            return reinterpret_cast<uint8_t const *>(&best_time)[sizeof(_nano_timer) - 1];
        }

    };
}
#endif //RISK_COMPUTATION_TIMEDCORE_H
