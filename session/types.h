//
// Created by isndev on 12/15/18.
//

#ifndef CUBE_SESSION_TYPES_H
#define CUBE_SESSION_TYPES_H

namespace cube {
    namespace session {

        enum class ReturnValue : int {
            KO = 0,
            REPOLL,
            OK
        };

    }
}

#endif //CUBE_SESSION_TYPES_H
