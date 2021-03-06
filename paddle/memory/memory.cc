/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserve.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/memory/memory.h"

#include "glog/logging.h"

#include "paddle/platform/enforce.h"
#include "paddle/memory/detail/buddy_allocator.h"
#include "paddle/memory/detail/system_allocator.h"
#include "paddle/platform/gpu_info.h"

#ifdef PADDLE_WITH_FPGA
#include "polaris.h"
#include "paddle/memory/detail/fpga_allocator.h"
#endif

DECLARE_double(fraction_of_gpu_memory_to_use);

namespace paddle {
namespace memory {

using BuddyAllocator = detail::BuddyAllocator;

std::once_flag fpga_allocator_flag;

BuddyAllocator* GetCPUBuddyAllocator() {
  static detail::BuddyAllocator* a = nullptr;
  if (a == nullptr) {
    a = new detail::BuddyAllocator(new detail::CPUAllocator,
                                   platform::CpuMinChunkSize(),
                                   platform::CpuMaxChunkSize());
  }
  return a;
}

template <>
void* Alloc<platform::CPUPlace>(platform::CPUPlace place, size_t size) {
  VLOG(10) << "Allocate " << size << " bytes on " << platform::Place(place);
  void* p = GetCPUBuddyAllocator()->Alloc(size);
  VLOG(10) << "  pointer=" << p;
  return p;
}

template <>
void Free<platform::CPUPlace>(platform::CPUPlace place, void* p) {
  VLOG(10) << "Free pointer=" << p << " on " << platform::Place(place);
  GetCPUBuddyAllocator()->Free(p);
}

template <>
size_t Used<platform::CPUPlace>(platform::CPUPlace place) {
  return GetCPUBuddyAllocator()->Used();
}

#ifdef PADDLE_WITH_CUDA

BuddyAllocator* GetGPUBuddyAllocator(int gpu_id) {
  static BuddyAllocator** as = NULL;
  if (as == NULL) {
    int gpu_num = platform::GetCUDADeviceCount();
    as = new BuddyAllocator*[gpu_num];
    for (int gpu = 0; gpu < gpu_num; gpu++) {
      platform::SetDeviceId(gpu);
      as[gpu] = new BuddyAllocator(new detail::GPUAllocator,
                                   platform::GpuMinChunkSize(),
                                   platform::GpuMaxChunkSize());
    }
    VLOG(10) << "\n\nNOTE: each GPU device use "
             << FLAGS_fraction_of_gpu_memory_to_use * 100
             << "% of GPU memory.\n"
             << "You can set environment variable '"
             << platform::kEnvFractionGpuMemoryToUse
             << "' to change the fraction of GPU usage.\n\n";
  }
  platform::SetDeviceId(gpu_id);
  return as[gpu_id];
}

template <>
void* Alloc<platform::GPUPlace>(platform::GPUPlace place, size_t size) {
  return GetGPUBuddyAllocator(place.device)->Alloc(size);
}

template <>
void Free<platform::GPUPlace>(platform::GPUPlace place, void* p) {
  GetGPUBuddyAllocator(place.device)->Free(p);
}

template <>
size_t Used<platform::GPUPlace>(platform::GPUPlace place) {
  return GetGPUBuddyAllocator(place.device)->Used();
}

#endif

#ifdef PADDLE_WITH_FPGA

#define MAX_FPGA_DEVICE_NUM 10

using FPGAAllocator = detail::FPGAAllocator;

FPGAAllocator* GetFPGAAllocator(int fpga_id) {
  using FPGAAllocVec = std::vector<FPGAAllocator*>;
  static std::unique_ptr<FPGAAllocVec, void (*)(FPGAAllocVec * p)> as{
      new FPGAAllocVec, [](FPGAAllocVec* p) {
        std::for_each(p->begin(), p->end(),
                      [](FPGAAllocator* p) { delete p; });
      }};

  // FPGA allocators
  auto& allocators = *as.get();
  int fpga_devices[MAX_FPGA_DEVICE_NUM];
  int fpga_num = polaris_get_devices(fpga_devices, MAX_FPGA_DEVICE_NUM);

  // FPGA allocator initialization
  std::call_once(fpga_allocator_flag, [&]() {
    allocators.reserve(fpga_num);
    for (int i = 0; i < fpga_num; i++) {
      allocators.emplace_back(new FPGAAllocator(fpga_devices[i]));
    }
  });

  FPGAAllocator* ret = NULL;
  for (int i = 0; i < fpga_num; i++) {
    if (allocators[i]->GetFPGAId() == fpga_id) {
      ret = allocators[i];
      break;
    }
  }

  PADDLE_ENFORCE_NOT_NULL(ret);
  return ret;
}

template <>
void* Alloc<platform::FPGAPlace>(platform::FPGAPlace place, size_t size) {
  return GetFPGAAllocator(place.device)->Alloc(size);
}

template <>
void Free<platform::FPGAPlace>(platform::FPGAPlace place, void* p) {
  GetFPGAAllocator(place.device)->Free(p);
}

template <>
size_t Used<platform::FPGAPlace>(platform::FPGAPlace place) {
  return GetFPGAAllocator(place.device)->Used();
}
#endif

}  // namespace memory
}  // namespace paddle
