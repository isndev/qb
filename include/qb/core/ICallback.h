//
// Created by isndev on 12/4/18.
//

#ifndef QB_ICALLBACK_H
#define QB_ICALLBACK_H

namespace qb {

    /*!
     * @interface ICallback core/ICallback.h qb/icallback.h
     * @ingroup Engine
     * @brief Actor callback interface
     * @details
     * DerivedActor must inherit from this interface
     * to register loop callback
     */
    class ICallback {
    public:
        virtual ~ICallback() {}

        virtual void onCallback() = 0;
    };

} // namespace qb

#endif //QB_ICALLBACK_H
