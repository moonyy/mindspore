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

#include "src/runtime/kernel/arm/fp32/matmul.h"
#include "nnacl/fp32/matmul.h"
#include "src/runtime/runtime_api.h"
#include "include/errorcode.h"

using mindspore::lite::RET_ERROR;
using mindspore::lite::RET_INPUT_TENSOR_ERROR;
using mindspore::lite::RET_MEMORY_FAILED;
using mindspore::lite::RET_OK;

namespace mindspore::kernel {
MatmulCPUKernel::~MatmulCPUKernel() { FreeTmpBuffer(); }

void MatmulCPUKernel::FreeTmpBuffer() {
  if (a_c12_ptr_ != nullptr) {
    ctx_->allocator->Free(a_c12_ptr_);
    a_c12_ptr_ = nullptr;
  }
  if (b_r8_ptr_ != nullptr) {
    ctx_->allocator->Free(b_r8_ptr_);
    b_r8_ptr_ = nullptr;
  }
  if (bias_ptr_ != nullptr) {
    ctx_->allocator->Free(bias_ptr_);
    bias_ptr_ = nullptr;
  }
}

int MatmulCPUKernel::ReSize() {
  FreeTmpBuffer();
  int batch = 1;
  auto a_shape = in_tensors_[0]->shape();
  auto c_shape = out_tensors_[0]->shape();
  if (in_tensors_.size() == 3) {
    auto bias_shape = in_tensors_[2]->shape();
    if (bias_shape[bias_shape.size() - 1] != c_shape[c_shape.size() - 1]) {
      MS_LOG(ERROR) << "The bias' dimension is not equal with column";
      return RET_INPUT_TENSOR_ERROR;
    }
  }

  for (size_t i = 0; i < a_shape.size() - 2; ++i) {
    batch *= a_shape[i];
  }
  params_->batch = batch;
  params_->row_ = c_shape[c_shape.size() - 2];
  params_->col_ = c_shape[c_shape.size() - 1];
  params_->deep_ = params_->a_transpose_ ? a_shape[a_shape.size() - 2] : a_shape[a_shape.size() - 1];
  params_->row_12_ = UP_ROUND(params_->row_, C12NUM);
  params_->col_8_ = UP_ROUND(params_->col_, 8);
  thread_count_ = MSMIN(thread_count_, UP_DIV(params_->col_8_, 8));
  thread_stride_ = UP_DIV(UP_DIV(params_->col_8_, 8), thread_count_);

  a_c12_ptr_ = reinterpret_cast<float *>(ctx_->allocator->Malloc(params_->row_12_ * params_->deep_ * sizeof(float)));
  if (a_c12_ptr_ == nullptr) {
    FreeTmpBuffer();
    return RET_MEMORY_FAILED;
  }
  memset(a_c12_ptr_, 0, params_->row_12_ * params_->deep_ * sizeof(float));
  b_r8_ptr_ = reinterpret_cast<float *>(ctx_->allocator->Malloc(params_->col_8_ * params_->deep_ * sizeof(float)));
  if (b_r8_ptr_ == nullptr) {
    FreeTmpBuffer();
    return RET_MEMORY_FAILED;
  }
  memset(b_r8_ptr_, 0, params_->col_8_ * params_->deep_ * sizeof(float));

  params_->a_const_ = false;
  params_->b_const_ = false;
  InitMatrixA(reinterpret_cast<float *>(in_tensors_[0]->Data()), a_c12_ptr_);
  InitMatrixB(reinterpret_cast<float *>(in_tensors_[1]->Data()), b_r8_ptr_);

  bias_ptr_ = reinterpret_cast<float *>(malloc(params_->col_8_ * sizeof(float)));
  if (bias_ptr_ == nullptr) {
    FreeTmpBuffer();
    return RET_MEMORY_FAILED;
  }
  memset(bias_ptr_, 0, params_->col_8_ * sizeof(float));
  if (in_tensors_.size() == 3) {
    memcpy(bias_ptr_, in_tensors_[2]->Data(), params_->col_ * sizeof(float));
  }

  return RET_OK;
}

void MatmulCPUKernel::InitMatrixA(float *src_ptr, float *dst_ptr) {
  if (params_->a_const_ == true) {
    return;
  }
  if (src_ptr == nullptr) {
    return;
  }
  params_->a_const_ = true;

  if (params_->a_transpose_) {
    RowMajor2Row12Major(src_ptr, dst_ptr, params_->deep_, params_->row_);
  } else {
    RowMajor2Col12Major(src_ptr, dst_ptr, params_->row_, params_->deep_);
  }
  return;
}

void MatmulCPUKernel::InitMatrixB(float *src_ptr, float *dst_ptr) {
  if (params_->b_const_ == true) {
    return;
  }
  if (src_ptr == nullptr) {
    return;
  }
  params_->b_const_ = true;

  if (params_->b_transpose_) {
    RowMajor2Col8Major(src_ptr, dst_ptr, params_->col_, params_->deep_);
  } else {
    RowMajor2Row8Major(src_ptr, dst_ptr, params_->deep_, params_->col_);
  }
  return;
}

int MatmulCPUKernel::Init() {
  if (!InferShapeDone()) {
    return RET_OK;
  }
  return ReSize();
}

int MatmulCPUKernel::RunImpl(int task_id) {
  int cur_oc = MSMIN(thread_stride_ * C8NUM, params_->col_ - task_id * thread_stride_ * C8NUM);
  if (cur_oc <= 0) {
    return RET_OK;
  }
  MatMulOpt(a_c12_ptr_, b_r8_ptr_ + task_id * thread_stride_ * C8NUM * params_->deep_,
            c_r_ptr_ + task_id * thread_stride_ * C8NUM, bias_ptr_ + task_id * thread_stride_ * C8NUM, ActType_No,
            params_->deep_, params_->row_, cur_oc, params_->col_, OutType_Nhwc);
  return RET_OK;
}

int MatmulFloatRun(int task_id, LiteParallelGroupEnv *penv, void *cdata) {
  auto op = reinterpret_cast<MatmulCPUKernel *>(cdata);
  auto error_code = op->RunImpl(task_id);
  if (error_code != RET_OK) {
    MS_LOG(ERROR) << "MatmulFp32Run error task_id[" << task_id << "] error_code[" << error_code << "]";
    return RET_ERROR;
  }
  return RET_OK;
}

int MatmulCPUKernel::Run() {
  auto prepare_ret = Prepare();
  if (prepare_ret != RET_OK) {
    MS_LOG(ERROR) << "Prepare fail!ret: " << prepare_ret;
    return prepare_ret;
  }
  auto a_ptr = reinterpret_cast<float *>(in_tensors_[0]->Data());
  auto b_ptr = reinterpret_cast<float *>(in_tensors_[1]->Data());
  auto c_ptr = reinterpret_cast<float *>(out_tensors_[0]->Data());
  auto a_stride = params_->row_ * params_->deep_;
  auto b_stride = params_->deep_ * params_->col_;
  auto c_stride = params_->row_ * params_->col_;
  for (int i = 0; i < params_->batch; ++i) {
    auto cur_a_ptr = a_ptr + i * a_stride;
    auto cur_b_ptr = b_ptr + i * b_stride;
    c_r_ptr_ = c_ptr + i * c_stride;

    InitMatrixA(cur_a_ptr, a_c12_ptr_);
    InitMatrixB(cur_b_ptr, b_r8_ptr_);

    LiteBackendParallelLaunch(MatmulFloatRun, this, thread_count_);
  }
  return RET_OK;
}
}  // namespace mindspore::kernel
