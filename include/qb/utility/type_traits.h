/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2021 isndev (www.qbaf.io). All rights reserved.
 *
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
 *         limitations under the License.
 */

#ifndef QB_TYPE_TRAITS_H
#define QB_TYPE_TRAITS_H
#include <utility>
#include <type_traits>
#include <valarray>
#include <string_view>

// Alias for std::move
template <typename T>
inline std::remove_reference_t<T>&& mv(T&& t) noexcept {
    return static_cast<std::remove_reference_t<T>&&>(t);
}

// Alias for std::forward
template <typename T>
inline T&& fwd(std::remove_reference_t<T>& t) noexcept {
    return std::forward<T>(t);
}

template <typename T>
inline T&& fwd(std::remove_reference_t<T>&& t) noexcept {
    return std::forward<T>(t);
}

namespace qb {

template <typename T>
struct crtp {
    inline T &impl() noexcept {
        return static_cast<T &>(*this);
    }

    inline T const &impl() const noexcept {
        return static_cast<T &>(*this);
    }
};

namespace detail {
// SFINAE type trait to detect whether T::const_iterator exists.

struct sfinae_base {
    using yes = char;
    using no = yes[2];
};

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
    static bool const beg_value = sizeof(f<T>(nullptr)) == sizeof(yes);
    static bool const end_value = sizeof(g<T>(nullptr)) == sizeof(yes);
};

template <typename T, typename U = void>
struct is_mappish_impl : std::false_type {};

template <typename T>
struct is_mappish_impl<
    T, std::void_t<
           typename T::key_type, typename T::mapped_type,
           decltype(std::declval<T &>()[std::declval<const typename T::key_type &>()])>>
    : std::true_type {};

} // namespace detail

template <typename T>
struct is_container
    : public std::integral_constant<bool, detail::has_const_iterator<T>::value &&
                                              detail::has_begin_end<T>::beg_value &&
                                              detail::has_begin_end<T>::end_value> {};

template <typename T, std::size_t N>
struct is_container<T[N]> : std::true_type {};

template <std::size_t N>
struct is_container<char[N]> : std::false_type {};

template <typename T>
struct is_container<std::valarray<T>> : std::true_type {};

template <typename T1, typename T2>
struct is_container<std::pair<T1, T2>> : std::true_type {};

template <typename... Args>
struct is_container<std::tuple<Args...>> : std::true_type {};

template <typename T, bool cond>
struct remove_reference_if {
    typedef T type;
    constexpr static bool value = false;
};

template <typename T>
struct remove_reference_if<T, true> {
    typedef typename std::remove_reference<T>::type type;
    constexpr static bool value = true;
};

template <typename T>
struct is_mappish : detail::is_mappish_impl<T>::type {};

template <typename...>
struct is_pair : std::false_type {};

template <typename T, typename U>
struct is_pair<std::pair<T, U>> : std::true_type {};

template <typename...>
using Void = void;

template <typename T, typename U = Void<>>
struct is_inserter : std::false_type {};

template <typename T>
// struct is_inserter<T, Void<typename T::container_type>> : std::true_type {};
struct is_inserter<
    T, typename std::enable_if<!std::is_void<typename T::container_type>::value>::type>
    : std::true_type {};

template <typename Iter, typename T = Void<>>
struct iterator_type {
    using type = typename std::iterator_traits<Iter>::value_type;
};

template <typename Iter>
struct iterator_type<Iter,
                     // typename std::enable_if<std::is_void<typename
                     // Iter::value_type>::value>::type> {
                     typename std::enable_if<is_inserter<Iter>::value>::type> {
    using type = typename std::decay<typename Iter::container_type::value_type>::type;
};

template <typename Iter, typename T = Void<>>
struct is_terator : std::false_type {};

template <typename Iter>
struct is_terator<Iter, typename std::enable_if<is_inserter<Iter>::value>::type>
    : std::true_type {};

template <typename Iter>
struct is_terator<Iter,
                  typename std::enable_if<!std::is_void<
                      typename std::iterator_traits<Iter>::value_type>::value>::type>
    : std::integral_constant<bool, !std::is_convertible<Iter, std::string_view>::value> {
};

template <typename T>
struct is_map_iterator : is_pair<typename iterator_type<T>::type> {};

template <typename T, typename = Void<>>
struct has_push_back : std::false_type {};

template <typename T>
struct has_push_back<
    T, typename std::enable_if<std::is_void<decltype(std::declval<T>().push_back(
           std::declval<typename T::value_type>()))>::value>::type> : std::true_type {};

template <typename T, typename = Void<>>
struct has_insert : std::false_type {};

template <typename T>
struct has_insert<
    T, typename std::enable_if<std::is_same<
           decltype(std::declval<T>().insert(std::declval<typename T::const_iterator>(),
                                             std::declval<typename T::value_type>())),
           typename T::iterator>::value>::type> : std::true_type {};

template <typename T>
struct is_sequence_container
    : std::integral_constant<
          bool, has_push_back<T>::value &&
                    !std::is_same<typename std::decay<T>::type, std::string>::value> {};

template <typename T>
struct is_associative_container
    : std::integral_constant<bool, has_insert<T>::value && !has_push_back<T>::value> {};

} // namespace qb

// Variadic to force ambiguity of class members.  C++11 and up.
template <typename... Args>
struct ambiguate : public Args... {};

// Non-variadic version of the line above.
// template <typename A, typename B> struct ambiguate : public A, public B {};

template <typename A, typename = void>
struct got_type : std::false_type {};

template <typename A>
struct got_type<A> : std::true_type {
    typedef A type;
};

template <typename T, T>
struct sig_check : std::true_type {};

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

    static bool const value = sizeof(f<Alias>(0)) == 2;
};

// Check for any member with given name, whether var, func, class, union, enum.
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

// Check for member variable with given name.
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

// Check for member class with given name.
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

// Check for member union with given name.
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

// Check for member enum with given name.
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

// Check for function with given name, any signature.
#define CREATE_MEMBER_FUNC_CHECK(func)                                                  \
    template <typename T>                                                               \
    struct has_member_func_##func {                                                     \
        static const bool value =                                                       \
            has_member_##func<T>::value && !has_member_var_##func<T>::value &&          \
            !has_member_class_##func<T>::value && !has_member_union_##func<T>::value && \
            !has_member_enum_##func<T>::value;                                          \
    }

// Create all the checks for one member.  foobares NOT include func sig checks.
#define CREATE_MEMBER_CHECKS(member)   \
    CREATE_MEMBER_CHECK(member);       \
    CREATE_MEMBER_VAR_CHECK(member);   \
    CREATE_MEMBER_CLASS_CHECK(member); \
    CREATE_MEMBER_UNION_CHECK(member); \
    CREATE_MEMBER_ENUM_CHECK(member);  \
    CREATE_MEMBER_FUNC_CHECK(member)

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

CREATE_MEMBER_CHECKS(is_alive);
CREATE_MEMBER_CHECKS(is_broadcast);
CREATE_MEMBER_CHECKS(is_valid);
CREATE_MEMBER_CHECKS(disconnect);
GENERATE_HAS_METHOD(on)
GENERATE_HAS_METHOD(read)
GENERATE_HAS_METHOD(write)

#endif // QB_TYPE_TRAITS_H
