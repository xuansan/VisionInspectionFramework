#include <array>
#include <atomic>
#include <concepts>
#include <iostream>
#include <span>
#include <thread>

template <std::integral T>
T sum(std::span<const T> values)
{
    T result{};
    for (const auto value : values) {
        result += value;
    }
    return result;
}

int main()
{
    static_assert(sizeof(void*) == 8, "The development target must be 64-bit.");
    const std::array values{1, 2, 3};
    std::atomic<int> result{0};
    std::jthread worker([&result, &values] {
        result.store(sum<int>(std::span<const int>{values}));
    });
    worker.join();
    if (result.load() != 6) {
        return 1;
    }
    std::cout << "PASS: x64 C++20 concepts/span/jthread/atomic\n";
    return 0;
}
