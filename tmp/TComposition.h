
#include <utility>
#include <initializer_list>
#include <type_traits>
#include <tuple>

#ifndef QB_UTILS_TCOMPOSITION_H
#define QB_UTILS_TCOMPOSITION_H
#if __cplusplus < 201402L
#error "TComposition need min c++14, compile with -std=c++14"
#endif


template<typename ...Group>
class TComposition {
protected:
    static constexpr std::size_t NB_ITEM = sizeof...(Group);

    std::tuple<Group...> _elements;
public:
    TComposition() = default;
    TComposition(std::tuple<Group...> &&elements)
            : _elements(std::forward<std::tuple<Group...>>(elements))
    {}
    TComposition(Group&& ...group)
            : _elements{std::forward<Group>(group)...}
    {}
    template <typename ...Init>
    TComposition(Init&& ...init)
        : _elements{std::forward<Init>(init)...}
    {}

    template<typename Func>
    using ret_type = decltype(std::declval<Func>()(std::get<0>(_elements)));

    /*
        Call a function for each selected item(s) by index
    */
    template<typename Lambda, std::size_t ...Index>
    constexpr inline auto
    each(Lambda const &func, std::index_sequence<Index...>) {
        return std::initializer_list<ret_type<Lambda>>{
                func(std::get<Index>(_elements))...
        };
    }

    /*
        Take item(s) by index and call a function with item(s) as parameters
    */
    template<typename Lambda, std::size_t ...Index>
    constexpr inline auto
    take(Lambda const &func, std::index_sequence<Index...>) {
        return func(std::get<Index>(_elements)...);
    }

    // Getters by index
    template<std::size_t Index>
    constexpr inline auto &
    get() {
        return std::get<Index>(_elements);
    }

    template<std::size_t Index>
    constexpr inline const auto &
    get() const {
        return std::get<Index>(_elements);
    }

#if __cplusplus >= 201703L
    /*
         Call a function for each selected item(s) by index
    */
    template<std::size_t ...Index, typename Lambda>
    constexpr inline auto
    each(Lambda const &func) {
        if constexpr (!sizeof...(Index)) {
            return each(func, std::make_index_sequence<NB_ITEM>{});
        } else {
            return std::initializer_list<ret_type<Lambda>>{
                    func(std::get<Index>(_elements))...
            };
        }
    }

    /*
        Call a function for each selected item(s) by type
    */
    template<typename ...Type, typename Lambda>
    constexpr inline auto
    each_t(Lambda const &func) {
        return std::initializer_list<ret_type<Lambda>>{
                func(std::get<Type>(_elements))...
        };
    }

    /*
        Call a function for each selected item(s) by index until function return False
    */
    template<typename Lambda, std::size_t ...Index>
    constexpr inline auto
    each_and(Lambda const &func, std::index_sequence<Index...>) {
        return (func(std::get<Index>(_elements)) && ...);
    }

    /*
        Call a function for each item until function return False
    */
    template<std::size_t ...Index, typename Lambda>
    constexpr inline auto
    each_and(Lambda const &func) {
        if constexpr (!sizeof...(Index)) {
            return each_and(func, std::make_index_sequence<NB_ITEM>{});
        } else {
            return (func(std::get<Index>(_elements)) && ...);
        }
    }

    /*
        Call a function for each selected item(s) by type until function return False
    */
    template<typename ...Type, typename Lambda>
    constexpr inline auto
    each_and_t(Lambda const &func) {
        return (func(std::get<Type>(_elements)) && ...);
    }

    /*
        Call a function for each selected item(s) by index until function return True
    */
    template<typename Lambda, std::size_t ...Index>
    constexpr inline auto
    each_or(Lambda const &func, std::index_sequence<Index...>) {
        return (func(std::get<Index>(_elements)) || ...);
    }

    /*
        Call a function for each item until function return True
    */
    template<std::size_t ...Index, typename Lambda>
    constexpr inline auto
    each_or(Lambda const &func) {
        if constexpr (!sizeof...(Index)) {
            return each_or(func, std::make_index_sequence<NB_ITEM>{});
        } else {
            return (func(std::get<Index>(_elements)) || ...);
        }
    }

    /*
        Call a function for each selected item(s) by type until function return False
    */
    template<typename ...Type, typename Lambda>
    constexpr inline auto
    each_or_t(Lambda const &func) {
        return (func(std::get<Type>(_elements)) || ...);
    }

    /*
        Take item(s) by index and call a function with item(s) as parameters
    */
    template<std::size_t ...Index, typename Lambda>
    constexpr inline auto
    take(Lambda const &func) {
        if constexpr (!sizeof...(Index)) {
            return take(func, std::make_index_sequence<NB_ITEM>{});
        } else {
            return take(func, std::index_sequence<Index...>());
        }
    }

    /*
        Take item(s) by type and call a function with item(s) as parameters
    */
    template<typename ...Type, typename Lambda>
    constexpr inline auto
    take_t(Lambda const &func) {
        return func(std::get<Type>(_elements)...);
    }

    // Getters by type
    template<typename Type>
    constexpr inline auto &
    get() {
        return std::get<Type>(_elements);
    }

    template<typename Type>
    constexpr inline const auto &
    get() const {
        return std::get<Type>(_elements);
    }
#else
    /*
        Call a function for each item
    */
    template<typename Lambda>
    constexpr inline auto
    each(Lambda const &func) {
        return each(func, std::make_index_sequence<NB_ITEM>{});
    }

    /*
        Take item(s) by index and call a function with item(s) as parameters
    */
    template<typename Lambda>
    constexpr inline auto
    take(Lambda const &func) {
        return take(func, std::make_index_sequence<NB_ITEM>{});
    }

#endif

};

#endif //QB_UTILS_TCOMPOSITION_H
