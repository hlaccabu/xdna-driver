// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2023-2025, Advanced Micro Devices, Inc. All rights reserved.

#ifndef HWCTX_UMQ_H
#define HWCTX_UMQ_H

#include "../hwctx.h"
#include "../buffer.h"

namespace shim_xdna {

class hwctx_umq : public hwctx {
public:
  hwctx_umq(const device& device, const xrt::xclbin& xclbin, const qos_type& qos);
  ~hwctx_umq();

private:
  std::unique_ptr<buffer> m_log_bo;
  uint32_t m_col_cnt = 0;

  enum umq_log_flag {
    UMQ_DEBUG_BUFFER = 0,
    UMQ_TRACE_BUFFER,
    UMQ_DBG_QUEUE,
    UMQ_LOG_BUFFER
  };

  struct umq_log_metadata {
    uint32_t magic_no;		// 0x43455254
    uint8_t major;
    uint8_t minor;
    uint8_t umq_log_flag;
    uint8_t num_ucs;		// ucs to config, up to 6 for npu3
    struct {
      uint64_t paddr:57;	// device accessible address array for each valid uc
      uint64_t index:7;		// uc index is mapped as 0 -> 0_A, 1 -> 0_B, 2 -> 1_A, 
				// 3 -> 1_B, 4 -> 2_A, 5 -> 2_B
      uint32_t size;		// buf size in words for this uc
    } uc_info[];
  };

  struct umq_log_metadata m_metadata;
  void *m_log_buf = nullptr;

  void init_log_buf();
  void fini_log_buf();
  void set_metadata(int num_cols, size_t size, uint64_t bo_paddr, enum umq_log_flag flag);
};

}

#endif
