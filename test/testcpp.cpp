/* 用于验证 -demangle 的 C++ 测试程序（含重载与命名空间）。 */
#include <cstdio>
#include <string>

namespace calc {
template <typename T> T add(T a, T b) { return a + b; }

struct Widget {
    int value;
    Widget(int v) : value(v) {}
    int compute(int x) const { return value + x; }
};
}

int main() {
    std::string s = "c++";
    calc::Widget w(10);
    int r = calc::add<int>(w.compute(5), 1) + (int)s.length();
    std::printf("%d\n", r);
    return 0;
}
