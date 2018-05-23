#include <iostream>
#include <thread>
#include "system/timestamp.h"

int main() {
    cube::NanoTimestamp ts;

    std::cout << "Nano Timestamp" << std::endl;
    std::cout << "seconds[" << ts.seconds() << "]" << std::endl;
    std::cout << "milliseconds[" << ts.milliseconds() << "]" << std::endl;
    std::cout << "microseconds[" << ts.microseconds() << "]" << std::endl;
    std::cout << "nanoseconds[" << ts.nanoseconds() << "]" << std::endl;

    cube::RdtsTimestamp rts;
    std::cout << "RTS Timestamp" << std::endl;
    std::cout << "seconds[" << ts.seconds() << "]" << std::endl;
    std::cout << "milliseconds[" << ts.milliseconds() << "]" << std::endl;
    std::cout << "microseconds[" << ts.microseconds() << "]" << std::endl;
    std::cout << "nanoseconds[" << ts.nanoseconds() << "]" << std::endl;


    std::cout << std::thread::hardware_concurrency() << std::endl;

    return 0;
}