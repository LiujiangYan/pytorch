#include "caffe2/contrib/tensorrt/tensorrt_tranformer.h"

#include <iostream>
#include <unordered_set>

#include <NvInfer.h>
#include <google/protobuf/text_format.h>
#include <onnx2trt.hpp>

#include "caffe2/contrib/tensorrt/trt_utils.h"
#include "caffe2/core/context_gpu.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/operator.h"
#include "caffe2/onnx/onnx_exporter.h"
#include "caffe2/opt/backend_cutting.h"

namespace caffe2 {

namespace {

// TODO(yinghai): Remove the awkward conversion between unordered_map and map
std::unordered_map<std::string, TensorShape> InferShapes(
    Workspace* ws,
    NetDef* pred_net,
    CaffeMap<std::string, TensorShape>* shape_hints_ordered) {
  // Populate shapes from workplace
  const std::vector<string>& ws_blobs = ws->Blobs();
  for (const auto& s : ws_blobs) {
    shape_hints_ordered->emplace(s, GetTensorShapeOfBlob(ws->GetBlob(s)));
  }

  std::vector<NetDef*> nets;
  nets.emplace_back(pred_net);
  InferBlobShapesAndTypes(*shape_hints_ordered, nets);
  std::unordered_map<std::string, TensorShape> shape_hints;
  for (const auto& kv : *shape_hints_ordered) {
    shape_hints.emplace(kv.first, kv.second);
  }

  return shape_hints;
}


void CPUTensorToTensorProto(
    const TensorCPU& cpu_tensor,
    ::ONNX_NAMESPACE::TensorProto* t) {
  const auto len = cpu_tensor.size();
  if (cpu_tensor.template IsType<float>()) {
    t->set_data_type(::ONNX_NAMESPACE::TensorProto::FLOAT);
    const float* data = cpu_tensor.template data<float>();
    for (auto i = 0; i < len; ++i) {
      t->add_float_data(*data++);
    }
  } else if (cpu_tensor.template IsType<int64_t>()) {
    t->set_data_type(::ONNX_NAMESPACE::TensorProto::INT64);
    const int64_t* data = cpu_tensor.template data<int64_t>();
    for (auto i = 0; i < len; ++i) {
      t->add_int64_data(*data++);
    }
  } else if (cpu_tensor.template IsType<int32_t>()) {
    t->set_data_type(::ONNX_NAMESPACE::TensorProto::INT32);
    const int32_t* data = cpu_tensor.template data<int32_t>();
    for (auto i = 0; i < len; ++i) {
      t->add_int32_data(*data++);
    }
  } else {
    CAFFE_THROW(
        "Don't know how to convert workspace tensor type ",
        cpu_tensor.meta().name(),
        " to ONNX TensorProto");
  }
}

void BlobToTensorProto(
    const std::string& name,
    Workspace* ws,
    CUDAContext* context,
    ::ONNX_NAMESPACE::TensorProto* t) {
  // Set name
  t->set_name(name);
  const Blob* blob = ws->GetBlob(name);
  CAFFE_ENFORCE(blob, "Blob ", name, " doesn't exist");

  // Set dims
  const auto shape = GetTensorShapeOfBlob(blob);
  for (const auto i : shape.dims()) {
    t->add_dims(i);
  }

  // Set values
  if (blob->template IsType<TensorCPU>()) {
    const auto& cpu_tensor = blob->template Get<TensorCPU>();
    CPUTensorToTensorProto(cpu_tensor, t);
  } else if (blob->template IsType<TensorCUDA>()) {
    const auto& cuda_tensor = blob->template Get<TensorCUDA>();
    const auto cpu_tensor = TensorCPU(cuda_tensor, context);
    context->FinishDeviceComputation();
    CPUTensorToTensorProto(cpu_tensor, t);
  } else {
    CAFFE_THROW(
        "Initialization blob ",
        name,
        " needs to be either TensorCPU or TensorCUDA");
  }
}

void BuildInitializationList(
    Workspace* ws,
    ::ONNX_NAMESPACE::GraphProto* g,
    std::unordered_set<std::string>* initialization_list) {
  const std::vector<string>& ws_blobs = ws->Blobs();

  // Create a CUDA context and reuse it for potential tensor copies across
  // devices
  CUDAContext context;

  for (const auto& s : ws_blobs) {
    auto it = initialization_list->find(s);
    if (it != initialization_list->end()) {
      auto* init_tensor = g->add_initializer();
      BlobToTensorProto(s, ws, &context, init_tensor);
      initialization_list->erase(it);
    }
  }
  CAFFE_ENFORCE(
      initialization_list->empty(), "Unfulfilled initialization list");
  for (const auto& t : g->initializer()) {
    VLOG(2) << "Initializer: " << t.name();
  }
}

std::vector<::ONNX_NAMESPACE::ValueInfoProto> ConvertToValueInfo(
    const std::vector<std::string>& names,
    const std::unordered_map<std::string, TensorShape>& shape_hints) {
  std::vector<::ONNX_NAMESPACE::ValueInfoProto> r;
  for (const auto& s : names) {
    r.emplace_back();
    auto& value_info = r.back();
    value_info.set_name(s);
    const auto it = shape_hints.find(s);
    if (it == shape_hints.end()) {
      LOG(WARNING) << "Cannot get shape of " << s;
    } else {
      auto* tensor_type = value_info.mutable_type()->mutable_tensor_type();
      tensor_type->set_elem_type(
          ::ONNX_NAMESPACE::TensorProto_DataType::TensorProto_DataType_FLOAT);
      auto* shape = tensor_type->mutable_shape();
      for (int i = 0; i < it->second.dims().size(); ++i) {
        auto* dim = shape->add_dim();
        dim->set_dim_value(it->second.dims(i));
      }
    }
  }
  return r;
}

void FillModelInfo(::ONNX_NAMESPACE::ModelProto* model) {
  model->set_ir_version(::ONNX_NAMESPACE::Version::IR_VERSION);
  model->set_producer_name("caffe2");
  auto* opset_id = model->add_opset_import();
  opset_id->set_domain("");
  opset_id->set_version(3);
}
} // namespace

OperatorDef TensorRTTransformer::BuildTrtOp(
    const std::string& onnx_model_str,
    const std::unordered_map<std::string, std::vector<int>>&
        output_size_hints) {
  OperatorDef op;
  op.set_type("TensorRT");

  tensorrt::TrtLogger logger;
  auto trt_builder = tensorrt::TrtObject(nvinfer1::createInferBuilder(logger));
  auto trt_network = tensorrt::TrtObject(trt_builder->createNetwork());
  auto importer =
      tensorrt::TrtObject(onnx2trt::createImporter(trt_network.get()));
  auto status =
      importer->import(onnx_model_str.data(), onnx_model_str.size(), false);
  if (status.is_error()) {
    CAFFE_THROW(
        "TensorRTTransformer ERROR: ",
        status.file(),
        ":",
        status.line(),
        " In function ",
        status.func(),
        ":\n",
        "[",
        status.code(),
        "] ",
        status.desc());
  }
  trt_builder->setMaxBatchSize(max_batch_size_);
  trt_builder->setMaxWorkspaceSize(max_workspace_size_);
  trt_builder->setDebugSync(debug_builder_);
  auto trt_engine =
      tensorrt::TrtObject(trt_builder->buildCudaEngine(*trt_network.get()));

  // Set up inputs/outputs in the order of they appearnce in getNbBindings
  int num_bindings = trt_engine->getNbBindings();
  for (int b = 0; b < num_bindings; ++b) {
    const auto& name = trt_engine->getBindingName(b);
    if (trt_engine->bindingIsInput(b)) {
      op.add_input(name);
    } else {
      op.add_output(name);
    }
  }

  auto engine_plan = tensorrt::TrtObject(trt_engine->serialize());

  auto* serialized_engine_arg = op.add_arg();
  serialized_engine_arg->set_s("");
  serialized_engine_arg->set_name("serialized_engine");
  auto* s = serialized_engine_arg->mutable_s();
  s->assign((char*)engine_plan->data(), engine_plan->size());

  auto* max_batch_size_arg = op.add_arg();
  max_batch_size_arg->set_name("max_batch_size");
  max_batch_size_arg->set_i(max_batch_size_);

  auto* verbosity_arg = op.add_arg();
  verbosity_arg->set_name("log_verbosity");
  verbosity_arg->set_i(verbosity_);

  for (int i = 0; i < op.output_size(); ++i) {
    const auto& o = op.output(i);
    const auto it = output_size_hints.find(o);
    if (it != output_size_hints.end()) {
      const auto& dims = it->second;
      auto* output_size_hint_arg = op.add_arg();
      output_size_hint_arg->set_name(MakeString("output_size_hint_", i));
      for (const auto& d : dims) {
        output_size_hint_arg->add_ints(d);
      }

      LOG(INFO) << "Adding output hint: " << o;
    }
  }

  return op;
}

NetDef TensorRTTransformer::SubnetToTrtOp(
    const caffe2::NetDef& net,
    Workspace* ws,
    onnx::OnnxExporter* exporter,
    std::unordered_map<std::string, TensorShape>* shape_hints) {
  ::ONNX_NAMESPACE::ModelProto onnx_model;
  FillModelInfo(&onnx_model);

  // Convert c2 ops to onnx ops, add const weights if there are any
  for (const auto& op : net.op()) {
    const auto results = exporter->Caffe2OpToOnnxNodes(op, *shape_hints);
    const auto& node_protos = results.first;
    for (const auto& n : node_protos) {
      onnx_model.mutable_graph()->add_node()->CopyFrom(n);
    }
    for (const auto& t : results.second) {
      VLOG(2) << "Adding extra init tensor: " << t.name();
      TensorShape shape;
      shape.mutable_dims()->CopyFrom(t.dims());
      shape_hints->emplace(t.name(), std::move(shape));
      ::ONNX_NAMESPACE::TensorProto tf;
      tf.set_name(t.name());
      tf.mutable_dims()->CopyFrom(t.dims());
      tf.set_data_type(::ONNX_NAMESPACE::TensorProto::FLOAT);
      std::vector<int64_t> v;
      v.resize(t.raw_data().size() / sizeof(int64_t));
      memcpy(v.data(), t.raw_data().data(), t.raw_data().size());
      std::vector<float> vf;
      for (auto i : v) {
        vf.push_back(static_cast<float>(i));
      }
      tf.mutable_raw_data()->assign(
          reinterpret_cast<const char*>(vf.data()), sizeof(float) * vf.size());

      onnx_model.mutable_graph()->add_initializer()->CopyFrom(tf);
    }
  }

  // Convert outputs and compute output shape hints
  std::vector<std::string> io_names;
  for (const auto& output : net.external_output()) {
    io_names.emplace_back(output);
  }
  auto io_vec = ConvertToValueInfo(io_names, *shape_hints);
  std::unordered_map<std::string, std::vector<int>> output_shape_hints;
  for (const auto& i : io_vec) {
    onnx_model.mutable_graph()->add_output()->CopyFrom(i);
    auto ret = output_shape_hints.emplace(i.name(), std::vector<int>());
    auto& vec = ret.first->second;
    const auto it = shape_hints->find(i.name());
    CAFFE_ENFORCE(
        it != shape_hints->end(),
        "Cannot find shape info for output ",
        i.name());
    const auto& shape = it->second;
    for (int k = 0; k < shape.dims().size(); ++k) {
      vec.push_back(shape.dims(k));
    }
  }

  // Convert inputs and figure out weights
  std::unordered_set<std::string> weights;
  const std::vector<string>& ws_blobs = ws->Blobs();
  for (const auto& s : ws_blobs) {
    VLOG(2) << "Add weights: " << s;
    weights.emplace(s);
  }

  std::unordered_set<std::string> total_inputs;
  std::unordered_set<std::string> initialization_list;
  std::vector<std::string> total_inputs_vec;

  // Extra intermediate weights created during conversion
  for (const auto& extra_weight : onnx_model.graph().initializer()) {
    if (total_inputs.emplace(extra_weight.name()).second) {
      total_inputs_vec.emplace_back(extra_weight.name());
    }
  }
  // Boundary inputs, should not be weights
  std::unordered_set<std::string> boundary_inputs;
  for (const auto& i : net.external_input()) {
    boundary_inputs.emplace(i);
  }

  for (const auto& op : net.op()) {
    for (const auto& input : op.input()) {
      if (total_inputs.emplace(input).second && weights.count(input)) {
        // We add weights as inputs too
        total_inputs_vec.emplace_back(input);
        initialization_list.emplace(input);
        VLOG(2) << "Add input weights: " << input;
      } else if (boundary_inputs.count(input)) {
        VLOG(2) << "Adding boundary input: " << input;
        total_inputs_vec.emplace_back(input);
      }
    }
  }
  io_vec = ConvertToValueInfo(total_inputs_vec, *shape_hints);
  for (const auto& i : io_vec) {
    onnx_model.mutable_graph()->add_input()->CopyFrom(i);
  }

  // Convert weights to initializing tensors
  BuildInitializationList(ws, onnx_model.mutable_graph(), &initialization_list);

  // Debug stuff
  if (debug_builder_) {
    std::ofstream ff("trt.onnx");
    for (const auto& t : onnx_model.graph().initializer()) {
      ff << "tensor: " << t.name() << std::endl;
      ff << "  dims: ";
      for (auto i : t.dims()) {
        ff << i << " ";
      }
      ff << std::endl;
      for (auto i : t.float_data()) {
        ff << "    " << i << std::endl;
      }
    }
    ff.close();
  }

  // Onnx model is ready. Call onnx-trt to convert to one trt c2 op
  std::string model_str;
  onnx_model.SerializeToString(&model_str);
  NetDef net_opt;
  auto* op = net_opt.add_op();
  *op = BuildTrtOp(model_str, output_shape_hints);
  for (const auto& i : op->input()) {
    net_opt.add_external_input(i);
  }
  for (const auto& i : op->output()) {
    net_opt.add_external_output(i);
  }

  return net_opt;
}

CaffeMap<std::string, TensorShape> TensorRTTransformer::SsaRewriteAndMapNames(
    Workspace* ws,
    NetDef* pred_net,
    const std::unordered_map<std::string, TensorShape>& input_shape_hints) {
  input_mapping_ = onnx::SsaRewrite(nullptr, pred_net);
  std::unordered_map<std::string, std::string> input_reverse_mapping;
  std::vector<std::string> external_inputs;
  for (const auto kv : input_mapping_) {
    input_reverse_mapping.emplace(kv.second, kv.first);
    if (!ws->HasBlob(kv.second)) {
      external_inputs.emplace_back(kv.first);
    }
  }
  for (const auto& i: external_inputs) {
    input_mapping_.erase(i);
  }
  CaffeMap<std::string, TensorShape> shape_hints_ordered;
  for (const auto& kv : input_shape_hints) {
    const auto it = input_reverse_mapping.find(kv.first);
    if (it != input_reverse_mapping.end()) {
      LOG(INFO) << "Adding input hint: " << it->second;
      shape_hints_ordered.emplace(it->second, kv.second);
    } else {
      shape_hints_ordered.emplace(kv.first, kv.second);
    }
  }
  return shape_hints_ordered;
}

void TensorRTTransformer::PruneUnusedWeights(
    Workspace* ws,
    const NetDef& pred_net) {
  std::unordered_set<std::string> used_weights;
  for (const auto& op : pred_net.op()) {
    for (const auto& i : op.input()) {
      used_weights.emplace(i);
    }
  }

  for (const auto kv: input_mapping_) {
    // for weights that are not referenced anywhere, we remove it from the
    // original workspace
    if (!used_weights.count(kv.first)) {
      VLOG(2) << "Removing unused weight blob: " << kv.second << " ("
              << kv.first << ")";
      ws->RemoveBlob(kv.second);
    }
  }
}

// Cutting off the runnable part and replace with tensor ops. Asssume the nets
// were topologically sorted
void TensorRTTransformer::Transform(
    Workspace* ws,
    NetDef* pred_net,
    const std::unordered_map<std::string, TensorShape>& input_shape_hints) {
  CAFFE_ENFORCE(ws);
  auto shape_hints_ordered =
      SsaRewriteAndMapNames(ws, pred_net, input_shape_hints);
  Workspace mapped_ws(ws, input_mapping_);
  auto shape_hints = InferShapes(&mapped_ws, pred_net, &shape_hints_ordered);

  CAFFE_ENFORCE(pred_net, "Predict net cannot be nullptr");
  onnx::OnnxExporter exporter(nullptr, true);
  auto importer = tensorrt::TrtObject(onnx2trt::createImporter(nullptr));

  // function to tell whether TensorRT supports a given C2 op or not
  auto supports =
      [&exporter, &shape_hints, importer](const caffe2::OperatorDef& op) {
        const OpSchema* schema = OpSchemaRegistry::Schema(op.type());
        if (!schema || schema->onnx_schema().empty()) {
          LOG(INFO) << "Cannot export c2 op " << op.type() << " to onnx";
          return false;
        }

        auto results = exporter.Caffe2OpToOnnxNodes(op, shape_hints);
        for (const auto& n : results.first) {
          if (!importer->supports(n)) {
            LOG(INFO) << "TRT does not support ONNX node " << n.op_type();
            return false;
          }
        }
        return true;
      };

  // function to convert runnbale subgraph into a trt op. Note that to keep the
  // interface clean, we do the double conversion from C2 op to Onnx ops here
  // but it should be OK as the cost is really small. We also need to keep the
  // same exporter throughout the process to avoid duplicated dummy name
  // generation
  onnx::OnnxExporter exporter2(nullptr, true);
  auto trt_converter = [this, &mapped_ws, &shape_hints, &exporter2](
                           const caffe2::NetDef& net) mutable {
    return SubnetToTrtOp(net, &mapped_ws, &exporter2, &shape_hints);
  };

  NetDef net_opt = opt::OptimizeForBackend(*pred_net, supports, trt_converter);

  // Need to figure out a proper place to handle device option
  net_opt.mutable_device_option()->CopyFrom(pred_net->device_option());
  pred_net->Swap(&net_opt);

  PruneUnusedWeights(ws, *pred_net);
}

} // namespace caffe2
