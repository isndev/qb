
#ifndef CUBE_TYPES_H
# define CUBE_TYPES_H
# include <iostream>
# include <sstream>
# include <mutex>
# include <type_traits>

namespace cube {
    struct io {
        static std::mutex io_lock;

#ifndef NDEBUG
        class cout : std::lock_guard<std::mutex> {
            std::stringstream ss;
        public:
            cout() : std::lock_guard<std::mutex>(io_lock) {}
			~cout() { std::cout << ss.str() << std::flush; }

            template<typename T>
            std::ostream &operator<<(T const &data) {
                ss << data;
               return ss;
            }
        };
    };
#else
		class cout  {
			std::stringstream ss;
		public:
			cout() {}
			~cout() {}

			template<typename T>
			std::ostream &operator<<(T const &data) {
				return ss;
			}
		};
	};
#endif // DEBUG
}

#endif //CUBE_TYPES_H
