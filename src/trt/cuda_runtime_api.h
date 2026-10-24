#pragma once
// Minimal CUDA runtime type stubs for TensorRT-RTX headers.
// The implementation uses the CUDA driver API from nvcuda.dll, so the CUDA
// Toolkit headers/runtime are not required to build this target.
#include <cstddef>
typedef struct CUstream_st* cudaStream_t;
typedef struct CUevent_st* cudaEvent_t;
typedef int cudaError_t;
