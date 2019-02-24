#include <string>
#include <sstream>
#include <iostream>
#include <chrono>
#include <queue>
#include <functional>

#ifndef ASSERT_H_
# define ASSERT_H_

template<typename T1, typename T2>
void assertEquals(T1 t1, T2 t2) {
    if (!(t1 == t2)) {
        std::stringstream ss;
        ss << "Assertion failed: ";
        ss << "Expected:" << t1;
        ss << ", Got:" << t2;
        throw std::runtime_error(ss.str());
    }
}


template <typename T>
class Timer {

    std::chrono::time_point<std::chrono::steady_clock, std::chrono::nanoseconds> start;
public:

    template<typename F>
    auto time(F f) {
        start = std::chrono::steady_clock::now();

        auto result = f(*this);

        auto end = std::chrono::steady_clock::now();
        auto diff = end - start;
        return std::make_pair(std::chrono::duration<double, T>(diff).count(), result);
    }

    void reset() {
        start = std::chrono::steady_clock::now();
    }

};



template<typename F>
auto test(const std::string &name, F f, std::ostream &os = std::cout) {
    os << "Running test '" << name << "' \t";
    os.flush();

    try {
        auto result = Timer<std::micro>().time(f);
        auto duration = result.first;

        os << "[" << duration << " us] ";
        os << "-> Success" << std::endl;
        return result.second;
    }
    catch (const std::exception &e) {
        os << "-> Failed !" << std::endl;
        os << "\t => " << e.what() << std::endl;
        return decltype(f(*new Timer<std::micro>())){};
    }
}

template<std::size_t REPEAT, typename F>
void test(const std::string &name, F f, std::ostream &os = std::cout) {
    os << "Running test '" << name << "' \t";
    os.flush();

    std::size_t i = 0;
    double min = (std::numeric_limits<double>::max)();
    double max = (std::numeric_limits<double>::min)();
    double avg = 0;

    try {
        while (i < REPEAT) {
            auto duration = Timer<std::micro>().time(f).first;
            min = (std::min)(min, duration);
            max = (std::max)(max, duration);
            avg += duration;
            ++i;
        }
        os << "\n\tMin[" << min << " us] ";
        os << "\n\tAvg[" << avg / i << " us] ";
        os << "\n\tMax[" << max << " us] ";
        os << "-> Success" << std::endl;
    }
    catch (const std::exception &e) {
        os << "-> Failed !" << std::endl;
        os << "\t => " << e.what() << std::endl;
    }
}

#endif // !ASSERT_H_