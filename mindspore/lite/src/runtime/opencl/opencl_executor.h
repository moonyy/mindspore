/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MINDSPORE_LITE_SRC_OPENCL_EXECUTOR_H_
#define MINDSPORE_LITE_SRC_OPENCL_EXECUTOR_H_

#include <vector>
#include "src/runtime/opencl/opencl_runtime.h"
#include "src/runtime/allocator.h"
#include "src/runtime/kernel/opencl/opencl_kernel.h"
#include "src/executor.h"
#include "include/lite_session.h"

namespace mindspore::lite::opencl {
class OpenCLExecutor : Executor {
 public:
  OpenCLExecutor() : Executor() { allocator_ = OpenCLRuntime::GetInstance()->GetAllocator(); }

  int Prepare(const std::vector<kernel::LiteKernel *> &kernels);

  int Run(std::vector<tensor::Tensor *> &inputs, std::vector<tensor::Tensor *> &outputs,
          std::vector<kernel::LiteKernel *> &kernels, Allocator *allocator = nullptr,
          const session::KernelCallBack &before = nullptr, const session::KernelCallBack &after = nullptr);

 protected:
  Context *context = nullptr;
  OpenCLAllocator *allocator_;
};
}  // namespace mindspore::lite::opencl
#endif
