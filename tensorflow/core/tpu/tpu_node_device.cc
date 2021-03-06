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

#include "tensorflow/core/tpu/tpu_node_device.h"

#include "tensorflow/compiler/jit/kernels/xla_ops.h"
#include "tensorflow/compiler/jit/xla_device.h"
#include "tensorflow/compiler/jit/xla_device_ops.h"
#include "tensorflow/compiler/tf2xla/tf2xla_util.h"
#include "tensorflow/compiler/tf2xla/xla_op_registry.h"
#include "tensorflow/core/common_runtime/copy_tensor.h"
#include "tensorflow/core/common_runtime/device.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/dma_helper.h"
#include "tensorflow/core/framework/kernel_def.pb.h"
#include "tensorflow/core/framework/tensor_reference.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/public/session_options.h"
#include "tensorflow/core/tpu/kernels/tpu_configuration_ops.h"
#include "tensorflow/core/tpu/kernels/tpu_util.h"
#include "tensorflow/core/tpu/tpu_defs.h"
#include "tensorflow/core/tpu/tpu_node_device_util.h"
#include "tensorflow/stream_executor/tpu/tpu_node_context.h"
#include "tensorflow/stream_executor/tpu/tpu_platform_interface.h"
#include "tensorflow/stream_executor/tpu/tpu_stream_interface.h"

namespace tensorflow {
namespace {

static bool tpu_autoclustering_flag = false;
static bool tpu_xla_device_failure_closes_chips_flag = true;
static bool tpu_use_substreams_for_cross_tpu_device_transfers_flag = true;

// Check if TPU has been initialized. TPU initialization is not necessary
// for 1x1.
Status CheckIfTPUInitialized() {
  auto* tpu_platform = tpu::TpuPlatformInterface::GetRegisteredPlatform();
  if (!tpu_platform->Initialized()) {
    return errors::FailedPrecondition(
        "The TPU system has not been initialized.");
  }
  return Status::OK();
}

// Implementation of TPU->TPU device copies that copies over the dedicated TPU
// interconnects, which is much faster than PCIe or the host network.
// TODO(b/117426293): This implementation is only called for direct interconnect
// transfers between TPU devices attached to the same host. Ideally, we would
// generalize this support to direct interconnect transfers across hosts, but
// currently the CopyTensor infrastructure seems to the network topology is
// strictly hierarchical, that is, transfers between devices on different hosts
// can only take place using the host network.
void TpuDeviceToDeviceCopy(DeviceContext* src_dev_context,
                           DeviceContext* dst_dev_context, Device* src,
                           Device* dst, AllocatorAttributes src_allocator_attrs,
                           AllocatorAttributes dst_allocator_attrs,
                           const Tensor* input, Tensor* output,
                           int dev_to_dev_stream_index, StatusCallback done) {
  XlaDeviceContext* const src_xla_context =
      static_cast<XlaDeviceContext*>(src_dev_context);
  XlaDeviceContext* const dst_xla_context =
      static_cast<XlaDeviceContext*>(dst_dev_context);
  static const bool should_use_substream =
      tpu_use_substreams_for_cross_tpu_device_transfers_flag;

  auto impl = [&]() -> Status {
    if (src->name() != dst->name()) {
      Status s = CheckIfTPUInitialized();
      if (!s.ok()) {
        done(s);
        return Status::OK();
      }
    }
    if (input->shape().num_elements() == 0) {
      // Zero-element tensors have no backing buffers.
      done(Status::OK());
      return Status::OK();
    }

    se::Stream* const src_compute_stream = src_xla_context->stream();
    TF_RET_CHECK(src_compute_stream != nullptr);
    TF_RET_CHECK(input->dtype() == output->dtype())
        << "input type: " << DataTypeString(input->dtype()) << " output type "
        << DataTypeString(output->dtype());
    TF_RET_CHECK(input->shape() == output->shape());
    TF_RET_CHECK(DMAHelper::CanUseDMA(input));
    auto* const src_compute_stream_impl = static_cast<tpu::TpuStreamInterface*>(
        src_compute_stream->implementation());

    se::Stream* dst_compute_stream = dst_xla_context->stream();
    auto* const dst_compute_stream_impl = static_cast<tpu::TpuStreamInterface*>(
        dst_compute_stream->implementation());

    if (src_compute_stream_impl->IsSameSharedMemoryLocation(
            dst_compute_stream_impl)) {
      // Surprisingly, this path does get triggered in practice.
      *output = *input;
      done(Status::OK());
      return Status::OK();
    }

    // To avoid stream exhaustion, we pick a substream from a pool if enabled.
    se::Stream* const device_to_device_master_stream =
        should_use_substream ? dst_xla_context->device_to_device_stream(0)
                             : nullptr;
    se::Stream* const dst_device_to_device_stream =
        should_use_substream
            ? device_to_device_master_stream->GetOrCreateSubStream()
            : dst_xla_context->GetDeviceToDeviceStream();
    TF_RET_CHECK(dst_device_to_device_stream != nullptr);
    auto return_substream = gtl::MakeCleanup(
        [device_to_device_master_stream, dst_device_to_device_stream] {
          if (device_to_device_master_stream) {
            device_to_device_master_stream->ReturnSubStream(
                dst_device_to_device_stream);
          }
        });

    auto* const dst_device_to_device_stream_impl =
        static_cast<tpu::TpuStreamInterface*>(
            dst_device_to_device_stream->implementation());

    const int dst_device_ordinal =
        dst_xla_context->stream()->parent()->device_ordinal();

    XlaTensor* const xla_input = XlaTensor::FromTensor(input);
    TF_RET_CHECK(xla_input != nullptr && xla_input->has_shaped_buffer());
    XlaTensor* const xla_output = XlaTensor::FromTensor(output);
    TF_RET_CHECK(xla_output != nullptr && !xla_output->has_shaped_buffer());
    TF_RET_CHECK(input->shape() == output->shape());

    TF_ASSIGN_OR_RETURN(xla::Shape shape,
                        dst_xla_context->shape_representation_fn()(
                            input->shape(), input->dtype(),
                            /*use_fast_memory=*/false));
    TF_RETURN_IF_ERROR(xla_output->AllocateShapedBuffer(
        input->dtype(), shape, dst_xla_context->client(), dst_device_ordinal));

    VLOG(2) << "TpuDeviceToDeviceCopy: src: "
            << src_compute_stream->parent()->device_ordinal() << ", "
            << " dst: " << dst_compute_stream->parent()->device_ordinal()
            << ", "
            << " input buffers: " << xla_input->shaped_buffer().ToString()
            << " output buffers: " << xla_output->shaped_buffer().ToString();

    // Wait for definition event of the source tensor so the input buffers are
    // available.
    xla_input->WaitForDefinitionEventOnStream(dst_device_to_device_stream);

    // Wait for the destination tensor buffers to be ready, if they are not
    // available for an immediate write.
    if (!dst_xla_context->transfer_manager()->CanShapedBufferBeAccessedNow(
            dst_compute_stream->parent(), xla_output->shaped_buffer())) {
      dst_device_to_device_stream->ThenWaitFor(dst_compute_stream);
      // If the representation is a tuple, we also must wait for the tuple index
      // buffers to be available on the destination host to device transfer
      // stream.
      if (xla_output->shaped_buffer().on_device_shape().IsTuple()) {
        dst_xla_context->host_to_device_stream()->ThenWaitFor(
            dst_compute_stream);
      }
    }

    for (const auto& leaf : xla_input->shaped_buffer().buffers().leaves()) {
      const xla::ShapeIndex& index = leaf.first;
      const se::DeviceMemoryBase& input_buffer = leaf.second;
      const se::DeviceMemoryBase& output_buffer =
          xla_output->shaped_buffer().buffer(index);
      TF_RET_CHECK(input_buffer.size() == output_buffer.size())
          << "input: " << input_buffer.size()
          << " output: " << output_buffer.size();
      TF_RETURN_IF_ERROR(
          dst_device_to_device_stream_impl->EnqueueOnTpuDeviceSendRecvLocal(
              input_buffer, output_buffer));
    }

    // If the on-device shape is a tuple, write new tuple index buffers.
    if (xla_output->shaped_buffer().on_device_shape().IsTuple()) {
      TF_RETURN_IF_ERROR(
          dst_xla_context->transfer_manager()->WriteTupleIndexTablesAsync(
              dst_xla_context->host_to_device_stream(),
              xla_output->shaped_buffer()));

      // We need a single definition event for an XlaTensor, so make the
      // device to device stream wait for the stream that wrote the tuple index
      // tables on the destination device. Should this prove to be a problem,
      // we can always extend XlaTensor to take a pair of definition events that
      // must all be satisfied, or add an Event::Merge() API that allows us to
      // build an event that is triggered when all of its dependencies are
      // triggered.
      dst_device_to_device_stream->ThenWaitFor(
          dst_xla_context->host_to_device_stream());
    }

    auto definition_event =
        std::make_shared<se::Event>(dst_xla_context->stream()->parent());
    TF_RET_CHECK(definition_event->Init()) << "Event failed to initialize!";
    dst_device_to_device_stream->ThenRecordEvent(definition_event.get());
    xla_output->ResetDefinitionEvent(std::move(definition_event),
                                     dst_device_to_device_stream);

    // The input must remain alive until the transfer completes, so we keep a
    // reference. We also wait until the transfer completes before calling
    // done().
    // The latter may be too conservative, but given the host is involved in
    // waiting for the transfer to complete anyway there is probably little
    // downside. If we were to add the ability for computations to wait directly
    // on transfers, then we might want to rethink this property.
    // Also ideally this host callback should be on source stream rather than
    // destination stream, but when this function returns, the send requests
    // might not be enqueued to the stream yet, we put it on destination stream.
    TensorReference input_reference(*input);
    std::move(return_substream).release();
    dst_device_to_device_stream->ThenDoHostCallback(
        [input_reference, done = std::move(done),
         device_to_device_master_stream, dst_device_to_device_stream] {
          if (device_to_device_master_stream) {
            device_to_device_master_stream->ReturnSubStream(
                dst_device_to_device_stream);
          }
          input_reference.Unref();
          done(Status::OK());
        });

    return Status::OK();
  };
  Status status = impl();
  if (!status.ok()) {
    done(status);
  }
}

class TpuNodeDeviceFactory : public DeviceFactory {
 public:
  Status ListPhysicalDevices(std::vector<string>* devices) override;
  Status CreateDevices(const SessionOptions& options, const string& name_prefix,
                       std::vector<std::unique_ptr<Device>>* devices) override;
};

Status TpuNodeDeviceFactory::ListPhysicalDevices(std::vector<string>* devices) {
  tpu::TpuPlatformInterface* platform =
      tpu::TpuPlatformInterface::GetRegisteredPlatform();
  if (platform == nullptr) {
    // If we don't have a platform registered, then we have no devices.
    return Status::OK();
  }

  int device_count = platform->VisibleDeviceCount();

  for (int i = 0; i < device_count; ++i) {
    const string device_name = strings::StrCat("/physical_device:TPU:", i);
    devices->push_back(device_name);
  }

  return Status::OK();
}

Status TpuNodeDeviceFactory::CreateDevices(
    const SessionOptions& session_options, const string& name_prefix,
    std::vector<std::unique_ptr<Device>>* devices) {
  tpu::TpuPlatformInterface* platform =
      tpu::TpuPlatformInterface::GetRegisteredPlatform();
  if (platform == nullptr) {
    // If we don't have a platform registered, then we should not create any.
    return Status::OK();
  }

  if (platform != nullptr && platform->ShouldRegisterTpuDeviceToDeviceCopy()) {
    RegisterTpuDeviceToDeviceCopy();
  }

  XlaOpRegistry::DeviceRegistration registration;
  registration.compilation_device_name = DEVICE_TPU_XLA_JIT;
  registration.autoclustering_policy =
      tpu_autoclustering_flag
          ? XlaOpRegistry::AutoclusteringPolicy::kAlways
          : XlaOpRegistry::AutoclusteringPolicy::kIfExplicitlyRequested;

  registration.cluster_resource_variable_ops_unsafely = true;
  registration.cluster_stack_ops = false;
  registration.cluster_tensor_array_ops = true;
  registration.cluster_stateful_rng_ops = true;
  registration.cluster_control_trigger = true;
  registration.elide_assert_and_checknumerics = true;
  registration.cluster_variant_ops = true;
  registration.cluster_slow_ops = true;
  registration.cluster_inaccurate_ops = true;
  XlaOpRegistry::RegisterCompilationDevice(DEVICE_TPU_NODE, registration);

  static XlaDeviceOpRegistrations* registrations =
      RegisterXlaDeviceKernels(DEVICE_TPU_NODE, DEVICE_TPU_XLA_JIT);
  (void)registrations;

  int device_count = platform->VisibleDeviceCount();
  VLOG(1) << "Creating " << device_count << " TPU devices";
  for (int i = 0; i < device_count; ++i) {
    TF_RETURN_IF_ERROR(tpu::TpuNodeContext::Initialize(i));

    XlaDevice::Options options;
    options.platform = platform;
    options.device_name_prefix = name_prefix;
    options.device_name = DEVICE_TPU_NODE;
    options.device_ordinal = i;
    options.compilation_device_name = DEVICE_TPU_XLA_JIT;
    options.use_multiple_streams = true;
    // TODO(jiawenhao): Implement and enable these.
    // options.shape_representation_fn = tpu::TpuShapeRepresentation;
    // options.padded_shape_fn = tpu::TpuPaddedShapeFn;
    auto device = absl::make_unique<XlaDevice>(session_options, options);

    // The GpuDeviceInfo actually provides information not only for GPU
    // devices but also for TPU. The name is a legacy from the pre-TPU
    // dark ages.
    Status status = device->UseGpuDeviceInfo();
    if (!status.ok()) {
      errors::AppendToMessage(&status, "while setting up ", DEVICE_TPU_XLA_JIT,
                              " device number ", i);
      return status;
    }
    device->SetAllowsSyncOnCompletion(false);
    if (tpu_xla_device_failure_closes_chips_flag) {
      device->SetHandleDeviceErrorCallback(&tpu::TpuNodeContext::CloseTpuHost);
    }

    devices->push_back(std::move(device));
  }

  return Status::OK();
}

}  // namespace

void RegisterTpuDeviceToDeviceCopy() {
  static auto* const register_tpu_tpu_copy = new CopyTensor::Registration(
      DEVICE_TPU_NODE, DEVICE_TPU_NODE, TpuDeviceToDeviceCopy);
  (void)register_tpu_tpu_copy;
}

void RegisterTpuNodeDevice(
    bool tpu_autoclustering, bool tpu_xla_device_failure_closes_chips,
    bool tpu_use_substreams_for_cross_tpu_device_transfers) {
  tpu_autoclustering_flag = tpu_autoclustering;
  tpu_xla_device_failure_closes_chips_flag =
      tpu_xla_device_failure_closes_chips;
  tpu_use_substreams_for_cross_tpu_device_transfers_flag =
      tpu_use_substreams_for_cross_tpu_device_transfers;

  REGISTER_LOCAL_DEVICE_FACTORY(DEVICE_TPU_NODE, TpuNodeDeviceFactory);

  REGISTER_XLA_LAUNCH_KERNEL(DEVICE_TPU_NODE, XlaLocalLaunchOp, kTpuAllTypes);
  REGISTER_XLA_COMPILE_KERNEL(DEVICE_TPU_NODE, XlaCompileOp, kTpuAllTypes);
  REGISTER_XLA_RUN_KERNEL(DEVICE_TPU_NODE, XlaRunOp, kTpuAllTypes);
  REGISTER_XLA_DEVICE_KERNELS(DEVICE_TPU_NODE, kTpuAllTypes);
}

}  // namespace tensorflow
