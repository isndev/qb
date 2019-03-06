//
// Created by isndev on 12/15/18.
//

#ifndef QB_SESSION_TYPES_H
#define QB_SESSION_TYPES_H

namespace qb {
    namespace session {

        enum class ReturnValue : int {
            KO = 0,
            REPOLL,
            OK
        };

    }
}

#endif //QB_SESSION_TYPES_H
