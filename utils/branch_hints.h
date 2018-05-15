
#ifndef CUBE_UTILS_BRANCH_HINTS_H
#define CUBE_UTILS_BRANCH_HINTS_H

namespace cube {
    /** \brief hint for the branch prediction */
    inline bool likely(bool expr) {
#ifdef __GNUC__
        return __builtin_expect(expr, true);
#else
        return expr;
#endif
    }

    /** \brief hint for the branch prediction */
    inline bool unlikely(bool expr) {
#ifdef __GNUC__
        return __builtin_expect(expr, false);
#else
        return expr;
#endif
    }

} /* namespace cube */

#endif /* CUBE_UTILS_BRANCH_HINTS_H */
