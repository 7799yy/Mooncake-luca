// Test-only CUDA spelling bridge. Real compilation uses LUCA's cu-bridge.
#pragma once
#include "lcr/lc_runtime_api.h"
#include "mock_luca.h"
using cudaError_t = lcError_t;
using cudaStream_t = lcStream_t;
using cudaEvent_t = lcEvent_t;
constexpr auto cudaSuccess = lcSuccess, cudaErrorNotReady = lcErrorNotReady;
constexpr int cudaErrorPeerAccessAlreadyEnabled = 704, cudaMemoryTypeDevice = 2;
constexpr auto cudaStreamNonBlocking = lcStreamNonBlocking;
constexpr auto cudaEventDisableTiming = lcEventDisableTiming;
constexpr auto cudaMemcpyDefault = lcMemcpyDefault;
struct cudaPointerAttributes { int type, device; };
#define cudaGetDevice lcGetDevice
#define cudaSetDevice lcSetDevice
#define cudaGetDeviceCount lcGetDeviceCount
#define cudaStreamCreateWithFlags lcStreamCreateWithFlags
#define cudaStreamDestroy lcStreamDestroy
#define cudaEventCreateWithFlags lcEventCreateWithFlags
#define cudaEventDestroy lcEventDestroy
#define cudaEventRecord lcEventRecord
#define cudaMemcpyAsync lcMemcpyAsync
#define cudaStreamPerThread mock::callerStream()
const char* cudaGetErrorString(cudaError_t);
cudaError_t cudaDeviceCanAccessPeer(int*,int,int);
cudaError_t cudaDeviceEnablePeerAccess(int,unsigned);
cudaError_t cudaPointerGetAttributes(cudaPointerAttributes*,const void*);
cudaError_t cudaStreamQuery(cudaStream_t);
cudaError_t cudaStreamWaitEvent(cudaStream_t,cudaEvent_t,unsigned);
cudaError_t cudaEventSynchronize(cudaEvent_t);
cudaError_t cudaMemcpy(void*,const void*,size_t,int);
cudaError_t cudaMalloc(void**,size_t);
cudaError_t cudaFree(void*);
