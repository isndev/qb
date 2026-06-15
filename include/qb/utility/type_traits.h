/**
 * @file qb/utility/type_traits.h
 * @brief Advanced type traits and metaprogramming utilities for the QB Framework.
 *
 * This file extends the standard library's `<type_traits>` with additional
 * type traits and metaprogramming utilities. These are used for compile-time
 * introspection and template metaprogramming, enabling features such as:
 * - Detection of container properties (e.g., `is_container`, `is_sequence_container`).
 * - Iterator category detection and value type extraction (`is_map_iterator`, `iterator_type`).
 * - CRTP (Curiously Recurring Template Pattern) base helper (`qb::crtp`).
 * - C++23 Concepts for detecting member types and functions (e.g., `has_is_alive`, `has_disconnect`).
 * - Helper aliases for `std::move` and `std::forward` (`qb::mv`, `qb::fwd`).
 * - Utilities for variadic template expansion (`qb::indexes_tuple`, `qb::expand`).
 *
 * These utilities are primarily for internal framework use but can also be leveraged
 * by application code for advanced template programming.
 * @ingroup Utility
 */

#ifndef QB_TYPE_TRAITS_H
#define QB_TYPE_TRAITS_H
#include <concepts>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <valarray>

namespace qb {

/**
 * @brief Alias for `std::move` with a concise syntax.
 * @ingroup MiscUtils
 * @tparam T Type of the value to move.
 * @param t Value to move.
 * @return Value cast to an rvalue reference.
 */
template <typename T>
[[nodiscard]] constexpr std::remove_reference_t<T> &&
mv(T &&t) noexcept {
    return static_cast<std::remove_reference_t<T> &&>(t);
}

/**
 * @brief Alias for `std::forward` with a concise syntax (lvalue overload).
 * @ingroup MiscUtils
 * @tparam T Type to forward.
 * @param t Lvalue reference to forward.
 * @return Forwarded reference, preserving value category.
 */
template <typename T>
[[nodiscard]] constexpr T &&
fwd(std::remove_reference_t<T> &t) noexcept {
    return std::forward<T>(t);
}

/**
 * @brief Alias for `std::forward` with a concise syntax (rvalue overload).
 * @ingroup MiscUtils
 * @tparam T Type to forward.
 * @param t Rvalue reference to forward.
 * @return Forwarded reference, preserving value category.
 */
template <typename T>
[[nodiscard]] constexpr T &&
fwd(std::remove_reference_t<T> &&t) noexcept {
    return std::forward<T>(t);
}

} // namespace qb

// Backward-compatible global aliases
using qb::mv;
using qb::fwd;

namespace qb {

/**
 * @brief Concept for container-like types with begin/end iteration
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
concept container = requires(T t) {
    typename T::const_iterator;
    { t.begin() } -> std::convertible_to<typename T::const_iterator>;
    { t.end() }   -> std::convertible_to<typename T::const_iterator>;
};

/**
 * @brief Concept for sequence containers (vector, list, deque, etc.)
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
concept sequence_container = container<T> && requires(T t) {
    { t.push_back(std::declval<typename T::value_type>()) } -> std::same_as<void>;
};

/**
 * @brief Concept for map-like containers with key_type and mapped_type
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
concept associative_container = container<T> && requires(T t) {
    typename T::key_type;
    typename T::mapped_type;
    { t[std::declval<const typename T::key_type &>()] } -> std::convertible_to<typename T::mapped_type &>;
};

/**
 * @brief Concept for iterator types that have value_type
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
concept has_iterator_traits = requires {
    typename std::iterator_traits<T>::value_type;
    typename std::iterator_traits<T>::reference;
    typename std::iterator_traits<T>::pointer;
    typename std::iterator_traits<T>::difference_type;
    typename std::iterator_traits<T>::iterator_category;
};

/**
 * @brief Concept for types that can be called with operator()
 * @ingroup TypeTraits
 * @tparam F Callable type
 * @tparam Args Argument types
 */
template <typename F, typename... Args>
concept callable = requires(F f, Args... args) {
    { f(std::forward<Args>(args)...) };
};

/**
 * @brief Concept for types with a clone() method
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
concept clonable = requires(const T t) {
    { t.clone() } -> std::convertible_to<T *>;
};

/**
 * @brief Concept for types with value_type typedef
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
concept has_value_type = requires {
    typename T::value_type;
};

/**
 * @brief Concept for types with size() method
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
concept has_size_method = requires(T t) {
    { t.size() } -> std::convertible_to<std::size_t>;
};


/**
 * @struct crtp
 * @ingroup TypeTraits
 * @brief Base class for implementing the Curiously Recurring Template Pattern (CRTP).
 *
 * This class provides helper methods `impl()` to safely cast the base class pointer/reference
 * to the derived class type `T`. This is the core mechanism of CRTP, enabling static polymorphism
 * and code reuse by allowing base classes to access members of their derived classes.
 *
 * C++23: Uses explicit object parameter (deducing this) to handle const/non-const in one method.
 *
 * @tparam T The derived class type that inherits from `crtp<T>`.
 */
template <typename T>
struct crtp {
    /**
     * @brief Access the derived class instance
     * C++23: Single method with deducing-this handles both const and non-const.
     * Preserves const-ness: const crtp<T> → const T&, non-const → T&.
     *
     * @return Reference to the derived class
     */
    [[nodiscard]] inline auto &&impl(this auto &&self) noexcept {
        using Self = std::remove_reference_t<decltype(self)>;
        if constexpr (std::is_const_v<Self>)
            return static_cast<const T &>(self);
        else
            return static_cast<T &>(self);
    }
};

/**
 * @brief Type trait to check if a type `T` is a container.
 * @ingroup TypeTraits
 * @details C++23: Uses the container concept instead of complex SFINAE.
 *          A type `T` is considered a container if it satisfies qb::container concept.
 *          Specializations exist for C-style arrays, `std::valarray`, `std::pair`, and `std::tuple`.
 * @tparam T The type to check.
 */
template <typename T>
struct is_container : std::bool_constant<container<T>> {};

/**
 * @brief Specialization for array types, which are containers
 *
 * @tparam T Element type
 * @tparam N Array size
 */
template <typename T, std::size_t N>
struct is_container<T[N]> : std::true_type {};

/**
 * @brief Specialization for character arrays, which are not considered containers
 *
 * @tparam N Array size
 */
template <std::size_t N>
struct is_container<char[N]> : std::false_type {};

/**
 * @brief Specialization for std::valarray, which is a container
 *
 * @tparam T Element type
 */
template <typename T>
struct is_container<std::valarray<T>> : std::true_type {};

/**
 * @brief Specialization for std::pair, which is a container
 *
 * @tparam T1 First element type
 * @tparam T2 Second element type
 */
template <typename T1, typename T2>
struct is_container<std::pair<T1, T2>> : std::true_type {};

/**
 * @brief Specialization for std::tuple, which is a container
 *
 * @tparam Args Element types
 */
template <typename... Args>
struct is_container<std::tuple<Args...>> : std::true_type {};

/**
 * @struct remove_reference_if
 * @ingroup TypeTraits
 * @brief Conditionally removes a reference from type `T` if `cond` is true.
 * @tparam T The type to process.
 * @tparam cond A boolean condition. If true, `std::remove_reference_t<T>` is used.
 * @return `type` is `T` if `cond` is false, or `std::remove_reference_t<T>` if `cond` is true.
 *         `value` indicates if the reference was actually removed.
 */
template <typename T, bool cond>
struct remove_reference_if {
    /** @brief Resulting type (unchanged if condition is false) - C++23: using alias */
    using type = T;
    /** @brief Whether reference was removed */
    constexpr static bool value = false;
};

/**
 * @brief Specialization that actually removes the reference
 *
 * @tparam T Type to process
 */
template <typename T>
struct remove_reference_if<T, true> {
    /** @brief Resulting type with reference removed - C++23: using _t alias */
    using type = std::remove_reference_t<T>;
    /** @brief Whether reference was removed */
    constexpr static bool value = true;
};

/**
 * @struct is_mappish
 * @ingroup TypeTraits
 * @brief Type trait to check if a type `T` is map-like.
 * @details C++23: Uses associative_container concept.
 *          A type is considered map-like if it satisfies qb::associative_container.
 * @tparam T The type to check.
 * @return `std::true_type` if `T` is map-like, `std::false_type` otherwise.
 */
template <typename T>
struct is_mappish : std::bool_constant<associative_container<T>> {};

/**
 * @struct is_pair
 * @ingroup TypeTraits
 * @brief Type trait to check if a type is a `std::pair`.
 * @tparam Args Deduced template parameters of the type being checked.
 * @return `std::true_type` if the type is `std::pair<T,U>`, `std::false_type` otherwise.
 */
template <typename...>
struct is_pair : std::false_type {};

/**
 * @brief Specialization for std::pair, which returns true
 *
 * @tparam T First element type
 * @tparam U Second element type
 */
template <typename T, typename U>
struct is_pair<std::pair<T, U>> : std::true_type {};

/**
 * @typedef Void
 * @ingroup TypeTraits
 * @brief Helper alias to `void` for SFINAE purposes.
 * @tparam Args Ignored template parameters.
 * @details Used in `std::enable_if` and other SFINAE contexts to create a dependent void type.
 */
template <typename...>
using Void = void;

/**
 * @struct is_inserter
 * @ingroup TypeTraits
 * @brief Type trait to check if a type `T` is an inserter iterator (e.g., `std::back_inserter`).
 * @details Checks for the presence of a nested `container_type`.
 * @tparam T The type to check.
 * @tparam U SFINAE helper.
 * @return `std::true_type` if `T` is an inserter, `std::false_type` otherwise.
 */
template <typename T, typename U = Void<>>
struct is_inserter : std::false_type {};

/**
 * @brief Specialization for types that have a container_type
 *
 * @tparam T Type to check
 */
template <typename T>
struct is_inserter<
    // C++23: Use _v suffix and _t alias
    T, std::enable_if_t<!std::is_void_v<typename T::container_type>>>
    : std::true_type {};

/**
 * @brief Helper variable template for is_inserter
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
inline constexpr bool is_inserter_v = is_inserter<T>::value;

/**
 * @struct iterator_type
 * @ingroup TypeTraits
 * @brief Type trait to extract the `value_type` from an iterator `Iter`.
 * @details Uses `std::iterator_traits<Iter>::value_type` by default.
 *          Specialized for inserter iterators to use `Iter::container_type::value_type`.
 * @tparam Iter The iterator type.
 * @tparam T SFINAE helper.
 * @return `type` is the deduced value type of the iterator.
 */
template <typename Iter, typename T = Void<>>
struct iterator_type {
    /** @brief The value type of the iterator */
    using type = typename std::iterator_traits<Iter>::value_type;
};

/**
 * @brief Specialization for inserter iterators
 *
 * For inserter iterators, the value type comes from the container.
 *
 * @tparam Iter Iterator type
 */
template <typename Iter>
// C++23: Use _t alias and _v suffix
struct iterator_type<Iter, std::enable_if_t<is_inserter_v<Iter>>> {
    /** @brief The value type of the underlying container - C++23: using _t alias */
    using type = std::decay_t<typename Iter::container_type::value_type>;
};

/**
 * @struct is_terator
 * @ingroup TypeTraits
 * @brief Type trait to check if a type `Iter` is an iterator.
 * @details Checks for inserters or types with a valid `std::iterator_traits<Iter>::value_type`,
 *          excluding types convertible to `std::string_view` to avoid misclassifying strings.
 * @tparam Iter The type to check.
 * @tparam T SFINAE helper.
 * @return `std::true_type` if `Iter` is considered an iterator, `std::false_type` otherwise.
 */
template <typename Iter, typename T = Void<>>
struct is_terator : std::false_type {};

/**
 * @brief Specialization for inserter iterators
 *
 * @tparam Iter Iterator type
 */
template <typename Iter>
// C++23: Use _t alias and _v suffix
struct is_terator<Iter, std::enable_if_t<is_inserter_v<Iter>>>
    : std::true_type {};

/**
 * @brief Specialization for standard iterators
 *
 * Excludes types that can be converted to string_view.
 *
 * @tparam Iter Iterator type
 */
template <typename Iter>
// C++23: Use _t alias
struct is_terator<Iter,
                  std::enable_if_t<!std::is_void_v<
                      typename std::iterator_traits<Iter>::value_type>>>
    // C++23: Use _v suffix
    : std::integral_constant<bool, !std::is_convertible_v<Iter, std::string_view>> {
};

/**
 * @struct is_map_iterator
 * @ingroup TypeTraits
 * @brief Type trait to check if an iterator `T` points to map elements (i.e., `std::pair`).
 * @tparam T The iterator type to check.
 * @return `std::true_type` if `iterator_type<T>::type` is a `std::pair`, `std::false_type` otherwise.
 */
template <typename T>
struct is_map_iterator : is_pair<typename iterator_type<T>::type> {};

/**
 * @brief Helper variable template for is_map_iterator
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
inline constexpr bool is_map_iterator_v = is_map_iterator<T>::value;

/**
 * @brief Helper variable template for is_terator
 * @ingroup TypeTraits
 * @tparam Iter Type to check
 */
template <typename Iter>
inline constexpr bool is_terator_v = is_terator<Iter>::value;

/**
 * @brief Corrected alias for is_terator (preserved for backward compatibility)
 * @ingroup TypeTraits
 * @tparam Iter Type to check
 */
template <typename Iter, typename T = Void<>>
using is_iterator = is_terator<Iter, T>;

template <typename Iter>
inline constexpr bool is_iterator_v = is_terator_v<Iter>;

/**
 * @brief Helper type alias for iterator_type
 * @ingroup TypeTraits
 * @tparam Iter Iterator type
 */
template <typename Iter>
using iterator_type_t = typename iterator_type<Iter>::type;

/**
 * @struct has_push_back
 * @ingroup TypeTraits
 * @brief Type trait to check if a type `T` has a `push_back(T::value_type)` member function.
 * @tparam T The type to check.
 * @return `std::true_type` if `T` has `push_back`, `std::false_type` otherwise.
 */
template <typename T, typename = Void<>>
struct has_push_back : std::false_type {};

/**
 * @brief Specialization for types with a valid push_back method
 *
 * @tparam T Type to check
 */
template <typename T>
struct has_push_back<
    // C++23: Use _t alias
    T, std::enable_if_t<std::is_void_v<decltype(std::declval<T>().push_back(
           std::declval<typename T::value_type>()))>>> : std::true_type {};

/**
 * @struct has_insert
 * @ingroup TypeTraits
 * @brief Type trait to check if a type `T` has an `insert(T::const_iterator, T::value_type)` member function.
 * @tparam T The type to check.
 * @return `std::true_type` if `T` has such an `insert` method, `std::false_type` otherwise.
 */
template <typename T, typename = Void<>>
struct has_insert : std::false_type {};

/**
 * @brief Specialization for types with a valid insert method
 *
 * @tparam T Type to check
 */
template <typename T>
struct has_insert<
    // C++23: Use _t alias
    T, std::enable_if_t<std::is_same_v<
           decltype(std::declval<T>().insert(std::declval<typename T::const_iterator>(),
                                             std::declval<typename T::value_type>())),
           typename T::iterator>>> : std::true_type {};

/**
 * @brief Helper variable template for has_push_back
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
inline constexpr bool has_push_back_v = has_push_back<T>::value;

/**
 * @brief Helper variable template for has_insert
 * @ingroup TypeTraits
 * @tparam T Type to check
 */
template <typename T>
inline constexpr bool has_insert_v = has_insert<T>::value;

/**
 * @struct is_sequence_container
 * @ingroup TypeTraits
 * @brief Type trait to check if type `T` is a sequence container.
 * @details Considered true if `T` has `push_back` and is not `std::string`.
 * @tparam T The type to check.
 */
template <typename T>
struct is_sequence_container
    : std::integral_constant<
          // C++23: Use _v suffix and _t alias
          bool, has_push_back_v<T> &&
                    !std::is_same_v<std::decay_t<T>, std::string>> {};

/**
 * @struct is_associative_container
 * @ingroup TypeTraits
 * @brief Type trait to check if type `T` is an associative container.
 * @details Considered true if `T` has `insert` but not `push_back`.
 * @tparam T The type to check.
 */
template <typename T>
struct is_associative_container
    // C++23: Use _v suffix
    : std::integral_constant<bool, has_insert_v<T> && !has_push_back_v<T>> {};

/**
 * @struct nth_type
 * @ingroup TypeTraits
 * @brief Metafunction for selecting the Nth type in a variadic template parameter pack.
 * @tparam num 0-based index of the type to select.
 * @tparam T Variadic parameter pack of types.
 * @return `type` is an alias to the Nth type in `T...`.
 */
template <size_t num, typename... T>
struct nth_type;

/**
 * @brief Recursive case for nth_type template
 *
 * Continues the recursion by decrementing the index and
 * removing the first type from the parameter pack.
 *
 * @tparam num Current index
 * @tparam T Current head type
 * @tparam Y Remaining types in the parameter pack
 */
template <size_t num, typename T, typename... Y>
struct nth_type<num, T, Y...> : nth_type<num - 1, Y...> {};

/**
 * @brief Base case for nth_type template
 *
 * When the index reaches 0, this specialization is selected,
 * which defines the target type as the first type in the current pack.
 *
 * @tparam T The selected type (when index is 0)
 * @tparam Y Remaining types (not used in this specialization)
 */
template <typename T, typename... Y>
struct nth_type<0, T, Y...> {
    using type = T; ///< The type at the specified index - C++23: using alias
};

/**
 * @struct indexes_tuple
 * @ingroup TypeTraits
 * @brief Represents a compile-time tuple of `size_t` indexes.
 * @tparam Indexes A parameter pack of `size_t` indices.
 * @details Used with `index_builder` for variadic template expansion and manipulation.
 *          `size` enum gives the number of indexes.
 */
template <size_t... Indexes>
struct indexes_tuple {
    enum { size = sizeof...(Indexes) }; ///< Number of indices in the tuple
};

/**
 * @struct index_builder
 * @ingroup TypeTraits
 * @brief Generates a compile-time sequence of indices as an `indexes_tuple`.
 * @tparam num The number of indices to generate (from 0 to `num-1`).
 * @tparam tp Internal helper for recursive construction.
 * @return `type` is an `indexes_tuple<0, 1, ..., num-1>`.
 */
template <size_t num, typename tp = indexes_tuple<>>
struct index_builder;

/**
 * @brief Recursive case for index_builder
 *
 * Adds the next index to the tuple and continues the recursion.
 *
 * @tparam num Current count of remaining indices to add
 * @tparam Indexes Current sequence of indices
 */
template <size_t num, size_t... Indexes>
struct index_builder<num, indexes_tuple<Indexes...>>
    : index_builder<num - 1, indexes_tuple<Indexes..., sizeof...(Indexes)>> {};

/**
 * @brief Base case for index_builder
 *
 * When num reaches 0, the recursion stops and the final tuple type is defined.
 *
 * @tparam Indexes The complete sequence of indices
 */
template <size_t... Indexes>
struct index_builder<0, indexes_tuple<Indexes...>> {
    using type = indexes_tuple<Indexes...>; ///< The final tuple type with all indices - C++23: using alias
    enum { size = sizeof...(Indexes) };     ///< Size of the index sequence
};

/**
 * @struct expand
 * @ingroup TypeTraits
 * @brief Utility for forcing parameter pack expansion in certain contexts.
 * @details The constructor takes a variadic pack of arguments. This is useful when you need to
 *          perform an operation on each element of a pack, often using a comma operator inside
 *          an initializer list or braced-init-list for the constructor call.
 * @code
 * // template<typename... Args>
 * // void print_all(Args... args) {
 * //   qb::expand{ (std::cout << args << std::endl, 0)... };
 * // }
 * @endcode
 */
struct expand {
    /**
     * @brief Constructor that expands a parameter pack
     *
     * Takes an arbitrary number of arguments of any type and does nothing with them.
     * This is used purely for the side effect of expanding the parameter pack.
     *
     * @tparam U Types of parameters in the pack
     * @param ... Parameters to expand (not used)
     */
    template <typename... U>
    expand(U const &...) {}
};

} // namespace qb

// ============================================================================
// QB Compile-Time Trait Generation Macros
// ============================================================================
//
// Three macro families cover all detection patterns in the framework:
//
//  ┌──────────────────────────────┬───────────────────────────────────────────┐
//  │ Macro                        │ Detects                                   │
//  ├──────────────────────────────┼───────────────────────────────────────────┤
//  │ QB_DEFINE_METHOD_TRAIT(name) │ .name(Args...) — any or exact return type │
//  │ QB_DEFINE_PROPERTY_TRAIT(nm) │ .nm() or .nm — bool-valued property       │
//  │ QB_DEFINE_MEMBER_TRAIT(name) │ .name() — no-arg method existence         │
//  │ QB_DEFINE_TYPE_TRAIT(name)   │ typename T::name — nested type member     │
//  └──────────────────────────────┴───────────────────────────────────────────┘
//
// Each macro generates, for a given `name`:
//
//   In namespace qb (preferred for new code):
//     qb::has_<name>[<T[, Args...]>]   — simple existence concept
//     qb::has_<name>_r<C, Ret, Args...>— exact return type (METHOD only)
//
//   At global scope (legacy ::value compatibility):
//     has_member_<name><T>::value      — wraps the qb:: concept
//     has_member_func_<name><T>::value — identical alias (historical compat)
//
//   METHOD only — additional struct for `friend struct` declarations:
//     has_method_<name><C, Ret, Args...>::value
//       Ret = void → existence only (qb::has_<name>)
//       Ret ≠ void → exact return  (qb::has_<name>_r, std::same_as<Ret>)
//
// ============================================================================

// ----------------------------------------------------------------------------
// QB_DEFINE_METHOD_TRAIT — variadic method with optional return type constraint
// ----------------------------------------------------------------------------
#define QB_DEFINE_METHOD_TRAIT(name)                                             \
    namespace qb {                                                               \
    /** Concept: C& has .name(Args...) callable, any return type. */             \
    template <typename C, typename... Args>                                      \
    concept has_##name = requires(C &c) {                                        \
        { c.name(std::declval<Args>()...) };                                     \
    };                                                                           \
    /** Concept: C& has .name(Args...) returning exactly Ret. */                 \
    template <typename C, typename Ret, typename... Args>                        \
    concept has_##name##_r = requires(C &c) {                                   \
        { c.name(std::declval<Args>()...) } -> std::same_as<Ret>;               \
    };                                                                           \
    } /* namespace qb */                                                         \
    /** Legacy trait — declared `struct`; befriend with `friend struct has_method_name<...>`. */ \
    /** Ret=void → existence only; Ret≠void → exact return (same_as<Ret>). */   \
    template <typename C, typename Ret, typename... Args>                        \
    struct has_method_##name                                                     \
        : std::bool_constant<std::is_void_v<Ret>                                 \
                                 ? qb::has_##name<C, Args...>                    \
                                 : qb::has_##name##_r<C, Ret, Args...>> {}

// ----------------------------------------------------------------------------
// QB_DEFINE_PROPERTY_TRAIT — bool-valued property: function OR data member
// ----------------------------------------------------------------------------
#define QB_DEFINE_PROPERTY_TRAIT(name)                                           \
    namespace qb {                                                               \
    /** Concept: T has .name() -> bool or .name convertible to bool. */          \
    template <typename T>                                                        \
    concept has_##name = requires(T &t) {                                        \
        { t.name() } -> std::convertible_to<bool>;                              \
    } || requires(T &t) {                                                        \
        { t.name } -> std::convertible_to<bool>;                                \
    };                                                                           \
    } /* namespace qb */                                                         \
    template <typename T>                                                        \
    struct has_member_##name : std::bool_constant<qb::has_##name<T>> {};        \
    template <typename T>                                                        \
    struct has_member_func_##name : std::bool_constant<qb::has_##name<T>> {}

// ----------------------------------------------------------------------------
// QB_DEFINE_MEMBER_TRAIT — no-arg method existence
// ----------------------------------------------------------------------------
#define QB_DEFINE_MEMBER_TRAIT(name)                                             \
    namespace qb {                                                               \
    /** Concept: T has .name() callable (any return type). */                    \
    template <typename T>                                                        \
    concept has_##name = requires(T &t) {                                        \
        { t.name() };                                                            \
    };                                                                           \
    } /* namespace qb */                                                         \
    template <typename T>                                                        \
    struct has_member_##name : std::bool_constant<qb::has_##name<T>> {};        \
    template <typename T>                                                        \
    struct has_member_func_##name : std::bool_constant<qb::has_##name<T>> {}

// ----------------------------------------------------------------------------
// QB_DEFINE_TYPE_TRAIT — nested type alias/typedef detection
// ----------------------------------------------------------------------------
#define QB_DEFINE_TYPE_TRAIT(name)                                               \
    namespace qb {                                                               \
    /** Concept: T has a nested type member T::name. */                          \
    template <typename T>                                                        \
    concept has_type_##name = requires { typename T::name; };                   \
    } /* namespace qb */                                                         \
    template <typename T>                                                        \
    struct has_member_##name : std::bool_constant<qb::has_type_##name<T>> {}

// ============================================================================
// Trait instantiations for all symbols used in the qb framework
// ============================================================================

// Method traits (variadic, with optional exact-return variant)
QB_DEFINE_METHOD_TRAIT(on);
QB_DEFINE_METHOD_TRAIT(read);
QB_DEFINE_METHOD_TRAIT(write);
QB_DEFINE_METHOD_TRAIT(flush);

// Bool property traits (function-or-variable)
QB_DEFINE_PROPERTY_TRAIT(is_alive);
QB_DEFINE_PROPERTY_TRAIT(is_broadcast);
QB_DEFINE_PROPERTY_TRAIT(is_valid);

// No-arg member method traits
QB_DEFINE_MEMBER_TRAIT(disconnect);
QB_DEFINE_MEMBER_TRAIT(shared_from_this);

// Nested-type member trait
QB_DEFINE_TYPE_TRAIT(Protocol);

// ============================================================================
// Own-override detection (CRTP-safe `on(Evt)` routing)
// ============================================================================
//
// `qb::has_on<D, Evt>` is an *existence* check: it returns `true` as soon as
// the expression `d.on(Evt{})` compiles for some `d` of type `D`. When `D`
// participates in a CRTP base/derived relationship where the base class
// *also* defines `on(Evt)`, the concept still evaluates to `true` even when
// `D` did not override it — because the base overload is visible through
// inheritance. Relying on `qb::has_on` to drive a
// `static_cast<D&>(*this).on(e)` re-dispatch therefore produces silent
// *infinite recursion*, a real runtime stack-overflow bug (historically
// masked by clang's `-Winfinite-recursion` diagnostic).
//
// `qb::has_own_on<D, Base, Evt>` closes that hole. It answers the precise
// question: "does @p D carry its own `on(Evt)` implementation, distinct
// from the one inherited from @p Base?". It returns `true` iff one of the
// following holds:
//
//   1. @p D declares `on(Evt&&)` and its member-function pointer differs
//      from @p Base's (compared via pointers cast to `D::*`).
//   2. @p D declares `on(Evt const&)` — an overload that would not exist in
//      @p D by pure inheritance, because @p Base does not expose one.
//
// Non-const lvalue overloads (`on(Evt&)`) are deliberately *not* detected:
// the framework dispatches using `std::move(e)`, which cannot bind to a
// mutable lvalue reference, so such a signature would be an incompatible
// user API.
//
// Usage:
//
//   if constexpr (qb::has_own_on<_Derived, this_type, event::disconnected>)
//       static_cast<_Derived&>(*this).on(std::move(e));
//   else
//       /* fallback: log, throw, ignore, ... */;
//
// @tparam D    Derived user class (the final type in the CRTP chain).
// @tparam Base CRTP base that itself defines `on(Evt...)` (e.g. the
//              framework class performing the dispatch).
// @tparam Evt  Event parameter type (without cv/ref qualifiers).
// ----------------------------------------------------------------------------

namespace qb::detail {

template <typename D, typename Base, typename Evt>
consteval bool compute_has_own_on() {
    // (1) rvalue-reference signature — compare PMFs cast to D::*.
    if constexpr (requires {
                      static_cast<void (D::*)(Evt &&)>(&D::on);
                      static_cast<void (D::*)(Evt &&)>(&Base::on);
                  }) {
        constexpr auto d_pmf = static_cast<void (D::*)(Evt &&)>(&D::on);
        constexpr auto b_pmf = static_cast<void (D::*)(Evt &&)>(&Base::on);
        if (d_pmf != b_pmf)
            return true;
    }
    // (2) const lvalue-reference overload — absent from Base by contract,
    //     so its presence on D is necessarily a user-defined override.
    if constexpr (requires(D &d, const Evt &ev) { d.on(ev); })
        return true;
    return false;
}

} // namespace qb::detail

namespace qb {

/**
 * @brief CRTP-safe detector: does @p D carry its own `on(Evt ...)` handler
 *        (rather than merely inheriting one from @p Base)?
 *
 * Use this instead of `qb::has_on` whenever you are about to
 * `static_cast<D&>(*this).on(e)` from within @p Base — otherwise the
 * redispatch turns into infinite recursion for users that do not override.
 *
 * @tparam D    Derived (user) class.
 * @tparam Base Direct CRTP base that declares an `on(Evt&&)` fallback.
 * @tparam Evt  Event type to route.
 *
 * @see qb::has_on for the plain existence concept.
 * @ingroup TypeTraits
 */
template <typename D, typename Base, typename Evt>
inline constexpr bool has_own_on = detail::compute_has_own_on<D, Base, Evt>();

} // namespace qb

#endif // QB_TYPE_TRAITS_H
