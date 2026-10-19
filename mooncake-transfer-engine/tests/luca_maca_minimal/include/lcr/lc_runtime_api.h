// Test double declarations; not a vendor header or a vendor ABI specification.
#pragma once
#include <cstddef>
using lcError_t = int;
struct MockStream;
struct MockEvent;
using lcStream_t = MockStream*;
using lcEvent_t = MockEvent*;
constexpr int lcSuccess = 0, lcErrorNotReady = 600;
constexpr unsigned lcStreamNonBlocking = 1, lcEventDisableTiming = 2;
constexpr unsigned lcIpcMemLazyEnablePeerAccess = 1;
constexpr int lcMemcpyDefault = 4, ParallelCopyEngineDefault = 0;
struct lcIpcCrossMemHandle_t {
    unsigned char bytes[1104];
};
struct lcCopyFlag_t {
    void* dst;
    const void* src;
    size_t count;
    int engine, waitNum, writeNum;
};
lcError_t lcGetDevice(int*);
lcError_t lcSetDevice(int);
lcError_t lcGetDeviceCount(int*);
lcError_t lcDeviceSynchronize();
lcError_t lcStreamCreateWithFlags(lcStream_t*, unsigned);
lcError_t lcStreamDestroy(lcStream_t);
lcError_t lcEventCreateWithFlags(lcEvent_t*, unsigned);
lcError_t lcEventDestroy(lcEvent_t);
lcError_t lcEventRecord(lcEvent_t, lcStream_t);
lcError_t lcEventQuery(lcEvent_t);
lcError_t lcMemcpyAsync(void*, const void*, size_t, int, lcStream_t);
lcError_t lcExtBatchCopyFlagAndWaitV2(lcCopyFlag_t*, size_t, void*, size_t,
                                      lcStream_t);
lcError_t lcIpcGetMemHandleCross_v2(lcIpcCrossMemHandle_t*, void*);
lcError_t lcIpcOpenMemHandleCross_v2(void**, lcIpcCrossMemHandle_t*, unsigned);
lcError_t lcIpcCloseMemHandle(void*);
