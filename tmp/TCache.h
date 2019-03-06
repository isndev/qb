
#ifndef QB_UTILS_TCACHE_H_
# define QB_UTILS_TCACHE_H_

template<typename _Item>
class TCache : public _Item {
    using base_t = _Item;
    _Item *_ref;
public:

    TCache()
            : _ref(nullptr) {}

    TCache(_Item &rhs)
            : base_t(rhs), _ref(&rhs) {}

    inline void reload() {
        static_cast<_Item &>(*this) = *_ref;
    }

    template<typename _Func>
    void reload(_Func const &func) {
        func(static_cast<_Item const &>(*this));
        reload();
    }

    inline operator _Item &() {
        return static_cast<_Item &>(*this);
    }

    inline operator _Item const &() const {
        return static_cast<_Item const &>(*this);
    }

    constexpr inline _Item &
    get() {
        return static_cast<_Item &>(*this);
    }

    constexpr inline _Item const &
    get() const {
        return static_cast<_Item const &>(*this);
    }

    constexpr inline _Item &
    ref() {
        return *_ref;
    }

    constexpr inline _Item const &
    ref() const {
        return *_ref;
    }
};

#endif // QB_UTILS_TCACHE_H_