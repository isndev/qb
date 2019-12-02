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

#endif //QB_TYPE_TRAITS_H
