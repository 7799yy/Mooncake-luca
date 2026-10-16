#pragma once
#include <cassert>
#include <iostream>
struct MockLog {
    template <class T>
    MockLog& operator<<(const T& value) {
        std::cerr << value;
        return *this;
    }
    ~MockLog() { std::cerr << '\n'; }
};
#define LOG(level) MockLog()
#define LOG_ASSERT(condition) assert(condition)
