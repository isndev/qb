//
// Created by isndev on 12/4/18.
//

#ifndef CUBE_ICALLBACK_H
#define CUBE_ICALLBACK_H

namespace cube {

    class ICallback {
    public:
        virtual ~ICallback() {}

        virtual void onCallback() = 0;
    };

} // namespace cube

#endif //CUBE_ICALLBACK_H
