#include <iostream>
#include <thread>
#include "utils/timestamp.h"

int main() {
    qb::NanoTimestamp ts;

    std::cout << "Nano Timestamp" << std::endl;
    std::cout << "seconds[" << ts.seconds() << "]" << std::endl;
    std::cout << "milliseconds[" << ts.milliseconds() << "]" << std::endl;
    std::cout << "microseconds[" << ts.microseconds() << "]" << std::endl;
    std::cout << "nanoseconds[" << ts.nanoseconds() << "]" << std::endl;

    qb::RdtsTimestamp rts;
    std::cout << "RTS Timestamp" << std::endl;
    std::cout << "seconds[" << ts.seconds() << "]" << std::endl;
    std::cout << "milliseconds[" << ts.milliseconds() << "]" << std::endl;
    std::cout << "microseconds[" << ts.microseconds() << "]" << std::endl;
    std::cout << "nanoseconds[" << ts.nanoseconds() << "]" << std::endl;

    return 0;
}