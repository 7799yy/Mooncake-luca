#include "mock_luca.h"
#include "cuda.h"
#include "common.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>

struct MockOperation {
    std::function<void()> run;
    std::vector<std::pair<uintptr_t, size_t>> ranges;
};
struct MockStream {
    std::vector<MockOperation> operations;
    size_t completed = 0;
};
struct MockEvent {
    lcStream_t stream = nullptr;
    size_t sequence = 0;
};
namespace mock {
State state;
size_t pendingCopies() {
    size_t count = 0;
    for (auto* stream : state.streams)
        for (size_t i = stream->completed; i < stream->operations.size(); ++i)
            count += !stream->operations[i].ranges.empty();
    return count;
}
void completeAll() {
    for (auto* stream : state.streams)
        while (stream->completed < stream->operations.size()) {
            auto index = stream->completed++;
            stream->operations[index].run();
        }
}
void reset() {
    assert(state.allocations.empty() && state.streams.empty() &&
           state.events.empty());
    state = State{};
    mooncake::mock_now_ns = 1;
}
void* allocate(size_t size) {
    auto* memory = std::calloc(1, size);
    assert(memory);
    state.allocations.emplace(reinterpret_cast<uintptr_t>(memory), size);
    return memory;
}
void release(void* memory) {
    state.allocations.erase(reinterpret_cast<uintptr_t>(memory));
    std::free(memory);
}
}  // namespace mock
lcError_t lcGetDevice(int* out) {
    *out = mock::state.device;
    return 0;
}
lcError_t lcSetDevice(int value) {
    mock::state.device = value;
    return 0;
}
lcError_t lcGetDeviceCount(int* out) {
    *out = mock::state.visible_devices;
    return 0;
}
lcError_t lcDeviceSynchronize() {
    ++mock::state.syncs;
    if (mock::pendingCopies() && mock::state.fail_pending_drain > 0) {
        --mock::state.fail_pending_drain;
        return 91;
    }
    mock::completeAll();
    return 0;
}
lcError_t lcStreamCreateWithFlags(lcStream_t* out, unsigned) {
    *out = new MockStream;
    mock::state.streams.push_back(*out);
    return 0;
}
lcError_t lcStreamDestroy(lcStream_t stream) {
    assert(stream->completed == stream->operations.size());
    auto& streams = mock::state.streams;
    streams.erase(std::find(streams.begin(), streams.end(), stream));
    delete stream;
    return 0;
}
lcError_t lcEventCreateWithFlags(lcEvent_t* out, unsigned) {
    if (mock::state.fail_event_create > 0) {
        --mock::state.fail_event_create;
        return 92;
    }
    *out = new MockEvent;
    ++mock::state.event_creates;
    mock::state.events.push_back(*out);
    return 0;
}
lcError_t lcEventDestroy(lcEvent_t event) {
    auto& events = mock::state.events;
    events.erase(std::find(events.begin(), events.end(), event));
    ++mock::state.event_destroys;
    delete event;
    return 0;
}
lcError_t lcEventRecord(lcEvent_t event, lcStream_t stream) {
    if (mock::state.fail_record > 0) {
        --mock::state.fail_record;
        return 93;
    }
    event->stream = stream;
    event->sequence = stream->operations.size();
    return 0;
}
lcError_t lcEventQuery(lcEvent_t event) {
    ++mock::state.event_queries;
    if (mock::state.fail_query > 0) {
        --mock::state.fail_query;
        return 94;
    }
    return event->stream->completed >= event->sequence ? lcSuccess
                                                       : lcErrorNotReady;
}
lcError_t lcMemcpyAsync(void* dst, const void* src, size_t length, int,
                        lcStream_t stream) {
    ++mock::state.copies;
    stream->operations.push_back(
        {[=] { std::memmove(dst, src, length); },
         {{reinterpret_cast<uintptr_t>(dst), length},
          {reinterpret_cast<uintptr_t>(src), length}}});
    // Simulate partial enqueue even when the API reports an error.
    return mock::state.copies == mock::state.fail_copy_at ? 95 : 0;
}
lcError_t lcExtBatchCopyFlagAndWaitV2(lcCopyFlag_t* batch, size_t count, void*,
                                      size_t, lcStream_t stream) {
    ++mock::state.batches;
    std::vector<std::pair<uintptr_t, size_t>> ranges;
    for (size_t i = 0; i < count; ++i) {
        ranges.emplace_back(reinterpret_cast<uintptr_t>(batch[i].dst),
                            batch[i].count);
        ranges.emplace_back(reinterpret_cast<uintptr_t>(batch[i].src),
                            batch[i].count);
    }
    // Deliberately read the original descriptor storage only at completion.
    stream->operations.push_back(
        {[=] {
             for (size_t i = 0; i < count; ++i)
                 std::memmove(batch[i].dst, batch[i].src, batch[i].count);
         },
         std::move(ranges)});
    return mock::state.batches == mock::state.fail_batch_at ? 96 : 0;
}
lcError_t lcIpcGetMemHandleCross_v2(lcIpcCrossMemHandle_t* handle, void* base) {
    std::memset(handle, 0, sizeof(*handle));
    auto address = reinterpret_cast<uintptr_t>(base);
    int generation = ++mock::state.exports;
    std::memcpy(handle->bytes, &address, sizeof(address));
    std::memcpy(handle->bytes + sizeof(address), &generation,
                sizeof(generation));
    return 0;
}
lcError_t lcIpcOpenMemHandleCross_v2(void** out, lcIpcCrossMemHandle_t* handle,
                                     unsigned) {
    uintptr_t base = 0;
    std::memcpy(&base, handle->bytes, sizeof(base));
    if (!mock::state.allocations.count(base)) return 17;
    ++mock::state.opens;
    *out = reinterpret_cast<void*>(base);
    return 0;
}
lcError_t lcIpcCloseMemHandle(void* pointer) {
    auto base = reinterpret_cast<uintptr_t>(pointer);
    size_t size = mock::state.allocations.at(base);
    for (auto* stream : mock::state.streams)
        for (size_t i = stream->completed; i < stream->operations.size(); ++i)
            for (const auto& range : stream->operations[i].ranges)
                if (range.first >= base && range.first - base < size)
                    ++mock::state.unsafe_closes;
    ++mock::state.closes;
    if (mock::state.fail_close > 0) {
        --mock::state.fail_close;
        return 97;
    }
    return 0;
}
CUresult cuMemGetAddressRange(CUdeviceptr* base, size_t* size,
                              CUdeviceptr pointer) {
    for (const auto& entry : mock::state.allocations)
        if (pointer >= entry.first && pointer - entry.first < entry.second) {
            *base = entry.first;
            *size = entry.second;
            return 0;
        }
    return 1;
}
