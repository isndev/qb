#include <string>
#include <sstream>
#include <iostream>
#include <chrono>
#include <queue>

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

template<typename T, typename F>
auto time(F f) {
    auto start = std::chrono::steady_clock::now();

    auto result = f();

    auto end = std::chrono::steady_clock::now();
    auto diff = end - start;
    return std::make_pair(std::chrono::duration<double, T>(diff).count(), result);
}

template<typename F>
auto test(const std::string &name, F f, std::ostream &os = std::cout) {
    os << "Running test '" << name << "' \t";
    os.flush();

    try {
        auto result = time<std::micro>(f);
        auto duration = result.first;

        os << "[" << duration << " us] ";
        os << "-> Success" << std::endl;
        return result.second;
    }
    catch (const std::exception &e) {
        os << "-> Failed !" << std::endl;
        os << "\t => " << e.what() << std::endl;
        return decltype(f()){};
    }
}

template<std::size_t REPEAT, typename F>
void test(const std::string &name, F f, std::ostream &os = std::cout) {
    os << "Running test '" << name << "' \t";
    os.flush();

    std::size_t i = 1;
    double min = (std::numeric_limits<double>::max)();
    double max = (std::numeric_limits<double>::min)();
    double avg = 0;

    try {
        while (i <= REPEAT) {
            auto duration = time<std::micro>(f).first;
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