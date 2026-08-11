// <qb/io/async/coroutine/task.h> ALONE -- the sharpest case in this directory.
// task<T>::~task calls forget_frame_if_current(), and task<T>::final_suspend calls
// defer_frame_destruction(). Both are DECLARED here and DEFINED `inline` in scheduler.h. A
// non-`inline` first declaration made every TU that reaches task.h without scheduler.h emit
// an undefined reference no archive can satisfy -- and it silenced -Wundefined-inline, which
// is what would have said so, because the entity is not a template either. Destroying a task
// is the most ordinary thing a consumer does.
#include <qb/io/async/coroutine/task.h>

qb::io::async::task<int>
produce() {
    co_return 7;
}

int
main() {
    auto t = produce(); // ~task<int>  -> forget_frame_if_current
    (void) t;
    return 0;
}
