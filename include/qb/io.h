
#ifndef QB_TYPES_H
# define QB_TYPES_H

# include <iostream>
# include <sstream>
# include <mutex>
# include <utility>
# include <type_traits>
# include <nanolog/nanolog.h>

namespace qb {
#ifdef NDEBUG
    constexpr static bool debug = false;
#else
    constexpr static bool debug = true;
#endif

    namespace io {
        using stream = nanolog::NanoLogLine;

        namespace log {
            using Level = nanolog::LogLevel;
            void setLevel(Level lvl);
            void init(std::string const &file_path, uint32_t const roll_MB = 128);
        }

        class cout {
            static std::mutex io_lock;
            std::stringstream ss;
        public:
            cout() = default;
            cout(cout const &) = delete;
            ~cout();

            template<typename T>
            inline std::stringstream &operator<<(T const &data)  {
                ss << data;
                return ss;
            }
        };
    }
}

#endif //QB_TYPES_H