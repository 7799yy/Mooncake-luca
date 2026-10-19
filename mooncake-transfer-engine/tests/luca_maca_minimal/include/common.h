#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>
#include <glog/logging.h>
#include "error.h"
namespace mooncake {
inline int64_t mock_now_ns = 1;
inline int64_t getCurrentTimeInNano() { return mock_now_ns; }
constexpr int LOCAL_SEGMENT_ID = 0;
struct RWSpinlock {
    std::mutex mutex;
    void lock() { mutex.lock(); }
    void unlock() { mutex.unlock(); }
    void lockShared() { mutex.lock(); }
    void unlockShared() { mutex.unlock(); }
    struct WriteGuard {
        RWSpinlock& lock;
        explicit WriteGuard(RWSpinlock& value) : lock(value) { lock.lock(); }
        ~WriteGuard() { lock.unlock(); }
    };
};
class Topology {};
}  // namespace mooncake
