#pragma once
#include <cstddef>
#include <cstdint>
using CUdeviceptr = uintptr_t;
using CUresult = int;
constexpr int CUDA_SUCCESS = 0;
CUresult cuMemGetAddressRange(CUdeviceptr*, size_t*, CUdeviceptr);
