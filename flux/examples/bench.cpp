// bench.cpp — 与 bench_vm.flux 等价的 C++ 基准测试
#include <iostream>
#include <vector>
#include <chrono>
#include <numeric>

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int main() {
    using Clock = std::chrono::high_resolution_clock;

    // 测试1: 5000次 factorial(12)
    auto t0 = Clock::now();
    volatile int sink = 0;  // 防止编译器优化掉
    for (int i = 0; i < 5000; ++i) {
        sink = factorial(12);
    }
    auto t1 = Clock::now();
    double elapsed1 = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "5000x factorial(12): " << elapsed1 << "s\n";

    // 测试2: 10000元素数组构建+求和
    auto t2 = Clock::now();
    std::vector<int> arr;
    for (int j = 0; j < 10000; ++j) {
        arr.push_back(j);
    }
    long long sum = 0;
    for (int v : arr) {
        sum += v;
    }
    auto t3 = Clock::now();
    double elapsed2 = std::chrono::duration<double>(t3 - t2).count();
    std::cout << "10000-element array build+sum: " << elapsed2 << "s\n";
    std::cout << "sum = " << sum << "\n";

    return 0;
}
