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
 * - SFINAE utilities for detecting member types and functions (e.g., `has_push_back`, macros like `CREATE_MEMBER_CHECK`).
 * - Helper aliases for `std::move` and `std::forward` (`qb::mv`, `qb::fwd`).
 * - Utilities for variadic template expansion (`qb::indexes_tuple`, `qb::expand`).
 *
 * These utilities are primarily for internal framework use but can also be leveraged
 * by application code for advanced template programming.
 * @ingroup Utility
 */

#ifndef QB_TYPE_TRAITS_H
#define QB_TYPE_TRAITS_H
#include <string_view>
#include <type_traits>
#include <utility>
#include <valarray>

/**
 * @brief Alias for `std::move` with a concise syntax.
 * @ingroup MiscUtils
 * @tparam T Type of the value to move.
 * @param t Value to move.
 * @return Value cast to an rvalue reference (`std::remove_reference_t<T>&&`).
 */
template <typename T>
inline std::remove_reference_t<T> &&
mv(T &&t) noexcept {
    return static_cast<std::remove_reference_t<T> &&>(t);
}

/**
 * @brief Alias for `std::forward` with a concise syntax (lvalue overload).
 * @ingroup MiscUtils
 * @tparam T Type to forward.
 * @param t Lvalue reference to forward.
 * @return Forwarded reference, preserving value category and const/volatile qualifiers of `T`.
 */
template <typename T>
inline T &&
fwd(std::remove_reference_t<T> &t) noexcept {
    return std::forward<T>(t);
}

/**
 * @brief Alias for `std::forward` with a concise syntax (rvalue overload).
 * @ingroup MiscUtils
 * @tparam T Type to forward.
 * @param t Rvalue reference to forward.
 * @return Forwarded reference, preserving value category and const/volatile qualifiers of `T`.
 */
template <typename T>
inline T &&
fwd(std::remove_reference_t<T> &&t) noexcept {
    return std::forward<T>(t);
}

namespace qb {

/**
 * @struct crtp
 * @ingroup TypeTraits
 * @brief Base class for implementing the Curiously Recurring Template Pattern (CRTP).
 *
 * This class provides helper methods `impl()` to safely cast the base class pointer/reference
 * to the derived class type `T`. This is the core mechanism of CRTP, enabling static polymorphism
 * and code reuse by allowing base classes to access members of their derived classes.
 *
 * @tparam T The derived class type that inherits from `crtp<T>`.
 */
template <typename T>
struct crtp {
    /**
     * @brief Access the derived class instance
     *
     * @return Reference to the derived class
     */
    inline T &
    impl() noexcept {
        return static_cast<T &>(*this);
    }

    /**
     * @brief Access the derived class instance (const version)
     *
     * @return Const reference to the derived class
     */
    inline T const &
    impl() const noexcept {
        return static_cast<T &>(*this);
    }
};

namespace detail {
/** @private */
struct sfinae_base {
    using yes = char;
    using no  = yes[2];
};

/**
 * @brief Type trait to detect whether T has a const_iterator type
 *
 * @tparam T Type to check
 */
template <typename T>
struct has_const_iterator : private sfinae_base {
private:
    template <typename C>
    static yes &test(typename C::const_iterator *);
    template <typename C>
    static no &test(...);

public:
    static const bool value = sizeof(test<T>(nullptr)) == sizeof(yes);
    using type              = T;
};

/**
 * @brief Type trait to detect whether T has begin() and end() methods
 *
 * Checks if the type has proper const_iterator returning begin and end methods.
 *
 * @tparam T Type to check
 */
template <typename T>
struct has_begin_end : private sfinae_base {
private:
    template <typename C>
    static yes &
    f(typename std::enable_if<std::is_same<
          decltype(static_cast<typename C::const_iterator (C::*)() const>(&C::begin)),
          typename C::const_iterator (C::*)() const>::value>::type *);

    template <typename C>
    static no &f(...);

    template <typename C>
    static yes &
    g(typename std::enable_if<
        std::is_same<
            decltype(static_cast<typename C::const_iterator (C::*)() const>(&C::end)),
            typename C::const_iterator (C::*)() const>::value,
        void>::type *);

    template <typename C>
    static no &g(...);

public:
    /** @brief Whether the type has a valid begin() method */
    static bool const beg_value = sizeof(f<T>(nullptr)) == sizeof(yes);

    /** @brief Whether the type has a valid end() method */
    static bool const end_value = sizeof(g<T>(nullptr)) == sizeof(yes);
};

/**
 * @brief Internal implementation to detect map-like types
 *
 * @tparam T Type to check
 * @tparam U SFINAE enabler
 */
template <typename T, typename U = void>
struct is_mappish_impl : std::false_type {};

/**
 * @brief Specialization for types that have key_type, mapped_type, and operator[]
 *
 * @tparam T Type to check
 */
template <typename T>
struct is_mappish_impl<
    T, std::void_t<
           typename T::key_type, typename T::mapped_type,
           decltype(std::declval<T &>()[std::declval<const typename T::key_type &>()])>>
    : std::true_type {};

} // namespace detail

/**
 * @struct is_container
 * @ingroup TypeTraits
 * @brief Type trait to check if a type `T` is a container.
 * @details A type `T` is considered a container if it has a `const_iterator` nested type,
 *          and `begin()` and `end()` member functions that return this `const_iterator`.
 *          Specializations exist for C-style arrays, `std::valarray`, `std::pair`, and `std::tuple`.
 * @tparam T The type to check.
 * @return `std::true_type` if `T` is a container, `std::false_type` otherwise.
 */
template <typename T>
struct is_container
    : public std::integral_constant<bool, detail::has_const_iterator<T>::value &&
                                              detail::has_begin_end<T>::beg_value &&
                                              detail::has_begin_end<T>::end_value> {};

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
 * @tparam cond A boolean condition. If true, `std::remove_reference<T>::type` is used.
 * @return `type` is `T` if `cond` is false, or `std::remove_reference_t<T>` if `cond` is true.
 *         `value` indicates if the reference was actually removed.
 */
template <typename T, bool cond>
struct remove_reference_if {
    /** @brief Resulting type (unchanged if condition is false) */
    typedef T type;
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
    /** @brief Resulting type with reference removed */
    typedef typename std::remove_reference<T>::type type;
    /** @brief Whether reference was removed */
    constexpr static bool value = true;
};

/**
 * @struct is_mappish
 * @ingroup TypeTraits
 * @brief Type trait to check if a type `T` is map-like.
 * @details A type is considered map-like if it has `key_type` and `mapped_type` nested types,
 *          and an `operator[]` that takes a `const key_type&`.
 * @tparam T The type to check.
 * @return `std::true_type` if `T` is map-like, `std::false_type` otherwise.
 */
template <typename T>
struct is_mappish : detail::is_mappish_impl<T>::type {};

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
    T, typename std::enable_if<!std::is_void<typename T::container_type>::value>::type>
    : std::true_type {};

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
struct iterator_type<Iter, typename std::enable_if<is_inserter<Iter>::value>::type> {
    /** @brief The value type of the underlying container */
    using type = typename std::decay<typename Iter::container_type::value_type>::type;
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
struct is_terator<Iter, typename std::enable_if<is_inserter<Iter>::value>::type>
    : std::true_type {};

/**
 * @brief Specialization for standard iterators
 *
 * Excludes types that can be converted to string_view.
 *
 * @tparam Iter Iterator type
 */
template <typename Iter>
struct is_terator<Iter,
                  typename std::enable_if<!std::is_void<
                      typename std::iterator_traits<Iter>::value_type>::value>::type>
    : std::integral_constant<bool, !std::is_convertible<Iter, std::string_view>::value> {
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
    T, typename std::enable_if<std::is_void<decltype(std::declval<T>().push_back(
           std::declval<typename T::value_type>()))>::value>::type> : std::true_type {};

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
    T, typename std::enable_if<std::is_same<
           decltype(std::declval<T>().insert(std::declval<typename T::const_iterator>(),
                                             std::declval<typename T::value_type>())),
           typename T::iterator>::value>::type> : std::true_type {};

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
          bool, has_push_back<T>::value &&
                    !std::is_same<typename std::decay<T>::type, std::string>::value> {};

/**
 * @struct is_associative_container
 * @ingroup TypeTraits
 * @brief Type trait to check if type `T` is an associative container.
 * @details Considered true if `T` has `insert` but not `push_back`.
 * @tparam T The type to check.
 */
template <typename T>
struct is_associative_container
    : std::integral_constant<bool, has_insert<T>::value && !has_push_back<T>::value> {};

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
    typedef T type; ///< The type at the specified index
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
    typedef indexes_tuple<Indexes...> type; ///< The final tuple type with all indices
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

/**
 * @brief Helper class to force ambiguity of class members
 *
 * Used in the implementation of member detection techniques.
 *
 * @tparam Args Types to inherit from
 */
template <typename... Args>
struct ambiguate : public Args... {};

/**
 * @brief Type trait to check if a type exists
 *
 * Default implementation is false.
 *
 * @tparam A Type to check
 * @tparam SFINAE enabler
 */
template <typename A, typename = void>
struct got_type : std::false_type {};

/**
 * @brief Specialization that indicates the type exists
 *
 * @tparam A Type that exists
 */
template <typename A>
struct got_type<A> : std::true_type {
    /** @brief The type that was found */
    typedef A type;
};

/**
 * @brief Helper for signature checking
 *
 * @tparam T Type of the signature
 * @tparam The actual signature to check
 */
template <typename T, T>
struct sig_check : std::true_type {};

/**
 * @brief Type trait to check if a type has a specific member
 *
 * @tparam Alias Alias type that may contain the member
 * @tparam AmbiguitySeed Seed type for ambiguity resolution
 */
template <typename Alias, typename AmbiguitySeed>
struct has_member {
    template <typename C>
    static char (&f(decltype(&C::value)))[1];
    template <typename C>
    static char (&f(...))[2];

    // Make sure the member name is consistently spelled the same.
    static_assert(
        (sizeof(f<AmbiguitySeed>(0)) == 1),
        "Member name specified in AmbiguitySeed is different from member name specified "
        "in Alias, or wrong Alias/AmbiguitySeed has been specified.");

    /** @brief Whether the member exists */
    static bool const value = sizeof(f<Alias>(0)) == 2;
};

/**
 * @brief Macro to create a type trait to check for any member with a given name
 *
 * Creates has_member_[member] trait that can detect variables, functions,
 * classes, unions, or enums.
 *
 * @param member Name of the member to check for
 */
#define CREATE_MEMBER_CHECK(member)                                               \
                                                                                  \
    template <typename T, typename = std::true_type>                              \
    struct Alias_##member;                                                        \
                                                                                  \
    template <typename T>                                                         \
    struct Alias_##member<                                                        \
        T, std::integral_constant<bool, got_type<decltype(&T::member)>::value>> { \
        static const decltype(&T::member) value;                                  \
    };                                                                            \
                                                                                  \
    struct AmbiguitySeed_##member {                                               \
        char member;                                                              \
    };                                                                            \
                                                                                  \
    template <typename T>                                                         \
    struct has_member_##member {                                                  \
        static const bool value =                                                 \
            has_member<Alias_##member<ambiguate<T, AmbiguitySeed_##member>>,      \
                       Alias_##member<AmbiguitySeed_##member>>::value;            \
    }

/**
 * @brief Macro to create a type trait to check for a member variable with given name
 *
 * Creates has_member_var_[var_name] trait.
 *
 * @param var_name Name of the variable to check for
 */
#define CREATE_MEMBER_VAR_CHECK(var_name)                                              \
                                                                                       \
    template <typename T, typename = std::true_type>                                   \
    struct has_member_var_##var_name : std::false_type {};                             \
                                                                                       \
    template <typename T>                                                              \
    struct has_member_var_##var_name<                                                  \
        T, std::integral_constant<                                                     \
               bool, !std::is_member_function_pointer<decltype(&T::var_name)>::value>> \
        : std::true_type {}

/**
 * @brief Macro to create a type trait to check for a member class with given name
 *
 * Creates has_member_class_[class_name] trait.
 *
 * @param class_name Name of the class to check for
 */
#define CREATE_MEMBER_CLASS_CHECK(class_name)                                       \
                                                                                    \
    template <typename T, typename = std::true_type>                                \
    struct has_member_class_##class_name : std::false_type {};                      \
                                                                                    \
    template <typename T>                                                           \
    struct has_member_class_##class_name<                                           \
        T, std::integral_constant<bool, std::is_class<typename got_type<            \
                                            typename T::class_name>::type>::value>> \
        : std::true_type {}

/**
 * @brief Macro to create a type trait to check for a member union with given name
 *
 * Creates has_member_union_[union_name] trait.
 *
 * @param union_name Name of the union to check for
 */
#define CREATE_MEMBER_UNION_CHECK(union_name)                                       \
                                                                                    \
    template <typename T, typename = std::true_type>                                \
    struct has_member_union_##union_name : std::false_type {};                      \
                                                                                    \
    template <typename T>                                                           \
    struct has_member_union_##union_name<                                           \
        T, std::integral_constant<bool, std::is_union<typename got_type<            \
                                            typename T::union_name>::type>::value>> \
        : std::true_type {}

/**
 * @brief Macro to create a type trait to check for a member enum with given name
 *
 * Creates has_member_enum_[enum_name] trait.
 *
 * @param enum_name Name of the enum to check for
 */
#define CREATE_MEMBER_ENUM_CHECK(enum_name)                                             \
                                                                                        \
    template <typename T, typename = std::true_type>                                    \
    struct has_member_enum_##enum_name : std::false_type {};                            \
                                                                                        \
    template <typename T>                                                               \
    struct has_member_enum_##enum_name<                                                 \
        T,                                                                              \
        std::integral_constant<                                                         \
            bool, std::is_enum<typename got_type<typename T::enum_name>::type>::value>> \
        : std::true_type {}

/**
 * @brief Macro to create a type trait to check for a member function with given name
 *
 * Creates has_member_func_[func] trait that identifies functions specifically
 * (not variables, classes, unions, or enums).
 *
 * @param func Name of the function to check for
 */
#define CREATE_MEMBER_FUNC_CHECK(func)                                                  \
    template <typename T>                                                               \
    struct has_member_func_##func {                                                     \
        static const bool value =                                                       \
            has_member_##func<T>::value && !has_member_var_##func<T>::value &&          \
            !has_member_class_##func<T>::value && !has_member_union_##func<T>::value && \
            !has_member_enum_##func<T>::value;                                          \
    }

/**
 * @brief Macro to create all member check variants for a single member name
 *
 * Creates checks for any member type, variables, classes, unions, enums, and functions.
 *
 * @param member Name of the member to check for
 */
#define CREATE_MEMBER_CHECKS(member)   \
    CREATE_MEMBER_CHECK(member);       \
    CREATE_MEMBER_VAR_CHECK(member);   \
    CREATE_MEMBER_CLASS_CHECK(member); \
    CREATE_MEMBER_UNION_CHECK(member); \
    CREATE_MEMBER_ENUM_CHECK(member);  \
    CREATE_MEMBER_FUNC_CHECK(member)

/**
 * @brief Macro to generate a type trait to check for a method with a specific signature
 *
 * Creates has_method_[method] trait that checks if a type has a method with the
 * given name and matches the specified signature (return type and parameters).
 *
 * @param method Name of the method to check for
 */
#define GENERATE_HAS_METHOD(method)                                                  \
                                                                                     \
    template <typename C, typename Ret, typename... Args>                            \
    class has_method_##method {                                                      \
        template <typename T>                                                        \
        static constexpr auto check(T *) -> typename std::is_same<                   \
            decltype(std::declval<T>().method(std::declval<Args>()...)), Ret>::type; \
                                                                                     \
        template <typename>                                                          \
        static constexpr std::false_type check(...);                                 \
                                                                                     \
        typedef decltype(check<C>(0)) type;                                          \
                                                                                     \
    public:                                                                          \
        static constexpr bool value = type::value;                                   \
    };

/**
 * @brief Macro to generate a type trait to check for a member of any kind
 *
 * Creates has_member_[member] trait using a different technique that avoids
 * ambiguity issues in some contexts.
 *
 * @param member Name of the member to check for
 */
#define GENERATE_HAS_MEMBER(member)                                                   \
                                                                                      \
    template <class T>                                                                \
    class HasMember_##member {                                                        \
    private:                                                                          \
        using Yes = char[2];                                                          \
        using No  = char[1];                                                          \
                                                                                      \
        struct Fallback {                                                             \
            int member;                                                               \
        };                                                                            \
        struct Derived                                                                \
            : T                                                                       \
            , Fallback {};                                                            \
                                                                                      \
        template <class U>                                                            \
        static No &test(decltype(U::member) *);                                       \
        template <typename U>                                                         \
        static Yes &test(U *);                                                        \
                                                                                      \
    public:                                                                           \
        static constexpr bool RESULT = sizeof(test<Derived>(nullptr)) == sizeof(Yes); \
    };                                                                                \
                                                                                      \
    template <class T>                                                                \
    struct has_member_##member                                                        \
        : public std::integral_constant<bool, HasMember_##member<T>::RESULT> {};

/**
 * @brief Macro to generate a type trait to check for a member type
 *
 * Creates has_member_type_[Type] trait that checks if a type has a nested type
 * definition with the given name.
 *
 * @param Type Name of the type to check for
 */
#define GENERATE_HAS_MEMBER_TYPE(Type)                                                \
                                                                                      \
    template <class T>                                                                \
    class HasMemberType_##Type {                                                      \
    private:                                                                          \
        using Yes = char[2];                                                          \
        using No  = char[1];                                                          \
                                                                                      \
        struct Fallback {                                                             \
            struct Type {};                                                           \
        };                                                                            \
        struct Derived                                                                \
            : T                                                                       \
            , Fallback {};                                                            \
                                                                                      \
        template <class U>                                                            \
        static No &test(typename U::Type *);                                          \
        template <typename U>                                                         \
        static Yes &test(U *);                                                        \
                                                                                      \
    public:                                                                           \
        static constexpr bool RESULT = sizeof(test<Derived>(nullptr)) == sizeof(Yes); \
    };                                                                                \
                                                                                      \
    template <class T>                                                                \
    struct has_member_type_##Type                                                     \
        : public std::integral_constant<bool, HasMemberType_##Type<T>::RESULT> {};

// Create member check traits for commonly used members
CREATE_MEMBER_CHECKS(is_alive);
CREATE_MEMBER_CHECKS(is_broadcast);
CREATE_MEMBER_CHECKS(is_valid);
CREATE_MEMBER_CHECKS(disconnect);

// Create method check traits for commonly used methods
GENERATE_HAS_METHOD(on)
GENERATE_HAS_METHOD(read)
GENERATE_HAS_METHOD(write)

#endif // QB_TYPE_TRAITS_H
