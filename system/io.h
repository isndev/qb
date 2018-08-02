
#ifndef CUBE_TYPES_H
# define CUBE_TYPES_H

# include <iostream>
# include <sstream>
# include <mutex>
# include <utility>
# include <type_traits>

#ifndef NOLOG

# include "../nanolog/nanolog.h"

#else
# define LOG_DEBUG cube::io::log::cout(cube::io::LogLevel::DEBUG)
# define LOG_VERB cube::io::log::cout(cube::io::LogLevel::VERBOSE)
# define LOG_INFO cube::io::log::cout(cube::io::LogLevel::INFO)
# define LOG_WARN cube::io::log::cout(cube::io::LogLevel::WARN)
# define LOG_CRIT cube::io::log::cout(cube::io::LogLevel::CRIT)
#endif

namespace cube {
#ifdef NDEBUG
    constexpr static bool debug = false;
#else
    constexpr static bool debug = true;
#endif

    namespace io {

#ifndef NOLOG
        using LogLevel = nanolog::LogLevel;
        using stream = nanolog::NanoLogLine;

        namespace log {
            void setLevel(io::LogLevel lvl) {
                nanolog::set_log_level(lvl);
            }
        }

#else
        enum class LogLevel : uint8_t {
            DEBUG,
            VERBOSE,
            INFO,
            WARN,
            CRIT
        };

        namespace log {
            extern LogLevel level;
            constexpr void setLevel(io::LogLevel lvl) {
                level = lvl;
            }
        };

#ifdef NOCOUT
        struct stream : public std::basic_ostream<char, std::char_traits<char>> {
        public:
            stream() : std::basic_ostream<char, std::char_traits<char>>(0) {}
        };

        class cout  {
            stream ss;
        public:
            cout() {}
            cout(LogLevel){}
            cout(cout const &) = delete;
            ~cout() {}

            template<typename T>
            inline stream &operator<<(T const &) {
                return ss;
            }
        };

        namespace log {
            using cout = io::cout;
        }

#else
        extern std::mutex io_lock;
        using stream = std::stringstream;

        class cout {
            stream ss;
        public:
            cout() {}
            cout(cout const &) = delete;
            ~cout() {
                std::lock_guard<std::mutex> lock(io_lock);
                std::cout << ss.str() << std::endl << std::flush;
            }


            template<typename T>
            inline stream &operator<<(T const &data)  {
                ss << data;
                return ss;
            }
        };

        namespace log {
            class cout {
                stream ss;
                LogLevel level;
            public:
                cout(io::LogLevel level) : level(level) {}
                cout(cout const &) = delete;
                ~cout() {
                    if (level >= log::level) {
                        std::lock_guard<std::mutex> lock(io_lock);
                        std::cout << ss.str() << std::endl << std::flush;
                    }
                }

                template<typename T>
                inline stream &operator<<(T const &data)  {
                    ss << data;
                    return ss;
                }
            };
        }

#endif //NOCOUT
#endif //NOLOG

    }
}

#endif //CUBE_TYPES_H