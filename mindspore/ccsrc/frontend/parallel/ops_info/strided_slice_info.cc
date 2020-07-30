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

#include "frontend/parallel/ops_info/strided_slice_info.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "frontend/parallel/device_matrix.h"
#include "frontend/parallel/strategy.h"
#include "frontend/parallel/tensor_layout/tensor_redistribution.h"
#include "pipeline/jit/resource.h"

namespace mindspore {
namespace parallel {
Status StridedSliceInfo::GetMask(const std::string &mask_name, int32_t *mask_value) {
  if (mask_value == nullptr) {
    return FAILED;
  }
  auto mask_iter = attrs_.find(mask_name);
  if (mask_iter != attrs_.end()) {
    MS_EXCEPTION_IF_NULL(mask_iter->second);
    if (mask_iter->second->isa<Int32Imm>()) {
      *mask_value = mask_iter->second->cast<Int32ImmPtr>()->value();
    } else {
      MS_LOG(ERROR) << name_ << ": The value of " << mask_name << " is not int";
      return FAILED;
    }
  }
  return SUCCESS;
}

Status GetInput(const ValuePtr &input_value, std::vector<int32_t> *input) {
  MS_EXCEPTION_IF_NULL(input_value);
  ValueTuplePtr value_tuple = input_value->cast<ValueTuplePtr>();
  if (value_tuple == nullptr) {
    MS_LOG(ERROR) << "Input value must be ValueTuplePtr.";
    return FAILED;
  }

  for (auto &element : value_tuple->value()) {
    MS_EXCEPTION_IF_NULL(element);
    if (element->isa<Int32Imm>()) {
      int32_t value = element->cast<Int32ImmPtr>()->value();
      input->push_back(value);
    } else {
      MS_LOG(ERROR) << "The value must be int32";
      return FAILED;
    }
  }

  return SUCCESS;
}

Status StridedSliceInfo::GetAttrs() {
  if (attrs_.size() < STRIDED_SLICE_ATTRS_SIZE) {
    MS_LOG(ERROR) << name_ << ": The size of attrs small than " << STRIDED_SLICE_ATTRS_SIZE;
    return FAILED;
  }

  if ((GetMask(BEGIN_MASK, &begin_mask_) != SUCCESS) || (GetMask(END_MASK, &end_mask_) != SUCCESS) ||
      (GetMask(ELLIPSIS_MASK, &ellipsis_mask_) != SUCCESS) || (GetMask(NEW_AXIS_MASK, &new_axis_mask_) != SUCCESS) ||
      (GetMask(SHRINK_AXIS_MASK, &shrink_axis_mask_) != SUCCESS)) {
    return FAILED;
  }
  has_mask_ = ((begin_mask_ != 0) || (end_mask_ != 0) || (ellipsis_mask_ != 0) || (new_axis_mask_ != 0) ||
               (shrink_axis_mask_ != 0));

  if (input_value_.size() != STRIDED_SLICE_INPUTS_SIZE) {
    MS_LOG(ERROR) << name_ << ": The size of input value must be " << STRIDED_SLICE_INPUTS_SIZE << ", but got "
                  << input_value_.size();
    return FAILED;
  }

  if ((GetInput(input_value_[STRIDED_SLICE_BEGIN_INDEX], &begin_) != SUCCESS) ||
      (GetInput(input_value_[STRIDED_SLICE_END_INDEX], &end_) != SUCCESS) ||
      (GetInput(input_value_[STRIDED_SLICE_STRIDES_INDEX], &strides_) != SUCCESS)) {
    return FAILED;
  }

  return SUCCESS;
}

Status StridedSliceInfo::CheckStrategy(const StrategyPtr &strategy) {
  MS_EXCEPTION_IF_NULL(strategy);
  if (CheckStrategyValue(strategy, inputs_shape_, is_auto_parallel_) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Invalid strategy";
    return FAILED;
  }

  std::vector<Dimensions> stra = strategy->GetInputDim();
  if (stra.empty()) {
    MS_LOG(ERROR) << name_ << ": The strategy is empty";
    return FAILED;
  }

  Dimensions strategy_value = stra[0];
  bool has_split = std::any_of(strategy_value.begin(), strategy_value.end(), [](int32_t v) { return v > 1; });

  if (has_split && has_mask_) {
    MS_LOG(ERROR) << name_ << ": When there is a mask, the input is not supported to be split";
    return FAILED;
  }

  if (strategy_value.size() < strides_.size()) {
    MS_LOG(ERROR) << name_ << ": The size of strategy must be larger or equal to the size of strides";
    return FAILED;
  }
  for (size_t i = 0; i < strides_.size(); ++i) {
    if ((strides_[i] != 1) && (strategy_value[i] > 1)) {
      MS_LOG(ERROR) << name_ << ": When a certain dimension is split, now does not support that the stride is not 1";
      return FAILED;
    }
  }

  if ((begin_.size() != end_.size()) || (begin_.size() != strides_.size())) {
    MS_LOG(ERROR) << name_ << ": The size of begin " << begin_.size() << ", end " << end_.size() << " and strides "
                  << strides_.size() << " must be equal";
    return FAILED;
  }

  for (size_t i = 0; i < begin_.size(); ++i) {
    bool no_fully_fetch = ((begin_[i] != 0) || (end_[i] < inputs_shape_[0][i]));
    if (no_fully_fetch && (strategy_value[i] != 1)) {
      MS_LOG(ERROR) << name_ << "When a dimension is not fully fetched, the dimension can not be split now";
      return FAILED;
    }
  }

  return SUCCESS;
}

Status StridedSliceInfo::InferDevMatrixShape() {
  MS_EXCEPTION_IF_NULL(strategy_);
  std::vector<Dimensions> stra = strategy_->GetInputDim();
  if (stra.empty()) {
    MS_LOG(ERROR) << name_ << "The strategy is empty";
    return FAILED;
  }

  dev_matrix_shape_ = stra[0];
  return SUCCESS;
}

Status StridedSliceInfo::InferTensorMap() {
  TensorMap tensor_map;
  if (inputs_shape_.empty()) {
    MS_LOG(ERROR) << name_ << "The inputs shape is empty";
    return FAILED;
  }

  // cannot use dev_matrix_shape_ replace inputs_shape_[0], because it may not be fully split in all devices.
  int32_t size = SizeToInt(inputs_shape_[0].size());
  for (int i = 0; i < size; ++i) {
    tensor_map.push_back(size - i - 1);
  }

  inputs_tensor_map_.push_back(tensor_map);
  outputs_tensor_map_.push_back(tensor_map);
  return SUCCESS;
}

Status StridedSliceInfo::InferMirrorOps() {
  mirror_ops_.clear();
  if (inputs_tensor_map_.empty()) {
    MS_LOG(ERROR) << name_ << ": The inputs tensor map is empty";
    return FAILED;
  }
  Shape input_tensor_map = inputs_tensor_map_[0];
  std::vector<Group> group;
  if (CreateGroupByTensorMap(input_tensor_map, &group) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Create group for input failed.";
    return FAILED;
  }

  if (group.empty()) {
    MS_LOG(INFO) << name_ << ": The mirror group is empty.";
    return SUCCESS;
  }

  OperatorVector input_op, begin_op, end_op, strides_op;
  input_op = CreateMirrorOps(group[0].name(), group[0].GetDevNum());
  mirror_ops_.push_back(input_op);
  mirror_ops_.push_back(begin_op);
  mirror_ops_.push_back(end_op);
  mirror_ops_.push_back(strides_op);
  return SUCCESS;
}

Status StridedSliceInfo::InferTensorInfo() {
  if (inputs_shape_.empty() || outputs_shape_.empty() || inputs_tensor_map_.empty() || outputs_tensor_map_.empty()) {
    MS_LOG(ERROR) << name_ << ": Invalid args";
    return FAILED;
  }
  // infer tensor layout
  TensorLayout input_layout, output_layout;
  if (input_layout.InitFromVector(dev_matrix_shape_, inputs_tensor_map_[0], inputs_shape_[0]) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Infer input tensor layout failed.";
    return FAILED;
  }
  if (output_layout.InitFromVector(dev_matrix_shape_, outputs_tensor_map_[0], outputs_shape_[0]) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Infer output tensor layout failed.";
    return FAILED;
  }

  TensorInfo input_tensor_info(input_layout);
  TensorInfo output_tensor_info(output_layout);

  inputs_tensor_info_.push_back(input_tensor_info);
  outputs_tensor_info_.push_back(output_tensor_info);
  return SUCCESS;
}

// Note: if the batch dimension is not fully fetched, the batch strategy may not work.
std::shared_ptr<std::vector<std::vector<int32_t>>> StridedSliceInfo::GenerateBatchStrategies() {
  split_flag_list_ = {true};
  return GenerateBatchStrategiesBySplitFlag(inputs_shape_, split_flag_list_);
}

Status StridedSliceInfo::SetCostUnderStrategy(const StrategyPtr &strategy) {
  if (SetCostUnderStrategyBase(strategy) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Set cost under strategy failed.";
    return FAILED;
  }

  return SUCCESS;
}

Status StridedSliceInfo::GenerateStrategies(int32_t stage_id) {
  if (InferAttrs() != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Infer attrs failed";
    return FAILED;
  }
  if (inputs_shape_.empty()) {
    MS_LOG(ERROR) << name_ << ": The inputs shape is empty";
    return FAILED;
  }
  Shape input_split(inputs_shape_[0].size(), 1);
  if (has_mask_) {
    for (size_t i = 0; i < inputs_shape_[0].size(); ++i) {
      input_split[i] = 0;
    }
  } else {
    for (size_t i = 0; i < begin_.size(); ++i) {
      bool no_fully_fetch = ((begin_[i] != 0) || (end_[i] < inputs_shape_[0][i]));
      if (no_fully_fetch || (strides_[i] != 1)) {
        input_split[i] = 0;
      }
    }
  }
  Shapes splittable_inputs = {input_split};

  std::vector<StrategyPtr> sp_vector;
  is_auto_parallel_ = true;
  if (GenerateStrategiesForIndependentInputs(stage_id, inputs_shape_, splittable_inputs, &sp_vector) != SUCCESS) {
    return FAILED;
  }

  size_t success = 0;
  for (auto &sp : sp_vector) {
    PrintStrategy(sp);
    if (SetCostUnderStrategy(sp) == SUCCESS) {
      success++;
      MS_LOG(INFO) << name_ << ": Successfully generated " << success << " strategy.";
      PrintStrategy(sp);
    }
  }
  return SUCCESS;
}

Status StridedSliceInfo::Init(const StrategyPtr &strategy) {
  if (InitWithAutoRepeatCalc(strategy) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Init failed.";
    return FAILED;
  }
  MS_LOG(INFO) << name_ << ": Init success.";
  return SUCCESS;
}

Status StridedSliceInfo::InitForCostModel(const StrategyPtr &strategy) {
  if (InitForCostModelWithAutoRepeatCalc(strategy) != SUCCESS) {
    MS_LOG(ERROR) << name_ << ": Init for cost model failed.";
    return FAILED;
  }

  MS_LOG(INFO) << name_ << ": Init for cost model success.";
  return SUCCESS;
}
}  // namespace parallel
}  // namespace mindspore