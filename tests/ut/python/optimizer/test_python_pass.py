# Copyright 2020 Huawei Technologies Co., Ltd
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ============================================================================
import numpy as np

import mindspore
import mindspore.nn as nn
from mindspore import context
from mindspore.common.tensor import Tensor
from mindspore.ops import operations as P
from mindspore.graph_utils.python_pass import registe_pass, unregiste_pass, set_renorm, gen_new_parameter,\
    cancel_new_parameter
from mindspore.common.api import _generate_pip_args
from mindspore._c_expression import generate_key, Executor_
from mindspore.graph_utils.graph_pattern import IsIn, IsPrimTypeOf, CallWith, IsNot, AnyPattern, NewTensor,\
    NewParameter, Imm

context.set_context(mode=context.GRAPH_MODE)

def get_func_graph(obj, *args, phase="validate"):
    args_names, args_list = _generate_pip_args(obj, *args)
    dic = dict(zip(args_names, args_list))
    key = generate_key(phase, dic)
    phase_prefix = str(key[1])
    if phase == 'export':
        phase = phase + '.' + phase_prefix + '.' + str(obj.create_time)
    else:
        phase = phase_prefix + phase + '.' + str(obj.create_time)
    _executor = Executor_.get_instance()
    _executor.compile(obj, args_list, phase, False)
    return _executor.get_func_graph(phase)

def test_softmax_relu():
    """
    Use python pass to transform from Softmax to ReLU.
    """
    inputs = Tensor(np.ones([42]), mindspore.float16)
    softmax_model = nn.Softmax()

    @registe_pass(run_only_once=True)
    def softmax_relu_pass():
        x = AnyPattern()
        softmax_pattern = IsPrimTypeOf(P.Softmax())
        pattern = CallWith(softmax_pattern, inputs=[x])
        relu_pattern = IsPrimTypeOf(P.ReLU(), should_replace=False)
        target = CallWith(relu_pattern, inputs=[x])
        return pattern, target

    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(2)
    unregiste_pass(softmax_relu_pass)
    assert "ReLU" in transformed_repr
    assert "Softmax" not in transformed_repr

def test_softmax_relu_sigmoid():
    """
    Use python pass to transform from Softmax(x) to ReLU(Sigmoid(x)).

    NOTE:
        Sigmoid pattern only exists in the target.
    """
    inputs = Tensor(np.ones([42]), mindspore.float16)
    softmax_model = nn.Softmax()

    @registe_pass(run_only_once=True)
    def softmax_relu_pass():
        x = AnyPattern()
        softmax_pattern = IsPrimTypeOf(P.Softmax())
        pattern = CallWith(softmax_pattern, inputs=[x])
        sigmoid_pattern = IsPrimTypeOf(P.Sigmoid(), should_replace=False)
        call_sigmoid = CallWith(sigmoid_pattern, [x])
        relu_pattern = IsPrimTypeOf(P.ReLU(), should_replace=False)
        target = CallWith(relu_pattern, inputs=[call_sigmoid])
        return pattern, target

    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(3)
    unregiste_pass(softmax_relu_pass)
    assert "ReLU" in transformed_repr
    assert "Sigmoid" in transformed_repr
    assert "Softmax" not in transformed_repr


def test_isin_pattern_0():
    """
    Test IsIn pattern which expresses the IsIn/OneOf semantics.
    """
    inputs = Tensor(np.ones([42]), mindspore.float16)
    softmax_model = nn.Softmax()

    @registe_pass(run_only_once=True)
    def softmax_relu_pass():
        x = AnyPattern()
        softmax_pattern = IsPrimTypeOf(P.Softmax())
        call_softmax = CallWith(softmax_pattern, inputs=[x])
        relu_pattern = IsPrimTypeOf(P.ReLU())
        call_relu = CallWith(relu_pattern, inputs=[x])

        pattern = IsIn([call_softmax, call_relu])
        relu6_pattern = IsPrimTypeOf(P.ReLU6(), should_replace=False)
        target = CallWith(relu6_pattern, inputs=[x])
        return pattern, target
    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(2)
    unregiste_pass(softmax_relu_pass)
    assert "ReLU6" in transformed_repr
    assert "Softmax" not in transformed_repr

def test_isin_pattern_1():
    """
    Test IsIn. IsIn is used as nested inputs for the target in this case.
    """
    inputs = Tensor(np.ones([42]), mindspore.float16)
    softmax_model = nn.Softmax()

    @registe_pass(run_only_once=True)
    def softmax_neg_pass():
        x = AnyPattern()
        softmax_pattern = IsPrimTypeOf(P.Softmax())
        call_softmax = CallWith(softmax_pattern, inputs=[x])
        relu_pattern = IsPrimTypeOf(P.ReLU())
        call_relu = CallWith(relu_pattern, inputs=[x])

        pattern = IsIn([call_softmax, call_relu])
        neg_ops = IsPrimTypeOf(P.Neg(), should_replace=False)
        target = CallWith(neg_ops, inputs=[pattern])
        return pattern, target
    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(4)
    print(transformed_repr)
    unregiste_pass(softmax_neg_pass)
    assert "Neg" in transformed_repr
    assert "Softmax" in transformed_repr

def test_isnot_pattern_0():
    """
    Test IsNot pattern which expresses the IsNot semantics.
    Case: IsNot pass failed to match
    """
    set_renorm(False)
    class ConvBN(nn.Cell):
        def __init__(self):
            super(ConvBN, self).__init__()
            self.conv = P.Conv2D(32, 3)
            self.conv_weight = Tensor(np.ones([32, 32, 3, 3]), mindspore.float32)
            self.scale = Tensor(np.ones([32]), mindspore.float32)
            self.bias = Tensor(np.ones([32]), mindspore.float32)
            self.mean = Tensor(np.ones([32]), mindspore.float32)
            self.variance = Tensor(np.ones([32]), mindspore.float32)
            self.bn = P.BatchNorm()
        def construct(self, x):
            x = self.conv(x, self.conv_weight)
            x = self.bn(x, self.scale, self.bias, self.mean, self.variance)
            return x
    inputs = Tensor(np.random.normal(0, 1, (10, 32, 32, 32)), mindspore.float32)
    conv_bn_model = ConvBN()

    @registe_pass(run_only_once=True)
    def single_bn_pass():
        """
        Sub a BN which does NOT take Conv as inputs to ReLU6.
        """
        conv2d_prim = IsPrimTypeOf("Conv2D")
        conv2d = CallWith(conv2d_prim)
        pattern_0 = IsNot(conv2d)
        pattern = CallWith(P.BatchNorm(), inputs=[pattern_0])
        target = CallWith(P.ReLU6(), inputs=[pattern_0])
        return pattern, target

    @registe_pass(run_only_once=True)
    def bn_pass():
        """
        Sub a BN to Softmax.
        """
        bn = P.BatchNorm()
        pattern = CallWith(bn)
        softmax = P.Softmax()
        target = CallWith(softmax, should_replace=False)
        return pattern, target

    transformed_repr = get_func_graph(conv_bn_model, inputs).get_return().expanded_str(5)
    unregiste_pass(single_bn_pass)
    unregiste_pass(bn_pass)
    assert "ReLU6" not in transformed_repr
    assert "Softmax" in transformed_repr
    set_renorm(True)

def test_isnot_pattern_1():
    """
    Test IsNot pattern which expresses the IsNot semantics.
    Case: IsNot pattern matches with the graph
    """
    inputs = Tensor(np.ones([42]), mindspore.float16)
    softmax_model = nn.Softmax()

    @registe_pass(run_only_once=True)
    def single_bn_pass():
        """
        Sub a BN which does NOT take MatMul as inputs to ReLU6.
        """
        matmul = IsPrimTypeOf("MatMul")
        pattern_0 = IsNot(matmul)
        softmax = P.Softmax()
        pattern = CallWith(softmax, inputs=[pattern_0])
        relu6 = P.ReLU6()
        target = CallWith(relu6, inputs=[pattern_0], should_replace=False)
        return pattern, target

    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(5)
    unregiste_pass(single_bn_pass)
    assert "ReLU6" in transformed_repr
    assert "Softmax" not in transformed_repr

def test_newtensor_pattern():
    """
    Test NewTensor pattern in the target
    """
    set_renorm(False)
    inputs = Tensor(np.ones([42]), mindspore.float16)
    softmax_model = nn.Softmax()

    @registe_pass(run_only_once=True)
    def softmax_addn_pass():
        x = AnyPattern()
        softmax = P.Softmax()
        pattern = CallWith(softmax, inputs=[x])

        weight_tensor = Tensor(np.zeros([42]), mindspore.float16)
        new_weight = NewTensor(weight_tensor)
        addn_ops = P.AddN()
        target = CallWith(addn_ops, inputs=[x, new_weight], should_replace=False)
        return pattern, target
    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(2)
    unregiste_pass(softmax_addn_pass)
    assert "AddN" in transformed_repr
    assert "Softmax" not in transformed_repr
    set_renorm(True)

def test_newparameter_pattern():
    """
    Test NewParameter pattern in the target
    """
    inputs = Tensor(np.ones([42]), mindspore.float16)
    softmax_model = nn.Softmax()

    @registe_pass(run_only_once=True)
    def softmax_addn_pass():
        x = AnyPattern()
        softmax = P.Softmax()
        pattern = CallWith(softmax, inputs=[x])

        default_tensor0 = Tensor(np.ones((4, 4)), mindspore.float32)
        default_tensor1 = Tensor(np.ones((4, 4)), mindspore.float32)
        new_para_0 = NewParameter("Merlin", default_tensor0)
        new_para_1 = NewParameter("Arthur", default_tensor1)
        target_0 = CallWith(P.MatMul(), inputs=[new_para_0, new_para_1], should_replace=False)
        target = CallWith("make_tuple", inputs=[target_0], should_replace=False)
        return pattern, target
    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(5)
    print(transformed_repr)
    unregiste_pass(softmax_addn_pass)
    assert "MatMul" in transformed_repr
    assert "make_tuple" in transformed_repr
    assert "Softmax" not in transformed_repr

def test_imm_pattern():
    """
    Test NewParameter pattern in the target
    """
    inputs = Tensor(np.ones([42]), mindspore.float16)
    softmax_model = nn.Softmax()

    @registe_pass(run_only_once=True)
    def softmax_addn_pass():
        x = AnyPattern()
        softmax = P.Softmax()
        pattern = CallWith(softmax, inputs=[x])
        imm = Imm(0)
        target_0 = CallWith("make_tuple", inputs=[pattern], should_replace=False)
        target = CallWith("tuple_getitem", inputs=[target_0, imm], should_replace=False)
        return pattern, target
    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(5)
    print(transformed_repr)
    unregiste_pass(softmax_addn_pass)
    assert "make_tuple" in transformed_repr
    assert "tuple_getitem" in transformed_repr
    assert "Softmax" in transformed_repr

def test_gen_new_parameter():
    """
    Test gen_new_parameter
    """
    inputs = Tensor(np.ones([42]), mindspore.float16)
    softmax_model = nn.Softmax()

    default_tensor = Tensor(np.ones((4, 4)), mindspore.float32)
    new_para = NewParameter("Merlin", default_tensor, should_replace=True)
    gen_new_parameter(new_para)
    @registe_pass(run_only_once=True)
    def softmax_make_tuple_pass():
        x = AnyPattern()
        softmax = P.Softmax()
        pattern = CallWith(softmax, inputs=[x])

        target = CallWith("make_tuple", inputs=[pattern, new_para], should_replace=False)
        return pattern, target
    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(5)
    print(transformed_repr)
    assert "Merlin" in transformed_repr
    unregiste_pass(softmax_make_tuple_pass)
    cancel_new_parameter(new_para)
    transformed_repr = get_func_graph(softmax_model, inputs).get_return().expanded_str(5)
    print(transformed_repr)
    assert "Merlin" not in transformed_repr
