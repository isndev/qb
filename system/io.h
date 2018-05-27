
#ifndef CUBE_TYPES_H
# define CUBE_TYPES_H

# include <iostream>
# include <sstream>
# include <mutex>
# include <utility>
# include <type_traits>

#ifndef NOLOG

# include "nanolog.h"

#else
# define LOG_DEBUG cube::io::cout()
# define LOG_INFO cube::io::cout()
# define LOG_WARN cube::io::cout()
# define LOG_CRIT cube::io::cout()
#endif

namespace cube {
#ifdef NODEBUG
    constexpr static bool debug = false;
#else
    constexpr static bool debug = true;
#endif

    struct io {

#ifndef NOLOG
        using stream = nanolog::NanoLogLine;
#else
#ifdef NOCOUT
        struct stream : public std::basic_ostream<char, std::char_traits<char>> {
            public:
            stream() : std::basic_ostream<char, std::char_traits<char>>(0) {}
        };

        class cout  {
            stream ss;
            public:
            cout() {}
            cout(cout const &) = delete;
            ~cout() {}

            template<typename T>
            inline stream &operator<<(T const &) {
                return ss;
            }
        };

#else

        static std::mutex io_lock;
        using stream = std::stringstream;

        class cout : std::lock_guard<std::mutex> {
            stream ss;
            public:
            cout() : std::lock_guard<std::mutex>(io_lock) {}
            cout(cout const &) = delete;
            ~cout() {
                std::cout << ss.str() << std::endl << std::flush;
            }


            template<typename T>
            inline stream &operator<<(T const &data)  {
                ss << data;
                return ss;
            }
        };

#endif //NOCOUT
#endif //NOLOG

    };
}

#endif //CUBE_TYPES_H