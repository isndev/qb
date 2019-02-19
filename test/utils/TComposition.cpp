
#include "assert.h"
#include "utils/TComposition.h"

struct Dummy {
    int x = 3;
    int y = 4;

    operator int() {
        return x;
    }
};

int main() {
    test("Getters", [](auto &){
        TComposition<int, double, Dummy> compo {1, 2, {}};
        auto i = compo.get<0>();
        assertEquals(i, 1);
#if __cplusplus >= 201703L
        auto &d = compo.get<double>();
        --d;
        assertEquals(1, compo.get<1>());
        auto const &dummy = compo.get<Dummy>();
        assertEquals(3, dummy);
#endif
        return 0;
    });

    test("Each", [](auto &){
        TComposition<int, double, Dummy> compo {1, 2, {}};
        int ret = 0;

        compo.each([&ret](auto &item) {
            ret += item;
            return 0;
        });
        assertEquals(ret, 6);
#if __cplusplus >= 201703L
        compo.each<0, 1, 2>([&ret](auto &item) {
            ret += item;
            return 0;
        });
        assertEquals(12, ret);
        compo.each_and<0, 1, 2>([&ret](auto &item) {
            ret += item;
            return item;
        });
        assertEquals(18, ret);
        compo.each_or([&ret](auto &item) {
            ret += item;
            return item;
        });
        assertEquals(19, ret);
        compo.each_t<int, double, Dummy>([&ret](auto &item) {
            ret += item;
            return 0;
        });
        assertEquals(25, ret);
#else
        compo.each([&ret](auto &item) {
            ret += item;
            return 0;
        }, std::index_sequence<0, 1, 2>());
        assertEquals(12, ret);
#endif
       return 0;
    });

    test("Take", [](auto &){
        TComposition<int, double, Dummy> compo {1, 2, {}};

        assertEquals(compo.take([](auto &i, auto &d, auto &dummy){
            return i + d + dummy;
        }), 6);
#if __cplusplus >= 201703L
        assertEquals(compo.take<0, 1, 2>([](auto &i, auto &d, auto &dummy){
            return i + d + dummy;
        }), 6);
        assertEquals(compo.take_t<int, double, Dummy>([](auto &i, auto &d, auto &dummy){
            return i + d + dummy;
        }), 6);
#else
        assertEquals(compo.take([](auto &i, auto &d, auto &dummy){
            return i + d + dummy;
        },  std::index_sequence<0, 1, 2>()), 6);
#endif
        return 0;
    });

    return 0;
}

