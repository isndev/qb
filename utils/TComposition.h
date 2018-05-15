
#include <utility>
#include <initializer_list>
#include <type_traits>
#include <tuple>

#ifndef CUBE_UTILS_TCOMPOSITION_H
#define CUBE_UTILS_TCOMPOSITION_H

template<typename ...Group>
class TComposition {
protected:
    static constexpr std::size_t NB_ITEM = sizeof...(Group);

    std::tuple<Group...> _elements;
public:
    TComposition() = default;
    TComposition(std::tuple<Group...> elements)
            : _elements(elements) {}
    TComposition(Group ...group)
            : _elements{std::forward<Group>(group)...} {}


    template<typename Func>
    using ret_type = decltype(std::declval<Func>()(std::get<0>(_elements)));
    /*
    Call a function for each selected item(s) by index
    */
#if __cplusplus >= 199711L

    /*
    Call a function for each item
    */
    template<typename Lambda>
    constexpr inline auto
    each(Lambda const &func) {
        return each(func, std::make_index_sequence<NB_ITEM>{});
    }

    template<typename Lambda, std::size_t ...Index>
    constexpr inline typename std::enable_if<!std::is_void<ret_type<Lambda>>::value, ret_type<Lambda>>::type
    each(Lambda const &func, std::index_sequence<Index...>) {
        return (func(std::get<Index>(_elements)) && ...);
    }

#else
    /*
    Call a function for each item
    */
    template<typename Lambda>
    constexpr inline void
        each(Lambda const &func) {
        each(func, std::make_index_sequence<NB_ITEM>{});
    }

    template<typename Lambda, std::size_t ...Index>
    constexpr inline typename std::enable_if<!std::is_void<ret_type<Lambda>>::value, ret_type<Lambda>>::type
        each(Lambda const &func, std::index_sequence<Index...>) {
        (void)std::initializer_list<int>{
            func(std::get<Index>(_elements))...
        };
    }

#endif

    /*
    Call a function for each selected item(s) by index
    */
    template<typename Lambda, std::size_t ...Index>
    constexpr inline typename std::enable_if<std::is_void<ret_type<Lambda>>::value, void>::type
    each(Lambda const &func, std::index_sequence<Index...>) {
        (void) std::initializer_list<int>{
                func(std::get<Index>(_elements))...
        };
    }

    /*
    Take all items and call a function with all items as parameters
    */
    template<typename Lambda>
    constexpr inline auto
    take(Lambda const &func) {
        return take(func, std::make_index_sequence<NB_ITEM>{});
    }

    /*
    Take item(s) by index and call a function with item(s) as parameters
    */
    template<typename Lambda, std::size_t ...Index>
    constexpr inline auto
    take(Lambda const &func, std::index_sequence<Index...>) {
        return func(std::get<Index>(_elements)...);
    }

    template<std::size_t _BookIdx>
    constexpr inline auto &get() {
        return std::get<_BookIdx>(_elements);
    }

    template<std::size_t _BookIdx>
    constexpr inline const auto &get() const {
        return std::get<_BookIdx>(_elements);
    }

};

#endif //CUBE_UTILS_TCOMPOSITION_H