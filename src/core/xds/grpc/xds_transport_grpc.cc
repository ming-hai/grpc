//
// Copyright 2022 gRPC authors.
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
//

#include "src/core/xds/grpc/xds_transport_grpc.h"

#include <grpc/byte_buffer.h>
#include <grpc/byte_buffer_reader.h>
#include <grpc/event_engine/event_engine.h>
#include <grpc/grpc.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpc/impl/connectivity_state.h>
#include <grpc/impl/propagation_bits.h>
#include <grpc/slice.h>
#include <string.h>

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "src/core/client_channel/client_channel_filter.h"
#include "src/core/config/core_configuration.h"
#include "src/core/credentials/call/composite/composite_call_credentials.h"
#include "src/core/credentials/transport/channel_creds_registry.h"
#include "src/core/credentials/transport/composite/composite_channel_credentials.h"
#include "src/core/credentials/transport/transport_credentials.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/channel_fwd.h"
#include "src/core/lib/channel/channel_stack.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/event_engine/default_event_engine.h"
#include "src/core/lib/iomgr/closure.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/pollset_set.h"
#include "src/core/lib/slice/slice.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/surface/call.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/surface/init_internally.h"
#include "src/core/lib/surface/lame_client.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/util/debug_location.h"
#include "src/core/util/down_cast.h"
#include "src/core/util/orphanable.h"
#include "src/core/util/ref_counted_ptr.h"
#include "src/core/util/time.h"
#include "src/core/xds/grpc/xds_server_grpc_interface.h"
#include "src/core/xds/xds_client/xds_bootstrap.h"

namespace grpc_core {

//
// GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall
//

GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall::GrpcStreamingCall(
    WeakRefCountedPtr<GrpcXdsTransportFactory> factory, Channel* channel,
    const char* method,
    std::unique_ptr<StreamingCall::EventHandler> event_handler)
    : factory_(std::move(factory)), event_handler_(std::move(event_handler)) {
  // Create call.
  call_ = channel->CreateCall(
      /*parent_call=*/nullptr, GRPC_PROPAGATE_DEFAULTS, /*cq=*/nullptr,
      factory_->interested_parties(), Slice::FromStaticString(method),
      /*authority=*/std::nullopt, Timestamp::InfFuture(),
      /*registered_method=*/true);
  CHECK_NE(call_, nullptr);
  // Init data associated with the call.
  grpc_metadata_array_init(&initial_metadata_recv_);
  grpc_metadata_array_init(&trailing_metadata_recv_);
  // Initialize closure to be used for sending messages.
  GRPC_CLOSURE_INIT(&on_request_sent_, OnRequestSent, this, nullptr);
  // Start ops on the call.
  grpc_call_error call_error;
  grpc_op ops[2];
  memset(ops, 0, sizeof(ops));
  // Send initial metadata.
  grpc_op* op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = GRPC_INITIAL_METADATA_WAIT_FOR_READY |
              GRPC_INITIAL_METADATA_WAIT_FOR_READY_EXPLICITLY_SET;
  op->reserved = nullptr;
  ++op;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata =
      &initial_metadata_recv_;
  op->flags = 0;
  op->reserved = nullptr;
  ++op;
  // Ref will be released in the callback
  GRPC_CLOSURE_INIT(
      &on_recv_initial_metadata_, OnRecvInitialMetadata,
      this->Ref(DEBUG_LOCATION, "OnRecvInitialMetadata").release(), nullptr);
  call_error = grpc_call_start_batch_and_execute(
      call_, ops, static_cast<size_t>(op - ops), &on_recv_initial_metadata_);
  CHECK_EQ(call_error, GRPC_CALL_OK);
  // Start a batch for recv_trailing_metadata.
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv_;
  op->data.recv_status_on_client.status = &status_code_;
  op->data.recv_status_on_client.status_details = &status_details_;
  op->flags = 0;
  op->reserved = nullptr;
  ++op;
  // This callback signals the end of the call, so it relies on the initial
  // ref instead of a new ref. When it's invoked, it's the initial ref that is
  // unreffed.
  GRPC_CLOSURE_INIT(&on_status_received_, OnStatusReceived, this, nullptr);
  call_error = grpc_call_start_batch_and_execute(
      call_, ops, static_cast<size_t>(op - ops), &on_status_received_);
  CHECK_EQ(call_error, GRPC_CALL_OK);
  GRPC_CLOSURE_INIT(&on_response_received_, OnResponseReceived, this, nullptr);
}

GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall::
    ~GrpcStreamingCall() {
  grpc_metadata_array_destroy(&trailing_metadata_recv_);
  grpc_byte_buffer_destroy(send_message_payload_);
  grpc_byte_buffer_destroy(recv_message_payload_);
  CSliceUnref(status_details_);
  CHECK_NE(call_, nullptr);
  grpc_call_unref(call_);
}

void GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall::Orphan() {
  CHECK_NE(call_, nullptr);
  // If we are here because xds_client wants to cancel the call,
  // OnStatusReceived() will complete the cancellation and clean up.
  // Otherwise, we are here because xds_client has to orphan a failed call,
  // in which case the following cancellation will be a no-op.
  grpc_call_cancel_internal(call_);
  // Note that the initial ref is held by OnStatusReceived(), so the
  // corresponding unref happens there instead of here.
}

void GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall::SendMessage(
    std::string payload) {
  // Create payload.
  grpc_slice slice = grpc_slice_from_cpp_string(std::move(payload));
  send_message_payload_ = grpc_raw_byte_buffer_create(&slice, 1);
  CSliceUnref(slice);
  // Send the message.
  grpc_op op;
  memset(&op, 0, sizeof(op));
  op.op = GRPC_OP_SEND_MESSAGE;
  op.data.send_message.send_message = send_message_payload_;
  Ref(DEBUG_LOCATION, "OnRequestSent").release();
  grpc_call_error call_error =
      grpc_call_start_batch_and_execute(call_, &op, 1, &on_request_sent_);
  CHECK_EQ(call_error, GRPC_CALL_OK);
}

void GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall::
    StartRecvMessage() {
  Ref(DEBUG_LOCATION, "StartRecvMessage").release();
  grpc_op op;
  memset(&op, 0, sizeof(op));
  op.op = GRPC_OP_RECV_MESSAGE;
  op.data.recv_message.recv_message = &recv_message_payload_;
  CHECK_NE(call_, nullptr);
  const grpc_call_error call_error =
      grpc_call_start_batch_and_execute(call_, &op, 1, &on_response_received_);
  CHECK_EQ(call_error, GRPC_CALL_OK);
}

void GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall::
    OnRecvInitialMetadata(void* arg, grpc_error_handle /*error*/) {
  RefCountedPtr<GrpcStreamingCall> self(static_cast<GrpcStreamingCall*>(arg));
  grpc_metadata_array_destroy(&self->initial_metadata_recv_);
}

void GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall::
    OnRequestSent(void* arg, grpc_error_handle error) {
  RefCountedPtr<GrpcStreamingCall> self(static_cast<GrpcStreamingCall*>(arg));
  // Clean up the sent message.
  grpc_byte_buffer_destroy(self->send_message_payload_);
  self->send_message_payload_ = nullptr;
  // Invoke request handler.
  self->event_handler_->OnRequestSent(error.ok());
}

void GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall::
    OnResponseReceived(void* arg, grpc_error_handle /*error*/) {
  RefCountedPtr<GrpcStreamingCall> self(static_cast<GrpcStreamingCall*>(arg));
  // If there was no payload, then we received status before we received
  // another message, so we stop reading.
  if (self->recv_message_payload_ != nullptr) {
    // Process the response.
    grpc_byte_buffer_reader bbr;
    grpc_byte_buffer_reader_init(&bbr, self->recv_message_payload_);
    grpc_slice response_slice = grpc_byte_buffer_reader_readall(&bbr);
    grpc_byte_buffer_reader_destroy(&bbr);
    grpc_byte_buffer_destroy(self->recv_message_payload_);
    self->recv_message_payload_ = nullptr;
    self->event_handler_->OnRecvMessage(StringViewFromSlice(response_slice));
    CSliceUnref(response_slice);
  }
}

void GrpcXdsTransportFactory::GrpcXdsTransport::GrpcStreamingCall::
    OnStatusReceived(void* arg, grpc_error_handle /*error*/) {
  RefCountedPtr<GrpcStreamingCall> self(static_cast<GrpcStreamingCall*>(arg));
  self->event_handler_->OnStatusReceived(
      absl::Status(static_cast<absl::StatusCode>(self->status_code_),
                   StringViewFromSlice(self->status_details_)));
}

//
// GrpcXdsTransportFactory::GrpcXdsTransport::StateWatcher
//

class GrpcXdsTransportFactory::GrpcXdsTransport::StateWatcher final
    : public AsyncConnectivityStateWatcherInterface {
 public:
  explicit StateWatcher(RefCountedPtr<ConnectivityFailureWatcher> watcher)
      : watcher_(std::move(watcher)) {}

 private:
  void OnConnectivityStateChange(grpc_connectivity_state new_state,
                                 const absl::Status& status) override {
    if (new_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
      watcher_->OnConnectivityFailure(absl::Status(
          status.code(),
          absl::StrCat("channel in TRANSIENT_FAILURE: ", status.message())));
    }
  }

  RefCountedPtr<ConnectivityFailureWatcher> watcher_;
};

//
// GrpcXdsTransportFactory::GrpcXdsTransport
//

namespace {

RefCountedPtr<Channel> CreateXdsChannel(const ChannelArgs& args,
                                        const GrpcXdsServerInterface& server) {
  RefCountedPtr<grpc_channel_credentials> channel_creds =
      CoreConfiguration::Get().channel_creds_registry().CreateChannelCreds(
          server.channel_creds_config());
  RefCountedPtr<grpc_call_credentials> call_creds;
  for (const auto& call_creds_config : server.call_creds_configs()) {
    RefCountedPtr<grpc_call_credentials> creds =
        CoreConfiguration::Get().call_creds_registry().CreateCallCreds(
            call_creds_config);
    if (call_creds == nullptr) {
      call_creds = std::move(creds);
    } else {
      call_creds = MakeRefCounted<grpc_composite_call_credentials>(
          std::move(call_creds), std::move(creds));
    }
  }
  if (call_creds != nullptr) {
    channel_creds = MakeRefCounted<grpc_composite_channel_credentials>(
        std::move(channel_creds), std::move(call_creds));
  }
  return RefCountedPtr<Channel>(Channel::FromC(grpc_channel_create(
      server.server_uri().c_str(), channel_creds.get(), args.ToC().get())));
}

}  // namespace

GrpcXdsTransportFactory::GrpcXdsTransport::GrpcXdsTransport(
    WeakRefCountedPtr<GrpcXdsTransportFactory> factory,
    const XdsBootstrap::XdsServerTarget& server, absl::Status* status)
    : XdsTransport(GRPC_TRACE_FLAG_ENABLED(xds_client_refcount)
                       ? "GrpcXdsTransport"
                       : nullptr),
      factory_(std::move(factory)),
      key_(server.Key()) {
  GRPC_TRACE_LOG(xds_client, INFO)
      << "[GrpcXdsTransport " << this << "] created";
  channel_ = CreateXdsChannel(factory_->args_,
                              DownCast<const GrpcXdsServerInterface&>(server));
  CHECK(channel_ != nullptr);
  if (channel_->IsLame()) {
    *status = absl::UnavailableError("xds client has a lame channel");
  }
}

GrpcXdsTransportFactory::GrpcXdsTransport::~GrpcXdsTransport() {
  GRPC_TRACE_LOG(xds_client, INFO)
      << "[GrpcXdsTransport " << this << "] destroying";
}

void GrpcXdsTransportFactory::GrpcXdsTransport::Orphaned() {
  GRPC_TRACE_LOG(xds_client, INFO)
      << "[GrpcXdsTransport " << this << "] orphaned";
  {
    MutexLock lock(&factory_->mu_);
    auto it = factory_->transports_.find(key_);
    if (it != factory_->transports_.end() && it->second == this) {
      factory_->transports_.erase(it);
    }
  }
  // Do an async hop before unreffing.  This avoids a deadlock upon
  // shutdown in the case where the xDS channel is itself an xDS channel
  // (e.g., when using one control plane to find another control plane).
  grpc_event_engine::experimental::GetDefaultEventEngine()->Run(
      [self = WeakRefAsSubclass<GrpcXdsTransport>()]() mutable {
        ExecCtx exec_ctx;
        self.reset();
      });
}

void GrpcXdsTransportFactory::GrpcXdsTransport::StartConnectivityFailureWatch(
    RefCountedPtr<ConnectivityFailureWatcher> watcher) {
  if (channel_->IsLame()) return;
  auto* state_watcher = new StateWatcher(watcher);
  {
    MutexLock lock(&mu_);
    watchers_.emplace(watcher, state_watcher);
  }
  channel_->AddConnectivityWatcher(
      GRPC_CHANNEL_IDLE,
      OrphanablePtr<AsyncConnectivityStateWatcherInterface>(state_watcher));
}

void GrpcXdsTransportFactory::GrpcXdsTransport::StopConnectivityFailureWatch(
    const RefCountedPtr<ConnectivityFailureWatcher>& watcher) {
  if (channel_->IsLame()) return;
  StateWatcher* state_watcher = nullptr;
  {
    MutexLock lock(&mu_);
    auto it = watchers_.find(watcher);
    if (it == watchers_.end()) return;
    state_watcher = it->second;
    watchers_.erase(it);
  }
  channel_->RemoveConnectivityWatcher(state_watcher);
}

OrphanablePtr<XdsTransportFactory::XdsTransport::StreamingCall>
GrpcXdsTransportFactory::GrpcXdsTransport::CreateStreamingCall(
    const char* method,
    std::unique_ptr<StreamingCall::EventHandler> event_handler) {
  return MakeOrphanable<GrpcStreamingCall>(
      factory_.WeakRef(DEBUG_LOCATION, "StreamingCall"), channel_.get(), method,
      std::move(event_handler));
}

void GrpcXdsTransportFactory::GrpcXdsTransport::ResetBackoff() {
  channel_->ResetConnectionBackoff();
}

//
// GrpcXdsTransportFactory
//

namespace {

ChannelArgs ModifyChannelArgs(const ChannelArgs& args) {
  return args.Set(GRPC_ARG_KEEPALIVE_TIME_MS, Duration::Minutes(5).millis());
}

}  // namespace

GrpcXdsTransportFactory::GrpcXdsTransportFactory(const ChannelArgs& args)
    : args_(ModifyChannelArgs(args)),
      interested_parties_(grpc_pollset_set_create()) {
  // Calling grpc_init to ensure gRPC does not shut down until the XdsClient is
  // destroyed.
  InitInternally();
}

GrpcXdsTransportFactory::~GrpcXdsTransportFactory() {
  grpc_pollset_set_destroy(interested_parties_);
  // Calling grpc_shutdown to ensure gRPC does not shut down until the XdsClient
  // is destroyed.
  ShutdownInternally();
}

RefCountedPtr<XdsTransportFactory::XdsTransport>
GrpcXdsTransportFactory::GetTransport(
    const XdsBootstrap::XdsServerTarget& server, absl::Status* status) {
  std::string key = server.Key();
  RefCountedPtr<GrpcXdsTransport> transport;
  MutexLock lock(&mu_);
  auto it = transports_.find(key);
  if (it != transports_.end()) {
    transport = it->second->RefIfNonZero().TakeAsSubclass<GrpcXdsTransport>();
  }
  if (transport == nullptr) {
    transport = MakeRefCounted<GrpcXdsTransport>(
        WeakRefAsSubclass<GrpcXdsTransportFactory>(), server, status);
    transports_.emplace(std::move(key), transport.get());
  }
  return transport;
}

}  // namespace grpc_core
