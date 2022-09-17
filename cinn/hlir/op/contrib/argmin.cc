#include "cinn/hlir/op/contrib/argmin.h"

#include <iostream>
#include <vector>

#include "cinn/hlir/framework/node.h"
#include "cinn/hlir/framework/op.h"
#include "cinn/hlir/framework/op_strategy.h"
#include "cinn/hlir/op/contrib/sort.h"
#include "cinn/hlir/pe/broadcast.h"
#include "cinn/hlir/pe/ir_schedule_pe.h"
#include "cinn/hlir/pe/schedule.h"
#include "cinn/hlir/pe/transform.h"
#include "cinn/ir/ir_operators.h"
#include "cinn/ir/ir_schedule.h"

DECLARE_bool(cinn_ir_schedule);

namespace cinn {
namespace hlir {
namespace op {

using common::CINNValue;
using framework::shape_t;
using ir::Tensor;

Tensor Argmin(const Tensor &in_tensor, const int &axis, const bool keep_dims, const std::string &output_name) {
  auto shape = in_tensor->shape;
  auto ndim  = shape.size();
  CHECK_GT(ndim, 0) << "tensor's dim must be more than 0";

  int real_axis = axis;
  if (axis < 0) {
    real_axis = static_cast<int>(ndim) + axis;
  }
  CHECK_LT(real_axis, ndim) << "Axis must be less than tensor's dim";
  CHECK_GE(real_axis, 0) << "Axis must be more than 0";

  std::vector<Expr> output_shape;
  for (int i = 0; i < shape.size(); ++i) {
    CHECK(shape[i].is_constant()) << "Input tensor's shape should be constant value.";
    if (axis == i) {
      if (keep_dims) {
        output_shape.push_back(Expr(1));
      }
    } else {
      output_shape.push_back(shape[i]);
    }
  }
  if (output_shape.empty()) {
    output_shape.push_back(Expr(1));
  }

  std::string extern_fun_name;
  if (target.arch == common::Target::Arch::NVGPU) {
    extern_fun_name.assign("cinn_cuda_");
  } else if (target.arch == common::Target::Arch::X86) {
    extern_fun_name.assign("cinn_host_");
  } else {
    LOG(FATAL) << "Argmin only supports X86 and NVGPU ! Please Check.\n";
  }
  if (true) {
    extern_fun_name.append("lt_num_float");
  } else {
    extern_fun_name.append("gt_num_float");
  }

  auto res = Compute(
      output_shape,
      [=](const std::vector<Expr> &indices) {
        std::vector<Expr> eval_indices(indices);
        if (!keep_dims) {
          eval_indices.insert(eval_indices.begin() + real_axis, Expr(1));
        }
        Expr offset(0);
        Expr stride(1);
        for (int i = 0; i < indices.size(); i++) {
          if (i < pos_axis) {
            offset = offset * A->shape[i] + indices[i];
          } else if (i == pos_axis) {
            offset = offset * A->shape[i];
          } else {
            offset = offset * A->shape[i] + indices[i];
            stride = stride * A->shape[i];
          }
        }
        offset            = common::AutoSimplify(offset);
        stride            = common::AutoSimplify(stride);
        auto A_shape_axis = A->shape[pos_axis];
        return lang::CallExtern(extern_fun_name, {A, A_shape_axis, A(indices), offset, stride});
      },
      name);
  return res;

  auto compute = [=](const std::vector<Expr> &indices) -> Expr {
    std::vector<Expr> eval_indices(indices);
    if (!keep_dims) {
      eval_indices.insert(eval_indices.begin() + real_axis, Expr(1));
    }

    //    Var loop_var("k0");
    //    eval_indices[real_axis] = i;
    //    auto value              = in_tensor(eval_indices);
    //    auto update             = ir::LT::Make(value, current[1]);
    //    auto c1                 = ir::Select::Make(update, Expr(i), current[0]);
    //    auto c2                 = ir::Select::Make(update, value, current[1]);
    //    current[0]              = c1;
    //    current[1]              = c2;
    //    auto for_loop           = ir::For::Make(i, Expr(0), current[0]);

    Placeholder<float> p_min_value("min_value", {shape[real_axis] + 1});
    Placeholder<int32_t> p_min_index("min_index", {shape[real_axis] + 1});
    auto min_value = ir::Tensor(p_min_value);
    auto min_index = ir::Tensor(p_min_index);

    Var loop_var("k0", Int(32));
    Expr loop_expr          = Expr(loop_var);
    eval_indices[real_axis] = loop_expr;

    auto value = lang::Identity(in_tensor(eval_indices));
    CHECK_EQ(min_value->type(), Float(32));
    //    ir::Store::Make(min_value, Expr(-3.402823e+38f), {Expr(int32_t(0))});

    //    auto update = ir::GT::Make(value, Expr(0));
    auto update = ir::GT::Make(value, ir::Load::Make(min_value, {loop_expr}));
    CHECK_EQ(min_index->type(), Int(32));
    auto c_v = ir::Select::Make(update, value, ir::Load::Make(min_value, {loop_expr}));
    auto c_i = ir::Select::Make(update, loop_expr, ir::Load::Make(min_index, {loop_expr}));

    Expr init  = ir::Store::Make(min_value, Expr(-3.402823e+38f), {Expr(int32_t(0))});
    Expr body1 = ir::Store::Make(min_value, c_v, {loop_expr + 1});
    Expr body2 = ir::Store::Make(min_index, c_i, {loop_expr + 1});

    Expr body = ir::Block::Make({init, body1, body2});

    auto output = ir::For::Make(
        loop_var, common::make_const(0), shape[real_axis], ir::ForType::Serial, ir::DeviceAPI::Host, body);

    return ir::Load::Make(output, {shape[real_axis]});
  };

  Tensor res = Compute(output_shape, compute, output_name);
  return res;
}

std::shared_ptr<framework::OpStrategy> StrategyForArgmin(const framework::NodeAttr &attrs,
                                                         const std::vector<Tensor> &inputs,
                                                         const std::vector<Type> &out_type,
                                                         const std::vector<std::vector<int>> &output_shapes,
                                                         const Target &target) {
  int axis;
  bool keep_dims = false;

  if (attrs.attr_store.count("axis")) {
    axis = absl::get<int>(attrs.attr_store.at("axis"));
  } else {
    LOG(FATAL) << "reduce dimension is not set!";
  }
  if (attrs.attr_store.count("keep_dim")) {
    keep_dims = absl::get<bool>(attrs.attr_store.at("keep_dim"));
  }

  framework::CINNCompute argmin_compute([=](lang::Args args, lang::RetValue *ret) {
    CHECK(!args.empty()) << "The input argument of argmin compute is empty! Please check.";
    common::CINNValuePack arg_packs = args[0];
    std::string tensor_name         = UniqName("Argmin_out");
    CHECK_EQ(arg_packs.size(), 1U) << "There should be 1 input args for argmax compute";
    Expr in_expr = arg_packs[0];
    CHECK(in_expr.as_tensor());
    Tensor in_tensor = in_expr.as_tensor_ref();
    auto stages      = CreateStages({in_tensor});
    auto out_tensor  = Argmin(in_tensor, axis, keep_dims, tensor_name);

    stages->InsertLazily(out_tensor);
    std::vector<CINNValue> cinn_values{CINNValue(out_tensor), CINNValue(stages)};
    *ret = common::CINNValuePack{cinn_values};
  });

  framework::CINNSchedule argmin_schedule([=](lang::Args args, lang::RetValue *ret) {
    CHECK(!args.empty()) << "The input argument of argmin schedule is empty! Please check.";
    common::CINNValuePack arg_pack = args[0];
    CHECK_EQ(arg_pack.size(), 2UL);
    Expr out = arg_pack[0];
    CHECK(out.as_tensor());

    // When develop FLAGS_cinn_ir_schedule=true case, we should run unit test with
    // FLAGS_cinn_ir_schedule=1
    if (FLAGS_cinn_ir_schedule) {
      *ret = common::CINNValuePack{{common::CINNValue(out)}};
    } else {
      poly::StageMap stages = arg_pack[arg_pack.size() - 1];
      *ret                  = common::CINNValuePack{{common::CINNValue(out), common::CINNValue(stages)}};
    }
  });

  auto strategy = std::make_shared<framework::OpStrategy>();
  strategy->AddImpl(argmin_compute, argmin_schedule, "strategy.argmin.x86", 1);

  return strategy;
}

std::vector<shape_t> InferShapeForArgmin(const std::vector<shape_t> &inputs_shape,
                                         const framework::AttrMapType &attrs) {
  CHECK(inputs_shape.size() == 1UL);
  auto ndim = inputs_shape[0].size();
  CHECK_GT(ndim, 0) << "tensor's dim must be more than 0";
  int axis;
  bool keep_dim;

  CHECK(attrs.find("axis") != attrs.end());
  axis = absl::get<int>(attrs.at("axis"));
  if (axis < 0) {
    axis = static_cast<int>(ndim) + axis;
  }
  CHECK_LT(axis, ndim) << "Axis must be less than tensor's dim";
  CHECK_GE(axis, 0) << "Axis must be more than 0";

  CHECK(attrs.find("keep_dim") != attrs.end());
  keep_dim = absl::get<bool>(attrs.at("keep_dim"));

  std::vector<int> out_shapes;
  for (size_t i = 0; i < ndim; ++i) {
    if (axis == i) {
      if (keep_dim) {
        out_shapes.push_back(1);
      }
    } else {
      out_shapes.push_back(inputs_shape[0][i]);
    }
  }

  if (keep_dim) {
    CHECK_EQ(ndim, out_shapes.size());
  } else {
    CHECK_EQ(ndim - 1, out_shapes.size());
  }

  if (out_shapes.empty()) {
    out_shapes.push_back(1);
  }

  return {out_shapes};
}

std::vector<Type> InferDtypeForArgmin(const std::vector<Type> &inputs_type, const framework::AttrMapType &attrs) {
  CHECK(!inputs_type.empty()) << "The input's type size is 0! Please check again.";
  return {Int(32)};
}

std::vector<std::vector<std::string>> InferLayoutForArgmin(const std::vector<framework::shape_t> &input_shapes,
                                                           const std::vector<std::string> &input_layouts,
                                                           const framework::NodeAttr &attrs,
                                                           const Target &target) {
  CHECK_EQ(input_shapes.size(), 1U) << "The input's shape size is not 1! Please check again.";
  CHECK_EQ(input_layouts.size(), 1U) << "The input's layout size is not 1! Please check again.";
  return {input_layouts, input_layouts};
}
}  // namespace op
}  // namespace hlir
}  // namespace cinn

CINN_REGISTER_HELPER(argmin_ops) {
  CINN_REGISTER_OP(argmin)
      .describe("This operator implements the op argmin.")
      .set_num_inputs(1)
      .set_num_outputs(1)
      .set_attr<cinn::hlir::framework::StrategyFunction>("CINNStrategy", cinn::hlir::op::StrategyForArgmin)
      .set_attr("infershape", MakeOpFunction(cinn::hlir::op::InferShapeForArgmin))
      .set_attr("inferdtype", MakeOpFunction(cinn::hlir::op::InferDtypeForArgmin))
      .set_support_level(4);

  return true;
}
