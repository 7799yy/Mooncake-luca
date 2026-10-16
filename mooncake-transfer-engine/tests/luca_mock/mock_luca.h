#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>
#include "lcr/lc_runtime_api.h"
namespace mock {
struct State {
    int device = 0, visible_devices = 1;
    int opens = 0, closes = 0, exports = 0, copies = 0, batches = 0;
    int event_queries = 0, event_creates = 0, event_destroys = 0;
    int syncs = 0, unsafe_closes = 0;
    int fail_copy_at = -1, fail_batch_at = -1, fail_record = 0;
    int fail_query = 0, fail_pending_drain = 0, fail_close = 0,
        fail_event_create = 0;
    std::map<uintptr_t, size_t> allocations;
    std::vector<lcStream_t> streams;
    std::vector<lcEvent_t> events;
};
extern State state;
void reset();
void* allocate(size_t size);
void release(void* pointer);
void completeAll();
size_t pendingCopies();
}  // namespace mock
