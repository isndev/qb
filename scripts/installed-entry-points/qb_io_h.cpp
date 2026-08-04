// <qb/io.h> ALONE -- qb-io's own umbrella. It is the logging/vocabulary entry point, so what
// it has to link is the log stream and the qb-io archive behind it, not the coroutine layer.
#include <qb/io.h>

int
main() {
    QB_LOG_INFO("qb/io.h entry point");
    return 0;
}
