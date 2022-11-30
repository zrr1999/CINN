// Copyright (c) 2022 CINN Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cinn/common/cas.h"
#include "cinn/common/common.h"
#include "cinn/common/context.h"
#include "cinn/common/macros.h"
#include "cinn/common/target.h"
#include "cinn/hlir/framework/node.h"
#include "cinn/hlir/framework/op.h"
#include "cinn/hlir/framework/op_strategy.h"
#include "cinn/hlir/pe/ir_schedule_pe.h"
#include "cinn/hlir/pe/nn.h"
#include "cinn/hlir/pe/schedule.h"
#include "cinn/ir/ir.h"
#include "cinn/ir/ir_base.h"
#include "cinn/ir/ir_operators.h"
#include "cinn/ir/tensor.h"
#include "cinn/lang/builtin.h"
#include "cinn/lang/compute.h"
#include "gflags/gflags.h"

DECLARE_bool(cinn_ir_schedule);

namespace cinn {
namespace hlir {
namespace op {

using common::_CINNValuePack_;
using common::CINNValue;
using common::CINNValuePack;
using framework::OpStrategy;
using framework::shape_t;
using framework::StrategyFunction;

ir::Tensor Reciprocal(const ir::Tensor &input, const std::string &output_name) {
  std::string extern_func = "cinn_";

  extern_func += "reciprocal";

  if (input->type().is_float(32)) {
    extern_func += "_fp32";
  } else if (input->type().is_float(64)) {
    extern_func += "_fp64";
  } else if (input->type().is_float(16)) {
    extern_func += "_fp16";
  } else {
    CINN_NOT_IMPLEMENTED
  }

  return {Compute(
      input->shape,
      [=](const std::vector<Expr> &indice) {
        ir::Tensor out_tensor(input);
        auto e = out_tensor(indice);
        return common::make_const(input->type(), 1.0f) / e;
      },
      output_name)};
}

std::shared_ptr<OpStrategy> StrategyForReciprocal(const framework::NodeAttr &attrs,
                                                  const std::vector<ir::Tensor> &inputs,
                                                  const std::vector<Type> &out_type,
                                                  const std::vector<std::vector<int>> &output_shapes,
                                                  const Target &target) {
  std::string op_name("reciprocal");

  framework::CINNCompute reciprocal_compute([=](lang::Args args, lang::RetValue *ret) {
    CHECK(!args.empty()) << "The input argument of " << op_name << " compute is empty! Please check.\n";
    CINNValuePack pack_args = args[0];
    CHECK(!pack_args.empty()) << "at least one input tensor for " << op_name << " compute\n";

    std::string tensor_name = UniqName("Reciprocal_out");

    if (FLAGS_cinn_ir_schedule) {
      CHECK_EQ(pack_args.size(), 2);
      CHECK(pack_args[1].is_string());
      tensor_name = pack_args[1].operator std::string();
    }

    Expr A = pack_args[0];
    CHECK(A.as_tensor());
    CHECK(!output_shapes.empty());
    auto tensor_A = A.as_tensor_ref();
    auto stages   = CreateStages({tensor_A});
    VLOG(3) << "A shape: " << utils::Join(tensor_A->shape, ", ")
            << ", output_shapes: " << utils::Join(output_shapes[0], ", ");

    if (FLAGS_cinn_ir_schedule) {
      CHECK_EQ(pack_args.size(), 2U);
      tensor_name = pack_args[1].operator std::string();
    }

    ir::Tensor out = Reciprocal(tensor_A, tensor_name);
    std::vector<CINNValue> res;
    stages->InsertLazily(out);
    res.push_back(CINNValue(out));
    CHECK(!out_type.empty()) << "Output type of Reciprocal is empty! Please check.\n";
    res.push_back(CINNValue(stages));
    *ret = CINNValuePack{res};
  });

  auto strategy = std::make_shared<framework::OpStrategy>();
  strategy->AddImpl(
      reciprocal_compute, framework::GetInjectiveScheduleFunc(output_shapes, target), "strategy.reciprocal.x86", 1);
  return strategy;
}

std::vector<framework::shape_t> InferShapeForReciprocal(const std::vector<framework::shape_t> &inputs_shape,
                                                        const framework::AttrMapType &attrs) {
  CHECK(!inputs_shape.empty() && !inputs_shape[0].empty()) << "The input's shape size is 0! Please check again.";
  std::vector<framework::shape_t> res{inputs_shape[0]};
  return res;
}

std::vector<Type> InferDtypeForReciprocal(const std::vector<Type> &inputs_type, const framework::AttrMapType &attrs) {
  CHECK(!inputs_type.empty()) << "The input's type size is 0! Please check again.";
  std::vector<Type> res{inputs_type[0]};
  return res;
}

}  // namespace op
}  // namespace hlir
}  // namespace cinn

CINN_REGISTER_HELPER(reciprocal_ops) {
  CINN_REGISTER_OP(reciprocal)
      .describe("Counting Leading Zeros.")
      .set_num_inputs(1)
      .set_num_outputs(1)
      .set_attr<cinn::hlir::framework::StrategyFunction>("CINNStrategy", cinn::hlir::op::StrategyForReciprocal)
      .set_attr("infershape", MakeOpFunction(cinn::hlir::op::InferShapeForReciprocal))
      .set_attr("inferdtype", MakeOpFunction(cinn::hlir::op::InferDtypeForReciprocal))
      .set_attr<cinn::hlir::framework::OpPatternKind>("OpPattern", cinn::hlir::framework::OpPatternKind::kElementWise)
      .set_support_level(4);

  return true;
}