/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/lite/delegates/gpu/common/tasks/special/depthwise_conv_plus_1x1_conv.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_replace.h"
#include "tensorflow/lite/delegates/gpu/common/flops_util.h"
#include "tensorflow/lite/delegates/gpu/common/task/util.h"
#include "tensorflow/lite/delegates/gpu/common/tasks/relu.h"
#include "tensorflow/lite/delegates/gpu/common/util.h"

namespace tflite {
namespace gpu {
namespace {
void UploadWeights(const DepthwiseConvolution2DAttributes& dw_attr,
                   const Convolution2DAttributes& conv_attr,
                   const GpuInfo& gpu_info, CalculationsPrecision precision,
                   GPUOperation* op) {
  int dw_dst_ch_aligned = AlignByN(dw_attr.weights.shape.i, 4);
  int dw_weights_count =
      dw_dst_ch_aligned * dw_attr.weights.shape.h * dw_attr.weights.shape.w;
  int conv_src_ch_aligned = AlignByN(conv_attr.weights.shape.i, 4);
  int conv_dst_ch_aligned = AlignByN(conv_attr.weights.shape.o, 4);
  int conv_weights_count = conv_src_ch_aligned * conv_dst_ch_aligned;
  std::vector<float> gpu_data;
  gpu_data.reserve(dw_dst_ch_aligned + dw_weights_count + conv_dst_ch_aligned +
                   conv_weights_count);
  // dw bias loading
  for (int i = 0; i < dw_dst_ch_aligned; ++i) {
    if (i < dw_attr.bias.shape.v) {
      gpu_data.push_back(dw_attr.bias.data[i]);
    } else {
      gpu_data.push_back(0.0f);
    }
  }
  // dw weights loading
  for (int d = 0; d < dw_dst_ch_aligned / 4; ++d) {
    for (int y = 0; y < dw_attr.weights.shape.h; ++y) {
      for (int x = 0; x < dw_attr.weights.shape.w; ++x) {
        for (int i = 0; i < 4; ++i) {
          const int d_ch = d * 4 + i;
          if (d_ch < dw_attr.weights.shape.i) {
            const int f_index =
                dw_attr.weights.shape.LinearIndex({0, y, x, d_ch});
            gpu_data.push_back(dw_attr.weights.data[f_index]);
          } else {
            gpu_data.push_back(0.0f);
          }
        }
      }
    }
  }
  // conv bias loading
  for (int i = 0; i < conv_dst_ch_aligned; ++i) {
    if (i < conv_attr.bias.shape.v) {
      gpu_data.push_back(conv_attr.bias.data[i]);
    } else {
      gpu_data.push_back(0.0f);
    }
  }
  // conv weights loading
  for (int d = 0; d < conv_dst_ch_aligned / 4; ++d) {
    for (int s = 0; s < conv_src_ch_aligned / 4; ++s) {
      for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
          const int s_ch = s * 4 + j;
          const int d_ch = d * 4 + i;
          if (s_ch < conv_attr.weights.shape.i &&
              d_ch < conv_attr.weights.shape.o) {
            const int f_index =
                conv_attr.weights.shape.LinearIndex({d_ch, 0, 0, s_ch});
            gpu_data.push_back(conv_attr.weights.data[f_index]);
          } else {
            gpu_data.push_back(0.0f);
          }
        }
      }
    }
  }

  const bool fp32_weights = precision == CalculationsPrecision::F32;
  const int float_size = fp32_weights ? 4 : 2;
  BufferDescriptor desc;
  desc.element_type = fp32_weights ? DataType::FLOAT32 : DataType::FLOAT16;
  desc.element_size = 4;
  desc.memory_type = gpu_info.IsMali() || gpu_info.IsAMD()
                         ? MemoryType::GLOBAL
                         : MemoryType::CONSTANT;
  desc.size = float_size * gpu_data.size();
  desc.data.resize(desc.size);

  if (fp32_weights) {
    memcpy(desc.data.data(), gpu_data.data(), desc.size);
  } else {
    half* gpu_data_half = reinterpret_cast<half*>(desc.data.data());
    for (int i = 0; i < gpu_data.size(); ++i) {
      gpu_data_half[i] = gpu_data[i];
    }
  }
  op->args_.AddObject("constants",
                      std::make_unique<BufferDescriptor>(std::move(desc)));
}

std::string MultiplyAccumulate(const GpuInfo& gpu_info,
                               const std::string& accum, const std::string& a,
                               const std::string& b) {
  const bool use_fma = gpu_info.IsAMD() && gpu_info.IsApiOpenCl();
  if (use_fma) {
    return accum + " = fma(" + a + ", " + b + ", " + accum + ")";
  } else {
    return accum + " += " + a + " * " + b;
  }
}

std::string GenerateCode(const OperationDef& op_def, const GpuInfo& gpu_info,
                         const DepthwiseConvolution2DAttributes& dw_attr,
                         ReLUAttributes* relu_attr_ptr, int result_depth,
                         GPUOperation* result) {
  auto src_desc = op_def.src_tensors[0];
  result->AddSrcTensor("src_tensor", src_desc);
  result->AddDstTensor("dst_tensor", op_def.dst_tensors[0]);

  result->args_.AddInt("stride_x", dw_attr.strides.w);
  result->args_.AddInt("padding_x", -dw_attr.padding.prepended.w);
  result->args_.AddInt("dilation_x", dw_attr.dilations.w);
  result->args_.AddInt("stride_y", dw_attr.strides.h);
  result->args_.AddInt("padding_y", -dw_attr.padding.prepended.h);
  result->args_.AddInt("dilation_y", dw_attr.dilations.h);

  std::string c;
  c += "MAIN_FUNCTION($0) {\n";
  if (op_def.dst_tensors[0].HasAxis(Axis::BATCH)) {
    c += "  int linear_id = GLOBAL_ID_0;\n";
    c += "  int X = linear_id / args.dst_tensor.Batch();\n";
    c += "  int B = linear_id % args.dst_tensor.Batch();\n";
    c += "  args.dst_tensor.SetBatchRef(B);\n";
    c += "  args.src_tensor.SetBatchRef(B);\n";
  } else {
    c += "  int X = GLOBAL_ID_0;\n";
  }
  c += "  int Y = GLOBAL_ID_1;\n";
  c += "  if (X >= args.dst_tensor.Width() || Y >= args.dst_tensor.Height()) { "
       "\n";
  c += "    return; \n";
  c += "  } \n";
  int intermediate_depth = DivideRoundUp(dw_attr.weights.shape.i, 4);
  int weights_counter = 0;
  for (int d = 0; d < intermediate_depth; ++d) {
    c += "  FLT4 dw_res_" + std::to_string(d) + " = args.constants.Read(" +
         std::to_string(weights_counter++) + ");\n";
  }
  c += "  int x_offseted = X * args.stride_x + args.padding_x;\n";
  c += "  int y_offseted = Y * args.stride_y + args.padding_y;\n";
  c += "  int x_c, y_c;\n";

  auto generate_check = [&]() {
    std::string check;
    const std::vector<Axis> axes{Axis::WIDTH, Axis::HEIGHT, Axis::DEPTH};
    const std::vector<std::string> names{"x_in", "y_in", "z_in"};
    for (int i = 0; i < axes.size(); ++i) {
      const auto& axis = axes[i];
      if (src_desc.HasAxis(axis) &&
          !src_desc.SupportsZeroClamp(axis, gpu_info)) {
        if (!check.empty()) {
          check += " && ";
        }
        check += names[i];
      }
    }
    return check;
  };
  const std::string check = generate_check();
  if (!src_desc.SupportsZeroClamp(Axis::HEIGHT, gpu_info)) {
    c += "  bool y_in;\n";
  }
  if (!src_desc.SupportsZeroClamp(Axis::WIDTH, gpu_info)) {
    c += "  bool x_in;\n";
  }

  const std::string postfixes[] = {".x", ".xy", ".xyz", ""};
  c += "  FLT4 src;\n";
  for (int d = 0; d < intermediate_depth; ++d) {
    const int src_ch_count = std::min(4, dw_attr.weights.shape.i - d * 4);
    const std::string s_postfix = postfixes[src_ch_count - 1];
    for (int ky = 0; ky < dw_attr.weights.shape.h; ++ky) {
      c += "  y_c = y_offseted + " + std::to_string(ky) +
           " * args.dilation_y;\n";
      if (!src_desc.SupportsZeroClamp(Axis::HEIGHT, gpu_info)) {
        c += "  y_in = y_c >= 0 && y_c < args.src_tensor.Height();\n";
        c += "  y_c = clamp(y_c, 0, args.src_tensor.Height() - 1);\n";
      }
      for (int kx = 0; kx < dw_attr.weights.shape.w; ++kx) {
        c += "  x_c = x_offseted + " + std::to_string(kx) +
             " * args.dilation_x;\n";
        if (!src_desc.SupportsZeroClamp(Axis::WIDTH, gpu_info)) {
          c += "  x_in = x_c >= 0 && x_c < args.src_tensor.Width();\n";
          c += "  x_c = clamp(x_c, 0, args.src_tensor.Width() - 1);\n";
        }
        std::string multiplier =
            check.empty() ? "" : " * INIT_FLT(" + check + ")";
        c += "  src" + s_postfix + " = args.src_tensor.Read(x_c, y_c, " +
             std::to_string(d) + ")" + s_postfix + multiplier + ";\n";
        c += "  " +
             MultiplyAccumulate(
                 gpu_info, "dw_res_" + std::to_string(d) + s_postfix,
                 "src" + s_postfix,
                 "args.constants.Read(" + std::to_string(weights_counter++) +
                     ")" + s_postfix) +
             ";\n";
      }
    }
  }
  if (relu_attr_ptr) {
    std::string elementwise_code;
    CreateReLU(*relu_attr_ptr, op_def.precision, &result->args_,
               &elementwise_code);
    for (int d = 0; d < intermediate_depth; ++d) {
      const std::string var_name = "dw_res_" + std::to_string(d);
      const std::string elementwise_new_code = absl::StrReplaceAll(
          elementwise_code, {{"in_value", var_name}, {"out_value", var_name}});
      c += "  {  " + elementwise_new_code + "  }\n";
    }
  }
  for (int d = 0; d < result_depth; ++d) {
    c += "  FLT4 conv_res_" + std::to_string(d) + " = args.constants.Read(" +
         std::to_string(weights_counter++) + ");\n";
  }
  for (int d = 0; d < result_depth; ++d) {
    for (int s = 0; s < intermediate_depth; ++s) {
      std::string src = "dw_res_" + std::to_string(s);
      std::string dst = "conv_res_" + std::to_string(d);
      const std::string c0 =
          "args.constants.Read(" + std::to_string(weights_counter++) + ")";
      const std::string c1 =
          "args.constants.Read(" + std::to_string(weights_counter++) + ")";
      const std::string c2 =
          "args.constants.Read(" + std::to_string(weights_counter++) + ")";
      const std::string c3 =
          "args.constants.Read(" + std::to_string(weights_counter++) + ")";
      c += "  " + MultiplyAccumulate(gpu_info, dst, c0, src + ".x") + ";\n";
      c += "  " + MultiplyAccumulate(gpu_info, dst, c1, src + ".y") + ";\n";
      c += "  " + MultiplyAccumulate(gpu_info, dst, c2, src + ".z") + ";\n";
      c += "  " + MultiplyAccumulate(gpu_info, dst, c3, src + ".w") + ";\n";
    }
    c += "  args.dst_tensor.Write(conv_res_" + std::to_string(d) + ", X, Y, " +
         std::to_string(d) + ");\n";
  }
  c += "}\n";

  return c;
}

bool IsDepthwiseConvPlus1x1ConvSupported(
    const OperationDef& definition, const GpuInfo& gpu_info,
    const DepthwiseConvolution2DAttributes& dw_attr,
    const Convolution2DAttributes& conv_attr, const BHWC* dst_shape) {
  const auto dw_shape = dw_attr.weights.shape;
  const auto conv_shape = conv_attr.weights.shape;
  bool good_dw = dw_shape.o == 1;
  bool good_conv =
      conv_shape.w == 1 && conv_shape.h == 1 && conv_attr.dilations.w == 1 &&
      conv_attr.dilations.h == 1 && conv_attr.strides.w == 1 &&
      conv_attr.strides.h == 1 && conv_attr.padding.prepended.w == 0 &&
      conv_attr.padding.prepended.h == 0 && conv_attr.padding.appended.w == 0 &&
      conv_attr.padding.appended.h == 0;
  if (gpu_info.IsApple()) {
    if (definition.precision == CalculationsPrecision::F16) {
      bool recommended_dw = dw_shape.i <= 16 &&
                            dw_shape.i * dw_shape.h * dw_shape.w <= 3 * 3 * 16;
      bool recommended_conv =
          conv_shape.o <= 16 && conv_shape.i * conv_shape.o <= 16 * 16;
      return good_dw && good_conv && recommended_dw && recommended_conv;
    } else {
      bool recommended_dw = dw_shape.i <= 16 &&
                            dw_shape.i * dw_shape.h * dw_shape.w <= 3 * 3 * 16;
      bool recommended_conv =
          conv_shape.o <= 8 && conv_shape.i * conv_shape.o <= 8 * 16;
      return good_dw && good_conv && recommended_dw && recommended_conv;
    }
  } else if (gpu_info.IsMali()) {
    if (gpu_info.mali_info.IsMidgard()) {
      return false;
    }
    if (dst_shape) {
      const int dst_slices = DivideRoundUp(dst_shape->c, 4);
      int task_size = dst_shape->b * dst_shape->h * dst_shape->w * dst_slices;
      int block_size = GetRecommendedBlockSizeForConv(
          gpu_info, definition.precision, task_size);
      if (block_size < 4 && dst_slices >= 2) {
        return false;
      }
      if (block_size < 2 && dst_slices >= 4) {
        return false;
      }
    }
    if (definition.precision == CalculationsPrecision::F16 &&
        definition.src_tensors[0].SupportsZeroClamp(Axis::WIDTH, gpu_info) &&
        definition.src_tensors[0].SupportsZeroClamp(Axis::HEIGHT, gpu_info)) {
      bool recommended_dw = dw_shape.i <= 16 &&
                            dw_shape.i * dw_shape.h * dw_shape.w <= 3 * 3 * 16;
      bool recommended_conv =
          conv_shape.o <= 16 && conv_shape.i * conv_shape.o <= 16 * 16;
      return good_dw && good_conv && recommended_dw && recommended_conv;
    } else {
      return false;
    }
  } else {
    if (definition.precision == CalculationsPrecision::F16) {
      bool recommended_dw = dw_shape.i <= 32 &&
                            dw_shape.i * dw_shape.h * dw_shape.w <= 3 * 3 * 32;
      bool recommended_conv =
          conv_shape.o <= 32 && conv_shape.i * conv_shape.o <= 32 * 32;
      return good_dw && good_conv && recommended_dw && recommended_conv;
    } else {
      bool recommended_dw = dw_shape.i <= 16 &&
                            dw_shape.i * dw_shape.h * dw_shape.w <= 3 * 3 * 16;
      bool recommended_conv =
          conv_shape.o <= 32 && conv_shape.i * conv_shape.o <= 16 * 32;
      return good_dw && good_conv && recommended_dw && recommended_conv;
    }
  }
}

}  // namespace

GPUOperation CreateDepthwiseConvPlus1x1Conv(
    const OperationDef& definition, const GpuInfo& gpu_info,
    const DepthwiseConvolution2DAttributes& dw_attr,
    const Convolution2DAttributes& conv_attr, ReLUAttributes* relu_attr_ptr) {
  GPUOperation result(definition);
  result.code_ =
      GenerateCode(definition, gpu_info, dw_attr, relu_attr_ptr,
                   DivideRoundUp(conv_attr.weights.shape.o, 4), &result);
  result.tensor_to_grid_ = TensorToGrid::kWBToX_HDToY_ZIs1;
  if (gpu_info.IsMali()) {
    result.compiler_options_.push_back(CompilerOptions::kClFastRelaxedMath);
  }
  UploadWeights(dw_attr, conv_attr, gpu_info, definition.precision, &result);
  return result;
}

absl::Status TryDepthwiseConvPlus1x1Conv(
    const GpuInfo& gpu_info, CalculationsPrecision precision,
    const GraphFloat32& graph, NodeId first_node_id,
    const std::map<ValueId, TensorDescriptor>& tensor_descriptors,
    std::set<NodeId>* consumed_nodes, GPUOperationsSubgraph* gpu_subgraph) {
  if (!(gpu_info.IsAdreno() || gpu_info.IsNvidia() || gpu_info.IsMali() ||
        gpu_info.IsApple() || gpu_info.IsAMD())) {
    return absl::NotFoundError("FCFCAdd not suitable.");
  }
  auto* dw_node = graph.GetNode(first_node_id);
  if (dw_node == nullptr) {
    return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
  }
  if (OperationTypeFromString(dw_node->operation.type) !=
      OperationType::DEPTHWISE_CONVOLUTION) {
    return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
  }
  auto dw_inputs = graph.FindInputs(dw_node->id);
  if (dw_inputs.size() != 1) {
    return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
  }
  auto dw_outputs = graph.FindOutputs(dw_node->id);
  auto consumers = graph.FindConsumers(dw_outputs[0]->id);
  if (consumers.size() != 1) {
    return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
  }

  Node* next_node;
  next_node = consumers[0];
  if (next_node == nullptr) {
    return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
  }
  if (consumed_nodes->find(next_node->id) != consumed_nodes->end()) {
    return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
  }
  Node* relu_node = nullptr;
  ReLUAttributes relu_attributes;
  if (OperationTypeFromString(next_node->operation.type) ==
      OperationType::RELU) {
    relu_node = next_node;
    auto relu_outputs = graph.FindOutputs(relu_node->id);
    consumers = graph.FindConsumers(relu_outputs[0]->id);
    if (consumers.size() != 1) {
      return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
    }
    relu_attributes =
        absl::any_cast<ReLUAttributes>(relu_node->operation.attributes);
    next_node = consumers[0];
  }

  auto* conv_node = next_node;
  if (OperationTypeFromString(conv_node->operation.type) !=
      OperationType::CONVOLUTION_2D) {
    return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
  }
  if (graph.FindInputs(conv_node->id).size() != 1) {
    return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
  }
  auto dw_attr = absl::any_cast<DepthwiseConvolution2DAttributes>(
      dw_node->operation.attributes);
  auto conv_attr =
      absl::any_cast<Convolution2DAttributes>(conv_node->operation.attributes);
  auto conv_outputs = graph.FindOutputs(conv_node->id);
  OperationDef op_def;
  op_def.precision = precision;
  auto it = tensor_descriptors.find(dw_inputs[0]->id);
  if (it != tensor_descriptors.end()) {
    op_def.src_tensors.push_back(it->second);
  }
  it = tensor_descriptors.find(conv_outputs[0]->id);
  if (it != tensor_descriptors.end()) {
    op_def.dst_tensors.push_back(it->second);
  }
  if (!IsDepthwiseConvPlus1x1ConvSupported(op_def, gpu_info, dw_attr, conv_attr,
                                           &conv_outputs[0]->tensor.shape)) {
    return absl::NotFoundError("DepthwiseConvPlus1x1Conv not suitable.");
  }
  std::unique_ptr<GPUOperation>* gpu_op =
      InitSingleOpSubgraph(dw_inputs, conv_outputs, gpu_subgraph);
  ReLUAttributes* relu_attr_ptr = relu_node ? &relu_attributes : nullptr;
  auto operation = CreateDepthwiseConvPlus1x1Conv(op_def, gpu_info, dw_attr,
                                                  conv_attr, relu_attr_ptr);
  *gpu_op = std::make_unique<GPUOperation>(std::move(operation));
  (*gpu_op)->flops_ = GetDepthwiseConvolutionFlops(dw_outputs[0]->tensor.shape,
                                                   dw_attr.weights.shape) +
                      GetConvolutionFlops(conv_outputs[0]->tensor.shape,
                                          conv_attr.weights.shape);
  std::string fused_nodes = std::to_string(dw_node->id);
  if (relu_node) {
    fused_nodes += " " + std::to_string(relu_node->id);
  }
  fused_nodes += " " + std::to_string(conv_node->id);
  gpu_subgraph->operations[0].name =
      "depthwise_conv_plus_1x1_conv " + fused_nodes;
  consumed_nodes->insert(dw_node->id);
  if (relu_node) {
    consumed_nodes->insert(relu_node->id);
  }
  consumed_nodes->insert(conv_node->id);
  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace tflite
