// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/common/logging/logging.h"
#include "core/providers/coreml/coreml_execution_provider.h"
#include "core/providers/coreml/coreml_provider_factory.h"
#include "core/session/inference_session.h"
#include "test/common/tensor_op_test_utils.h"
#include "test/framework/test_utils.h"
#include "test/util/include/asserts.h"
#include "test/util/include/current_test_name.h"
#include "test/util/include/default_providers.h"
#include "test/util/include/inference_session_wrapper.h"
#include "test/util/include/test_environment.h"
#include "test/util/include/test_utils.h"

#if !defined(ORT_MINIMAL_BUILD)
// if this is a full build we need the provider test utils
#include "test/providers/provider_test_utils.h"
#endif  // !(ORT_MINIMAL_BUILD)

#include "gtest/gtest.h"
#include "gmock/gmock.h"

using namespace ONNX_NAMESPACE;
using namespace ::onnxruntime::logging;

namespace onnxruntime {
namespace test {

// We want to run UT on CPU only to get output value without losing precision to pass the verification
static constexpr uint32_t s_coreml_flags = COREML_FLAG_USE_CPU_ONLY;

static std::unique_ptr<IExecutionProvider> MakeCoreMLExecutionProvider(uint32_t flags = s_coreml_flags) {
  return std::make_unique<CoreMLExecutionProvider>(flags);
}

#if !defined(ORT_MINIMAL_BUILD)

TEST(CoreMLExecutionProviderTest, FunctionTest) {
  const ORTCHAR_T* model_file_name = ORT_TSTR("coreml_execution_provider_test_graph.onnx");

  {  // Create the model with 2 add nodes
    onnxruntime::Model model("graph_1", false, DefaultLoggingManager().DefaultLogger());
    auto& graph = model.MainGraph();
    std::vector<onnxruntime::NodeArg*> inputs;
    std::vector<onnxruntime::NodeArg*> outputs;

    // FLOAT tensor.
    ONNX_NAMESPACE::TypeProto float_tensor;
    float_tensor.mutable_tensor_type()->set_elem_type(ONNX_NAMESPACE::TensorProto_DataType_FLOAT);
    float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
    float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);
    float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(3);
    float_tensor.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(2);

    auto& input_arg_1 = graph.GetOrCreateNodeArg("X", &float_tensor);
    auto& input_arg_2 = graph.GetOrCreateNodeArg("Y", &float_tensor);
    inputs.push_back(&input_arg_1);
    inputs.push_back(&input_arg_2);
    auto& output_arg = graph.GetOrCreateNodeArg("node_1_out_1", &float_tensor);
    outputs.push_back(&output_arg);
    graph.AddNode("node_1", "Add", "node 1.", inputs, outputs);

    auto& input_arg_3 = graph.GetOrCreateNodeArg("Z", &float_tensor);
    inputs.clear();
    inputs.push_back(&output_arg);
    inputs.push_back(&input_arg_3);
    auto& output_arg_2 = graph.GetOrCreateNodeArg("M", &float_tensor);
    outputs.clear();
    outputs.push_back(&output_arg_2);
    graph.AddNode("node_2", "Add", "node 2.", inputs, outputs);

    ASSERT_STATUS_OK(graph.Resolve());
    ASSERT_STATUS_OK(onnxruntime::Model::Save(model, model_file_name));
  }

#if defined(__APPLE__)
  std::vector<int64_t> dims_mul_x = {1, 1, 3, 2};
  std::vector<float> values_mul_x = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
  OrtValue ml_value_x;

  AllocatorPtr allocator = std::make_shared<CPUAllocator>();
  CreateMLValue<float>(allocator, dims_mul_x, values_mul_x, &ml_value_x);
  OrtValue ml_value_y;
  CreateMLValue<float>(allocator, dims_mul_x, values_mul_x, &ml_value_y);
  OrtValue ml_value_z;
  CreateMLValue<float>(allocator, dims_mul_x, values_mul_x, &ml_value_z);

  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value_x));
  feeds.insert(std::make_pair("Y", ml_value_y));
  feeds.insert(std::make_pair("Z", ml_value_z));

  RunAndVerifyOutputsWithEP(model_file_name, "CoreMLExecutionProviderTest.FunctionTest",
                            MakeCoreMLExecutionProvider(),
                            feeds);
#else
  TestModelLoad(model_file_name, MakeCoreMLExecutionProvider(), ExpectedEPNodeAssignment::Some);
#endif
}

// CoreML EP currently handles a special case for supporting ArgMax op:
// An ArgMax followed by a Cast to int32 type.
// Please see in <repo_root>/onnxruntime/core/providers/coreml/builders/impl/argmax_op_builder.cc
// and /cast_op_builder.cc. We have the following UT test here for this special case
// This test case can also be shared later if we want to support similar cases in NNAPI
TEST(CoreMLExecutionProviderTest, ArgMaxCastTest) {
  const ORTCHAR_T* model_file_name = ORT_TSTR("testdata/coreml_argmax_cast_test.onnx");

#if defined(__APPLE__)
  std::vector<int64_t> dims_mul_x = {3, 2, 2};
  std::vector<float> values_mul_x = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f};
  OrtValue ml_value_x;
  AllocatorPtr allocator = std::make_shared<CPUAllocator>();
  CreateMLValue<float>(allocator, dims_mul_x, values_mul_x, &ml_value_x);

  NameMLValMap feeds;
  feeds.insert(std::make_pair("X", ml_value_x));

  RunAndVerifyOutputsWithEP(model_file_name, "CoreMLExecutionProviderTest.ArgMaxCastTest",
                            MakeCoreMLExecutionProvider(),
                            feeds);
#else
  TestModelLoad(model_file_name, MakeCoreMLExecutionProvider(), ExpectedEPNodeAssignment::Some);
#endif
}

TEST(CoreMLExecutionProviderTest, GatherWithScalarIndices) {
  // For scalar inputs, the input shape is modified from [] -> [1] before passing the input to CoreML.
  // This won't work for Gather because the output shape depends on the `indices` input shape which could be a scalar.
  // Currently, we expect the CoreML EP to only take the Shape node in this graph (Gather -> Shape).
  const auto model_file_name = ORT_TSTR("testdata/gather_with_scalar_indices_then_shape.onnx");

#if defined(__APPLE__)
  RandomValueGenerator gen{1234};
  std::vector<int64_t> X_shape = {5, 3, 4};
  std::vector<float> X_data = gen.Uniform<float>(X_shape, 0.0f, 1.0f);
  OrtValue X = CreateInputOrtValueOnCPU<float>(X_shape, X_data);
  OrtValue indices = CreateInputOrtValueOnCPU<int64_t>(AsSpan<int64_t>({}), AsSpan<int64_t>({1}));

  RunAndVerifyOutputsWithEP(model_file_name, CurrentTestName(),
                            MakeCoreMLExecutionProvider(),
                            {{"X", X}, {"indices", indices}});
#else
  TestModelLoad(model_file_name, MakeCoreMLExecutionProvider(), ExpectedEPNodeAssignment::Some);
#endif
}

TEST(CoreMLExecutionProviderTest, ShapeThenSliceAndGather) {
  // This is a simple test model that provides the output of Shape to Slice and Gather.
  // We expect the CoreML EP to support shape manipulations like this.
  const auto model_file_name = ORT_TSTR("testdata/shape_then_slice_and_gather.onnx");

#if defined(__APPLE__)
  RandomValueGenerator gen{1234};
  std::vector<int64_t> X_shape = {5, 3, 4, 1, 2};
  std::vector<float> X_data = gen.Uniform<float>(X_shape, 0.0f, 1.0f);
  OrtValue X = CreateInputOrtValueOnCPU<float>(X_shape, X_data);

  RunAndVerifyOutputsWithEP(model_file_name, CurrentTestName(),
                            MakeCoreMLExecutionProvider(),
                            {{"X", X}},
                            EPVerificationParams{ExpectedEPNodeAssignment::All});
#else
  TestModelLoad(model_file_name, MakeCoreMLExecutionProvider(), ExpectedEPNodeAssignment::All);
#endif
}

#endif  // !(ORT_MINIMAL_BUILD)

TEST(CoreMLExecutionProviderTest, TestOrtFormatModel) {
  // mnist model that has only had basic optimizations applied. CoreML should be able to take at least some of the nodes
  const ORTCHAR_T* model_file_name = ORT_TSTR("testdata/mnist.basic.ort");

#if defined(__APPLE__)
  RandomValueGenerator random{};
  const std::vector<int64_t> dims = {1, 1, 28, 28};
  std::vector<float> data = random.Gaussian<float>(dims, 0.0f, 1.f);

  OrtValue ml_value;
  CreateMLValue<float>(TestCPUExecutionProvider()->CreatePreferredAllocators()[0], dims, data, &ml_value);

  NameMLValMap feeds;
  feeds.insert(std::make_pair("Input3", ml_value));

  RunAndVerifyOutputsWithEP(model_file_name, "CoreMLExecutionProviderTest.TestOrtFormatModel",
                            MakeCoreMLExecutionProvider(),
                            feeds);
#else
  TestModelLoad(model_file_name, MakeCoreMLExecutionProvider(), ExpectedEPNodeAssignment::Some);
#endif
}

// Test that we fix invalid names in model inputs, initializers and outputs.
// Names in CoreML cannot start with [0-9] or contain anything but "[a-z][A-Z][0-9]_"
TEST(CoreMLExecutionProviderTest, TestNameSanitization) {
  OpTester test("Clip", 11);

  std::vector<int64_t> dims{3, 3};
  test.AddInput<float>("0", dims,
                       {-1.0f, 0.0f, 1.0f,
                        -6.0f, 0.0f, 6.0f,
                        -5.4f, 2.0f, 6.0f});
  test.AddInput<float>("1.min", {}, {-5}, true);  // add as initializers
  test.AddInput<float>("2/max", {}, {5}, true);
  test.AddOutput<float>("3", dims,
                        {-1.0f, 0.0f, 1.0f,
                         -5.0f, 0.0f, 5.0f,
                         -5.0f, 2.0f, 5.0f});

  // TensorRT does not support Clip opset 11 yet.
  test.Run(OpTester::ExpectResult::kExpectSuccess, "", {kTensorrtExecutionProvider});
}
}  // namespace test
}  // namespace onnxruntime
