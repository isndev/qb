
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

    };
}
#endif //RISK_COMPUTATION_TIMEDCORE_H
