/*
 * qb - C++ Actor Framework
 * Copyright (C) 2011-2019 isndev (www.qbaf.io). All rights reserved.
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
#include <type_traits>

namespace qb {
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

}

//Variadic to force ambiguity of class members.  C++11 and up.
template <typename... Args> struct ambiguate : public Args... {};

//Non-variadic version of the line above.
//template <typename A, typename B> struct ambiguate : public A, public B {};

template<typename A, typename = void>
struct got_type : std::false_type {};

template<typename A>
struct got_type<A> : std::true_type {
    typedef A type;
};

template<typename T, T>
struct sig_check : std::true_type {};

template<typename Alias, typename AmbiguitySeed>
struct has_member {
    template<typename C> static char (&f(decltype(&C::value)))[1];
    template<typename C> static char (&f(...))[2];

    //Make sure the member name is consistently spelled the same.
    static_assert(
            (sizeof(f<AmbiguitySeed>(0)) == 1)
            , "Member name specified in AmbiguitySeed is different from member name specified in Alias, or wrong Alias/AmbiguitySeed has been specified."
    );

    static bool const value = sizeof(f<Alias>(0)) == 2;
};

//Check for any member with given name, whether var, func, class, union, enum.
#define CREATE_MEMBER_CHECK(member)                                         \
                                                                            \
template<typename T, typename = std::true_type>                             \
struct Alias_##member;                                                      \
                                                                            \
template<typename T>                                                        \
struct Alias_##member <                                                     \
    T, std::integral_constant<bool, got_type<decltype(&T::member)>::value>  \
> { static const decltype(&T::member) value; };                             \
                                                                            \
struct AmbiguitySeed_##member { char member; };                             \
                                                                            \
template<typename T>                                                        \
struct has_member_##member {                                                \
    static const bool value                                                 \
        = has_member<                                                       \
            Alias_##member<ambiguate<T, AmbiguitySeed_##member>>            \
            , Alias_##member<AmbiguitySeed_##member>                        \
        >::value                                                            \
    ;                                                                       \
}

//Check for member variable with given name.
#define CREATE_MEMBER_VAR_CHECK(var_name)                                   \
                                                                            \
template<typename T, typename = std::true_type>                             \
struct has_member_var_##var_name : std::false_type {};                      \
                                                                            \
template<typename T>                                                        \
struct has_member_var_##var_name<                                           \
    T                                                                       \
    , std::integral_constant<                                               \
        bool                                                                \
        , !std::is_member_function_pointer<decltype(&T::var_name)>::value   \
    >                                                                       \
> : std::true_type {}

//Check for member class with given name.
#define CREATE_MEMBER_CLASS_CHECK(class_name)               \
                                                            \
template<typename T, typename = std::true_type>             \
struct has_member_class_##class_name : std::false_type {};  \
                                                            \
template<typename T>                                        \
struct has_member_class_##class_name<                       \
    T                                                       \
    , std::integral_constant<                               \
        bool                                                \
        , std::is_class<                                    \
            typename got_type<typename T::class_name>::type \
        >::value                                            \
    >                                                       \
> : std::true_type {}

//Check for member union with given name.
#define CREATE_MEMBER_UNION_CHECK(union_name)               \
                                                            \
template<typename T, typename = std::true_type>             \
struct has_member_union_##union_name : std::false_type {};  \
                                                            \
template<typename T>                                        \
struct has_member_union_##union_name<                       \
    T                                                       \
    , std::integral_constant<                               \
        bool                                                \
        , std::is_union<                                    \
            typename got_type<typename T::union_name>::type \
        >::value                                            \
    >                                                       \
> : std::true_type {}

//Check for member enum with given name.
#define CREATE_MEMBER_ENUM_CHECK(enum_name)                 \
                                                            \
template<typename T, typename = std::true_type>             \
struct has_member_enum_##enum_name : std::false_type {};    \
                                                            \
template<typename T>                                        \
struct has_member_enum_##enum_name<                         \
    T                                                       \
    , std::integral_constant<                               \
        bool                                                \
        , std::is_enum<                                     \
            typename got_type<typename T::enum_name>::type  \
        >::value                                            \
    >                                                       \
> : std::true_type {}

//Check for function with given name, any signature.
#define CREATE_MEMBER_FUNC_CHECK(func)          \
template<typename T>                            \
struct has_member_func_##func {                 \
    static const bool value                     \
        = has_member_##func<T>::value           \
        && !has_member_var_##func<T>::value     \
        && !has_member_class_##func<T>::value   \
        && !has_member_union_##func<T>::value   \
        && !has_member_enum_##func<T>::value    \
    ;                                           \
}

//Create all the checks for one member.  foobares NOT include func sig checks.
#define CREATE_MEMBER_CHECKS(member)    \
CREATE_MEMBER_CHECK(member);            \
CREATE_MEMBER_VAR_CHECK(member);        \
CREATE_MEMBER_CLASS_CHECK(member);      \
CREATE_MEMBER_UNION_CHECK(member);      \
CREATE_MEMBER_ENUM_CHECK(member);       \
CREATE_MEMBER_FUNC_CHECK(member)

#define GENERATE_HAS_METHOD(method)                                               \
                                                                                  \
template<typename C, typename Ret, typename... Args>                              \
class has_method_##method {                                                       \
    template<typename T>                                                          \
    static constexpr auto check(T*)                                               \
    -> typename                                                                   \
    std::is_same<                                                                 \
            decltype( std::declval<T>().method( std::declval<Args>()... ) ),      \
            Ret                                                                   \
    >::type;                                                                      \
                                                                                  \
    template<typename>                                                            \
    static constexpr std::false_type check(...);                                  \
                                                                                  \
    typedef decltype(check<C>(0)) type;                                           \
                                                                                  \
public:                                                                           \
    static constexpr bool value = type::value;                                    \
};


#define GENERATE_HAS_MEMBER(member)                                               \
                                                                                  \
template < class T >                                                              \
class HasMember_##member                                                          \
{                                                                                 \
private:                                                                          \
    using Yes = char[2];                                                          \
    using  No = char[1];                                                          \
                                                                                  \
    struct Fallback { int member; };                                              \
    struct Derived : T, Fallback { };                                             \
                                                                                  \
    template < class U >                                                          \
    static No& test ( decltype(U::member)* );                                     \
    template < typename U >                                                       \
    static Yes& test ( U* );                                                      \
                                                                                  \
public:                                                                           \
    static constexpr bool RESULT = sizeof(test<Derived>(nullptr)) == sizeof(Yes); \
};                                                                                \
                                                                                  \
template < class T >                                                              \
struct has_member_##member                                                        \
: public std::integral_constant<bool, HasMember_##member<T>::RESULT>              \
{                                                                                 \
};

#define GENERATE_HAS_MEMBER_TYPE(Type)                                            \
                                                                                  \
template < class T >                                                              \
class HasMemberType_##Type                                                        \
{                                                                                 \
private:                                                                          \
    using Yes = char[2];                                                          \
    using  No = char[1];                                                          \
                                                                                  \
    struct Fallback { struct Type { }; };                                         \
    struct Derived : T, Fallback { };                                             \
                                                                                  \
    template < class U >                                                          \
    static No& test ( typename U::Type* );                                        \
    template < typename U >                                                       \
    static Yes& test ( U* );                                                      \
                                                                                  \
public:                                                                           \
    static constexpr bool RESULT = sizeof(test<Derived>(nullptr)) == sizeof(Yes); \
};                                                                                \
                                                                                  \
template < class T >                                                              \
struct has_member_type_##Type                                                     \
: public std::integral_constant<bool, HasMemberType_##Type<T>::RESULT>            \
{ };

CREATE_MEMBER_CHECKS(is_alive);
CREATE_MEMBER_CHECKS(is_broadcast);
CREATE_MEMBER_CHECKS(is_valid);

#endif //QB_TYPE_TRAITS_H
