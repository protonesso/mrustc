/*
 */
#ifndef COMMON_HPP_INCLUDED
#define COMMON_HPP_INCLUDED

#include <iostream>
#include <vector>
#include <map>
#include <cassert>
#include <sstream>
#include <memory>

#define FMT(ss)    (dynamic_cast< ::std::stringstream&>(::std::stringstream() << ss).str())
// XXX: Evil hack - Define 'mv$' to be ::std::move
#define mv$(x)    ::std::move(x)
#define box$(x) ::make_unique_ptr(::std::move(x))
#define rc_new$(x) ::make_shared_ptr(::std::move(x))

#include "include/debug.hpp"
#include "include/rustic.hpp"	// slice and option
#include "include/compile_error.hpp"

template<typename T>
::std::unique_ptr<T> make_unique_ptr(T&& v) {
    return ::std::unique_ptr<T>(new T(mv$(v)));
}
template<typename T>
::std::shared_ptr<T> make_shared_ptr(T&& v) {
    return ::std::shared_ptr<T>(new T(mv$(v)));
}

enum Ordering
{
    OrdLess,
    OrdEqual,
    OrdGreater,
};
template<typename T>
Ordering ord(const ::std::vector<T>& l, const ::std::vector<T>& r)
{
    unsigned int i = 0;
    for(const auto& it : l)
    {
        if( i >= r.size() )
            return OrdGreater;
        
        auto rv = it.ord(r[i]);
        if( rv != OrdEqual )
            return rv;
        
        i ++;
    }
        
    return OrdEqual;
}
static inline Ordering ord(bool l, bool r)
{
    if(l == r)
        return OrdEqual;
    else if( l )
        return OrdGreater;
    else
        return OrdLess;
}
static inline Ordering ord(unsigned l, unsigned r)
{
    if(l == r)
        return OrdEqual;
    else if( l > r )
        return OrdGreater;
    else
        return OrdLess;
}
static inline Ordering ord(const ::std::string& l, const ::std::string& r)
{
    if(l == r)
        return OrdEqual;
    else if( l > r )
        return OrdGreater;
    else
        return OrdLess;
}


template <typename T>
struct LList
{
    const LList*  m_prev;
    T   m_item;
    
    LList(const LList* prev, T item):
        m_prev(prev),
        m_item( ::std::move(item) )
    {
    };
};

namespace std {

template <typename T>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::vector<T*>& v) {
    if( v.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : v )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << *i;
        }
    }
    return os;
}

template <typename T>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::vector<T>& v) {
    if( v.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : v )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << i;
        }
    }
    return os;
}

template <typename T, typename U>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::pair<T,U>& v) {
    os << "(" << v.first << ", " << v.second << ")";
    return os;
}

template <typename T, typename U, class Cmp>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::map<T,U,Cmp>& v) {
    if( v.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : v )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << i.first << ": " << i.second;
        }
    }
    return os;
}

template <typename T, typename U, class Cmp>
inline ::std::ostream& operator<<(::std::ostream& os, const ::std::multimap<T,U,Cmp>& v) {
    if( v.size() > 0 )
    {
        bool is_first = true;
        for( const auto& i : v )
        {
            if(!is_first)
                os << ", ";
            is_first = false;
            os << i.first << ": " << i.second;
        }
    }
    return os;
}

}

#endif
