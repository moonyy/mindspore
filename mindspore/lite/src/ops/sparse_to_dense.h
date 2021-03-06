/**
 * Copyright 2019-2020 Huawei Technologies Co., Ltd
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

#ifndef LITE_MINDSPORE_LITE_C_OPS_SPARSE_TO_DENSE_H_
#define LITE_MINDSPORE_LITE_C_OPS_SPARSE_TO_DENSE_H_

#include <vector>
#include <set>
#include <cmath>
#include "ir/dtype/type_id.h"
#include "src/ops/primitive_c.h"

namespace mindspore {
namespace lite {
class SparseToDense : public PrimitiveC {
 public:
#ifdef PRIMITIVE_WRITEABLE
  MS_DECLARE_PARENT(SparseToDense, PrimitiveC);
  SparseToDense() = default;
  explicit SparseToDense(schema::PrimitiveT *primitive) : PrimitiveC(primitive) {}
  void SetOutputShape(const std::vector<int> &output_shape);
  void SetSparseValue(const std::vector<int> &sparse_value);
  void SetDefaultValue(const std::vector<int> &default_value);
  void SetValidateIndices(bool validate_indices);
#else
  explicit SparseToDense(schema::Primitive *primitive) : PrimitiveC(primitive) {}
#endif
  std::vector<int> GetOutputShape() const;
  std::vector<int> GetSparseValue() const;
  std::vector<int> GetDefaultValue() const;
  bool GetValidateIndices() const;
};
}  // namespace lite
}  // namespace mindspore

#endif  // LITE_MINDSPORE_LITE_C_OPS_SPARSE_TO_DENSE_H_
