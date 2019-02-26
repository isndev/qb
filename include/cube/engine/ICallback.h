//
// Created by isndev on 12/4/18.
//

#ifndef CUBE_ICALLBACK_H
#define CUBE_ICALLBACK_H

namespace cube {

    /*!
     * @interface ICallback engine/ICallback.h cube/icallback.h
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

} // namespace cube

#endif //CUBE_ICALLBACK_H
