/*
 *
 * Copyright 2018 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <grpc/support/port_platform.h>

#include "src/core/lib/iomgr/port.h"

#include <limits.h>
#include <string.h>

#include <grpc/slice_buffer.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/address_utils/sockaddr_utils.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/iomgr_custom.h"
#include "src/core/lib/iomgr/resource_quota.h"
#include "src/core/lib/iomgr/tcp_client.h"
#include "src/core/lib/iomgr/tcp_custom.h"
#include "src/core/lib/iomgr/tcp_server.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"

#define GRPC_TCP_DEFAULT_READ_SLICE_SIZE 8192

extern grpc_core::TraceFlag grpc_tcp_trace;

grpc_socket_vtable* grpc_custom_socket_vtable = nullptr;
extern grpc_tcp_server_vtable custom_tcp_server_vtable;
extern grpc_tcp_client_vtable custom_tcp_client_vtable;

void grpc_custom_endpoint_init(grpc_socket_vtable* impl) {
  grpc_custom_socket_vtable = impl;
  grpc_set_tcp_client_impl(&custom_tcp_client_vtable);
  grpc_set_tcp_server_impl(&custom_tcp_server_vtable);
}

struct custom_tcp_endpoint {
  grpc_endpoint base;
  gpr_refcount refcount;
  grpc_custom_socket* socket;

  grpc_closure* read_cb = nullptr;
  grpc_closure* write_cb = nullptr;

  grpc_slice_buffer* read_slices = nullptr;
  grpc_slice_buffer* write_slices = nullptr;

  grpc_slice_allocator* slice_allocator;

  bool shutting_down;

  std::string peer_string;
  std::string local_address;
};
static void tcp_free(grpc_custom_socket* s) {
  custom_tcp_endpoint* tcp =
      reinterpret_cast<custom_tcp_endpoint*>(s->endpoint);
  grpc_slice_allocator_destroy(tcp->slice_allocator);
  delete tcp;
  s->refs--;
  if (s->refs == 0) {
    grpc_custom_socket_vtable->destroy(s);
    gpr_free(s);
  }
}

#ifndef NDEBUG
#define TCP_UNREF(tcp, reason) tcp_unref((tcp), (reason), __FILE__, __LINE__)
#define TCP_REF(tcp, reason) tcp_ref((tcp), (reason), __FILE__, __LINE__)
static void tcp_unref(custom_tcp_endpoint* tcp, const char* reason,
                      const char* file, int line) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_atm val = gpr_atm_no_barrier_load(&tcp->refcount.count);
    gpr_log(file, line, GPR_LOG_SEVERITY_ERROR,
            "TCP unref %p : %s %" PRIdPTR " -> %" PRIdPTR, tcp->socket, reason,
            val, val - 1);
  }
  if (gpr_unref(&tcp->refcount)) {
    tcp_free(tcp->socket);
  }
}

static void tcp_ref(custom_tcp_endpoint* tcp, const char* reason,
                    const char* file, int line) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_atm val = gpr_atm_no_barrier_load(&tcp->refcount.count);
    gpr_log(file, line, GPR_LOG_SEVERITY_ERROR,
            "TCP   ref %p : %s %" PRIdPTR " -> %" PRIdPTR, tcp->socket, reason,
            val, val + 1);
  }
  gpr_ref(&tcp->refcount);
}
#else
#define TCP_UNREF(tcp, reason) tcp_unref((tcp))
#define TCP_REF(tcp, reason) tcp_ref((tcp))
static void tcp_unref(custom_tcp_endpoint* tcp) {
  if (gpr_unref(&tcp->refcount)) {
    tcp_free(tcp->socket);
  }
}

static void tcp_ref(custom_tcp_endpoint* tcp) { gpr_ref(&tcp->refcount); }
#endif

static void call_read_cb(custom_tcp_endpoint* tcp, grpc_error_handle error) {
  grpc_closure* cb = tcp->read_cb;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p call_cb %p %p:%p", tcp->socket, cb, cb->cb,
            cb->cb_arg);
    size_t i;
    gpr_log(GPR_INFO, "read: error=%s", grpc_error_std_string(error).c_str());
    for (i = 0; i < tcp->read_slices->count; i++) {
      char* dump = grpc_dump_slice(tcp->read_slices->slices[i],
                                   GPR_DUMP_HEX | GPR_DUMP_ASCII);
      gpr_log(GPR_INFO, "READ %p (peer=%s): %s", tcp, tcp->peer_string.c_str(),
              dump);
      gpr_free(dump);
    }
  }
  TCP_UNREF(tcp, "read");
  tcp->read_slices = nullptr;
  tcp->read_cb = nullptr;
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb, error);
}

static void custom_read_callback(grpc_custom_socket* socket, size_t nread,
                                 grpc_error_handle error) {
  grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
  grpc_core::ExecCtx exec_ctx;
  grpc_slice_buffer garbage;
  custom_tcp_endpoint* tcp =
      reinterpret_cast<custom_tcp_endpoint*>(socket->endpoint);
  if (error == GRPC_ERROR_NONE && nread == 0) {
    error = GRPC_ERROR_CREATE_FROM_STATIC_STRING("EOF");
  }
  if (error == GRPC_ERROR_NONE) {
    // Successful read
    if (nread < tcp->read_slices->length) {
      /* TODO(murgatroid99): Instead of discarding the unused part of the read
       * buffer, reuse it as the next read buffer. */
      grpc_slice_buffer_init(&garbage);
      grpc_slice_buffer_trim_end(tcp->read_slices,
                                 tcp->read_slices->length - nread, &garbage);
      grpc_slice_buffer_reset_and_unref_internal(&garbage);
    }
  } else {
    grpc_slice_buffer_reset_and_unref_internal(tcp->read_slices);
  }
  call_read_cb(tcp, error);
}

static void tcp_read_allocation_done(void* tcpp, grpc_error_handle error) {
  custom_tcp_endpoint* tcp = static_cast<custom_tcp_endpoint*>(tcpp);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "TCP:%p read_allocation_done: %s", tcp->socket,
            grpc_error_std_string(error).c_str());
  }
  if (error == GRPC_ERROR_NONE) {
    /* Before calling read, we allocate a buffer with exactly one slice
     * to tcp->read_slices and wait for the callback indicating that the
     * allocation was successful. So slices[0] should always exist here */
    char* buffer = reinterpret_cast<char*>(
        GRPC_SLICE_START_PTR(tcp->read_slices->slices[0]));
    size_t len = GRPC_SLICE_LENGTH(tcp->read_slices->slices[0]);
    grpc_custom_socket_vtable->read(tcp->socket, buffer, len,
                                    custom_read_callback);
  } else {
    grpc_slice_buffer_reset_and_unref_internal(tcp->read_slices);
    call_read_cb(tcp, GRPC_ERROR_REF(error));
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "Initiating read on %p: error=%s", tcp->socket,
            grpc_error_std_string(error).c_str());
  }
}

static void endpoint_read(grpc_endpoint* ep, grpc_slice_buffer* read_slices,
                          grpc_closure* cb, bool /*urgent*/) {
  custom_tcp_endpoint* tcp = reinterpret_cast<custom_tcp_endpoint*>(ep);
  GRPC_CUSTOM_IOMGR_ASSERT_SAME_THREAD();
  GPR_ASSERT(tcp->read_cb == nullptr);
  tcp->read_cb = cb;
  tcp->read_slices = read_slices;
  grpc_slice_buffer_reset_and_unref_internal(read_slices);
  TCP_REF(tcp, "read");
  if (grpc_slice_allocator_allocate(
          tcp->slice_allocator, GRPC_TCP_DEFAULT_READ_SLICE_SIZE, 1,
          grpc_slice_allocator_intent::kReadBuffer, tcp->read_slices,
          tcp_read_allocation_done, tcp)) {
    tcp_read_allocation_done(tcp, GRPC_ERROR_NONE);
  }
}

static void custom_write_callback(grpc_custom_socket* socket,
                                  grpc_error_handle error) {
  grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
  grpc_core::ExecCtx exec_ctx;
  custom_tcp_endpoint* tcp =
      reinterpret_cast<custom_tcp_endpoint*>(socket->endpoint);
  grpc_closure* cb = tcp->write_cb;
  tcp->write_cb = nullptr;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "write complete on %p: error=%s", tcp->socket,
            grpc_error_std_string(error).c_str());
  }
  TCP_UNREF(tcp, "write");
  grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb, error);
}

static void endpoint_write(grpc_endpoint* ep, grpc_slice_buffer* write_slices,
                           grpc_closure* cb, void* /*arg*/) {
  custom_tcp_endpoint* tcp = reinterpret_cast<custom_tcp_endpoint*>(ep);
  GRPC_CUSTOM_IOMGR_ASSERT_SAME_THREAD();

  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    size_t j;

    for (j = 0; j < write_slices->count; j++) {
      char* data = grpc_dump_slice(write_slices->slices[j],
                                   GPR_DUMP_HEX | GPR_DUMP_ASCII);
      gpr_log(GPR_INFO, "WRITE %p (peer=%s): %s", tcp->socket,
              tcp->peer_string.c_str(), data);
      gpr_free(data);
    }
  }

  if (tcp->shutting_down) {
    grpc_core::ExecCtx::Run(
        DEBUG_LOCATION, cb,
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("TCP socket is shutting down"));
    return;
  }

  GPR_ASSERT(tcp->write_cb == nullptr);
  tcp->write_slices = write_slices;
  GPR_ASSERT(tcp->write_slices->count <= UINT_MAX);
  if (tcp->write_slices->count == 0) {
    // No slices means we don't have to do anything,
    // and libuv doesn't like empty writes
    grpc_core::ExecCtx::Run(DEBUG_LOCATION, cb, GRPC_ERROR_NONE);
    return;
  }
  tcp->write_cb = cb;
  TCP_REF(tcp, "write");
  grpc_custom_socket_vtable->write(tcp->socket, tcp->write_slices,
                                   custom_write_callback);
}

static void endpoint_add_to_pollset(grpc_endpoint* ep, grpc_pollset* pollset) {
  // No-op. We're ignoring pollsets currently
  (void)ep;
  (void)pollset;
}

static void endpoint_add_to_pollset_set(grpc_endpoint* ep,
                                        grpc_pollset_set* pollset) {
  // No-op. We're ignoring pollsets currently
  (void)ep;
  (void)pollset;
}

static void endpoint_delete_from_pollset_set(grpc_endpoint* ep,
                                             grpc_pollset_set* pollset) {
  // No-op. We're ignoring pollsets currently
  (void)ep;
  (void)pollset;
}

static void endpoint_shutdown(grpc_endpoint* ep, grpc_error_handle why) {
  custom_tcp_endpoint* tcp = reinterpret_cast<custom_tcp_endpoint*>(ep);
  if (!tcp->shutting_down) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
      gpr_log(GPR_INFO, "TCP %p shutdown why=%s", tcp->socket,
              grpc_error_std_string(why).c_str());
    }
    tcp->shutting_down = true;
    // grpc_core::ExecCtx::Run(DEBUG_LOCATION,tcp->read_cb,
    // GRPC_ERROR_REF(why));
    // grpc_core::ExecCtx::Run(DEBUG_LOCATION,tcp->write_cb,
    // GRPC_ERROR_REF(why)); tcp->read_cb = nullptr; tcp->write_cb = nullptr;
    grpc_custom_socket_vtable->shutdown(tcp->socket);
  }
  GRPC_ERROR_UNREF(why);
}

static void custom_close_callback(grpc_custom_socket* socket) {
  socket->refs--;
  if (socket->refs == 0) {
    grpc_custom_socket_vtable->destroy(socket);
    gpr_free(socket);
  } else if (socket->endpoint) {
    grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
    grpc_core::ExecCtx exec_ctx;
    custom_tcp_endpoint* tcp =
        reinterpret_cast<custom_tcp_endpoint*>(socket->endpoint);
    TCP_UNREF(tcp, "destroy");
  }
}

static void endpoint_destroy(grpc_endpoint* ep) {
  custom_tcp_endpoint* tcp = reinterpret_cast<custom_tcp_endpoint*>(ep);
  grpc_custom_socket_vtable->close(tcp->socket, custom_close_callback);
}

static absl::string_view endpoint_get_peer(grpc_endpoint* ep) {
  custom_tcp_endpoint* tcp = reinterpret_cast<custom_tcp_endpoint*>(ep);
  return tcp->peer_string;
}

static absl::string_view endpoint_get_local_address(grpc_endpoint* ep) {
  custom_tcp_endpoint* tcp = reinterpret_cast<custom_tcp_endpoint*>(ep);
  return tcp->local_address;
}

static int endpoint_get_fd(grpc_endpoint* /*ep*/) { return -1; }

static bool endpoint_can_track_err(grpc_endpoint* /*ep*/) { return false; }

static grpc_endpoint_vtable vtable = {endpoint_read,
                                      endpoint_write,
                                      endpoint_add_to_pollset,
                                      endpoint_add_to_pollset_set,
                                      endpoint_delete_from_pollset_set,
                                      endpoint_shutdown,
                                      endpoint_destroy,
                                      endpoint_get_peer,
                                      endpoint_get_local_address,
                                      endpoint_get_fd,
                                      endpoint_can_track_err};

grpc_endpoint* custom_tcp_endpoint_create(grpc_custom_socket* socket,
                                          grpc_slice_allocator* slice_allocator,
                                          const char* peer_string) {
  custom_tcp_endpoint* tcp = new custom_tcp_endpoint;
  grpc_core::ApplicationCallbackExecCtx callback_exec_ctx;
  grpc_core::ExecCtx exec_ctx;

  if (GRPC_TRACE_FLAG_ENABLED(grpc_tcp_trace)) {
    gpr_log(GPR_INFO, "Creating TCP endpoint %p", socket);
  }
  socket->refs++;
  socket->endpoint = reinterpret_cast<grpc_endpoint*>(tcp);
  tcp->socket = socket;
  tcp->base.vtable = &vtable;
  gpr_ref_init(&tcp->refcount, 1);
  tcp->peer_string = peer_string;
  grpc_resolved_address resolved_local_addr;
  resolved_local_addr.len = sizeof(resolved_local_addr.addr);
  if (grpc_custom_socket_vtable->getsockname(
          socket, reinterpret_cast<sockaddr*>(resolved_local_addr.addr),
          reinterpret_cast<int*>(&resolved_local_addr.len)) !=
      GRPC_ERROR_NONE) {
    tcp->local_address = "";
  } else {
    tcp->local_address = grpc_sockaddr_to_uri(&resolved_local_addr);
  }
  tcp->shutting_down = false;
  tcp->slice_allocator = slice_allocator;
  return &tcp->base;
}
