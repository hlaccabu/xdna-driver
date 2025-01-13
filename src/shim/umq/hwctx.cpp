// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2023-2024, Advanced Micro Devices, Inc. All rights reserved.

#include "bo.h"
#include "hwctx.h"
#include "hwq.h"

#include "core/common/config_reader.h"
#include "core/common/memalign.h"

namespace shim_xdna {

hw_ctx_umq::
hw_ctx_umq(const device& device, const xrt::xclbin& xclbin, const xrt::hw_context::qos_type& qos)
  : hw_ctx(device, qos, std::make_unique<hw_q_umq>(device, 8), xclbin)
{
  hw_ctx_umq::init_log_buf();
  hw_ctx::create_ctx_on_device();

  shim_debug("Created UMQ HW context (%d)", get_slotidx());
}

hw_ctx_umq::
~hw_ctx_umq()
{
  shim_debug("Destroying UMQ HW context (%d)...", get_slotidx());
}

std::unique_ptr<xrt_core::buffer_handle>
hw_ctx_umq::
alloc_bo(void* userptr, size_t size, uint64_t flags)
{
  // const_cast: alloc_bo() is not const yet in device class
  auto& dev = const_cast<device&>(get_device());

  return dev.alloc_bo(userptr, AMDXDNA_INVALID_CTX_HANDLE, size, flags);
}

void
hw_ctx_umq::
set_metadata(int num_cols, size_t size, uint64_t bo_paddr, uint8_t flag)
{
  m_metadata.magic_no = UMQ_MAGIC_NO;
  m_metadata.major = 0;
  m_metadata.minor = 1;
  m_metadata.umq_log_flag = flag;
  m_metadata.num_cols = num_cols;
  for (int i = 0; i < num_cols; i++) {
    m_metadata.col_paddr[i] = bo_paddr + size * i + sizeof(m_metadata);
    m_metadata.col_size[i] = size;
  }
}

void
hw_ctx_umq::
init_log_buf()
{
  size_t column_size = 1024;
  auto log_buf_size = m_num_cols * column_size + sizeof(m_metadata);
  m_log_bo = alloc_bo(nullptr, log_buf_size, XCL_BO_FLAGS_EXECBUF);
  m_log_buf = m_log_bo->map(bo::map_type::write);
  uint64_t bo_paddr = m_log_bo->get_properties().paddr;
  set_metadata(m_num_cols, column_size, bo_paddr, 1);
  std::memset(m_log_buf, 0, log_buf_size);
  std::memcpy(m_log_buf, &m_metadata, sizeof(m_metadata));
}

} // shim_xdna
