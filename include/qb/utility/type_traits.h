/**
 * @file qb/utility/type_traits.h
 * @brief Advanced type traits and metaprogramming utilities
 *
 * This file extends the standard library's type_traits with additional
 * type traits and metaprogramming utilities for container detection,
 * iterator properties, and CRTP (Curiously Recurring Template Pattern)
 * support. It provides compile-time introspection capabilities to detect
 * container properties and specialized behavior based on types.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup Utility
 */

#ifndef QB_TYPE_TRAITS_H
#define QB_TYPE_TRAITS_H
#include <utility>
#include <type_traits>
#include <valarray>
#include <string_view>

/**
 * @brief Alias for std::move with cleaner syntax
 * 
 * @tparam T Type of the value to move
 * @param t Value to move
 * @return Value cast to an rvalue reference
 */
template <typename T>
inline std::remove_reference_t<T>&& mv(T&& t) noexcept {
    return static_cast<std::remove_reference_t<T>&&>(t);
}

/**
 * @brief Alias for std::forward with cleaner syntax (lvalue overload)
 * 
 * @tparam T Type to forward
 * @param t Lvalue reference to forward
 * @return Forwarded reference preserving value category
 */
template <typename T>
inline T&& fwd(std::remove_reference_t<T>& t) noexcept {
    return std::forward<T>(t);
}

/**
 * @brief Alias for std::forward with cleaner syntax (rvalue overload)
 * 
 * @tparam T Type to forward
 * @param t Rvalue reference to forward
 * @return Forwarded reference preserving value category
 */
template <typename T>
inline T&& fwd(std::remove_reference_t<T>&& t) noexcept {
    return std::forward<T>(t);
}

namespace qb {

/**
 * @brief Base class for implementing the Curiously Recurring Template Pattern (CRTP)
 * 
 * This class provides helper methods to access the derived class from the base class,
 * which is the core mechanism of CRTP.
 * 
 * @tparam T The derived class type
 */
template <typename T>
struct crtp {
    /**
     * @brief Access the derived class instance
     * 
     * @return Reference to the derived class
     */
    inline T &impl() noexcept {
        return static_cast<T &>(*this);
    }

    /**
     * @brief Access the derived class instance (const version)
     * 
     * @return Const reference to the derived class
     */
    inline T const &impl() const noexcept {
        return static_cast<T &>(*this);
    }
};

namespace detail {
// SFINAE type trait to detect whether T::const_iterator exists.

/**
 * @brief Base class for SFINAE (Substitution Failure Is Not An Error) type traits
 * 
 * Provides yes/no types used to determine sizes in SFINAE detection techniques.
 */
struct sfinae_base {
    using yes = char;
    using no = yes[2];
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
    using type = T;
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
 * @brief Type trait to check if a type is a container
 * 
 * A container must have const_iterator type and begin()/end() methods.
 * 
 * @tparam T Type to check
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
 * @brief Conditionally removes reference from a type
 * 
 * @tparam T Type to process
 * @tparam cond Whether to remove reference
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
 * @brief Type trait to check if a type is map-like (has key_type, mapped_type, and operator[])
 * 
 * @tparam T Type to check
 */
template <typename T>
struct is_mappish : detail::is_mappish_impl<T>::type {};

/**
 * @brief Type trait to check if a type is a std::pair
 * 
 * Default implementation is false.
 * 
 * @tparam Args Template parameters
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
 * @brief Helper alias to create a void type for SFINAE purposes
 * 
 * @tparam Args Ignored template parameters
 */
template <typename...>
using Void = void;

/**
 * @brief Type trait to check if a type is an inserter iterator
 * 
 * Default implementation is false.
 * 
 * @tparam T Type to check
 * @tparam U SFINAE enabler
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
 * @brief Type trait to extract the value type from an iterator
 * 
 * Default implementation uses std::iterator_traits.
 * 
 * @tparam Iter Iterator type
 * @tparam T SFINAE enabler
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
struct iterator_type<Iter,
                     typename std::enable_if<is_inserter<Iter>::value>::type> {
    /** @brief The value type of the underlying container */
    using type = typename std::decay<typename Iter::container_type::value_type>::type;
};

/**
 * @brief Type trait to check if a type is an iterator
 * 
 * Default implementation is false.
 * 
 * @tparam Iter Type to check
 * @tparam T SFINAE enabler
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
 * @brief Type trait to check if an iterator points to map elements (pairs)
 * 
 * @tparam T Iterator type
 */
template <typename T>
struct is_map_iterator : is_pair<typename iterator_type<T>::type> {};

/**
 * @brief Type trait to check if a container has push_back method
 * 
 * Default implementation is false.
 * 
 * @tparam T Type to check
 * @tparam SFINAE enabler
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
 * @brief Type trait to check if a container has insert method
 * 
 * Default implementation is false.
 * 
 * @tparam T Type to check
 * @tparam SFINAE enabler
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
 * @brief Type trait to check if a type is a sequence container
 * 
 * A sequence container has push_back and is not a string.
 * 
 * @tparam T Type to check
 */
template <typename T>
struct is_sequence_container
    : std::integral_constant<
          bool, has_push_back<T>::value &&
                    !std::is_same<typename std::decay<T>::type, std::string>::value> {};

/**
 * @brief Type trait to check if a type is an associative container
 * 
 * An associative container has insert but not push_back.
 * 
 * @tparam T Type to check
 */
template <typename T>
struct is_associative_container
    : std::integral_constant<bool, has_insert<T>::value && !has_push_back<T>::value> {};

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
        using No = char[1];                                                           \
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
        using No = char[1];                                                           \
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
