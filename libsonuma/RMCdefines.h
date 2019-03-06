/*
 * Scale-Out NUMA Open Source License
 *
 * Copyright (c) 2017, Parallel Systems Architecture Lab, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * * Neither the name of the Parallel Systems Architecture Lab, EPFL,
 *   nor the names of its contributors may be used to endorse or promote
 *   products derived from this software without specific prior written
 *   permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE PARALLEL SYSTEMS ARCHITECTURE LAB,
 * EPFL BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  soNUMA library functions
 */

#ifndef H_RMC_DEFINES
#define H_RMC_DEFINES

#define MAX_NUM_WQ 64
#define MAX_NUM_SRQ_SLOTS 256 // Msutherl: probably needs to be bigger

#define RPC_DATA_PAYLOAD 4096
#define MAX_RPC_BYTES (RPC_DATA_PAYLOAD)// + HEADER_DATA_BYTES)

#define KAL_REG_WQ      1
#define KAL_UNREG_WQ    6
#define KAL_REG_CQ      5
#define KAL_REG_CTX     3
#define KAL_PIN_BUFF    4
#define KAL_PIN         14

#define PAGE_SIZE 4096
#define MSGS_PER_PAIR 16

#include <stdbool.h>

#ifdef __cplusplus
    #include <cstdint>
#else
    #include <stdint.h>
#endif


typedef struct wq_entry {
  uint8_t op;
  volatile uint8_t SR;
    //set with a new WQ entry, unset when entry completed.
    //Required for pipelining async ops.
  volatile uint8_t valid;
  uint64_t buf_addr;
  uint64_t buf_offset;
  uint16_t cid;
  uint16_t nid;
  uint64_t offset;
  uint64_t length;
  /* Msutherl: */
    uint16_t slot_idx;
    uint16_t qp_num_at_receiver;
    bool send_qp_terminate;
    bool dispatch_on_recv;
} wq_entry_t;

typedef struct cq_entry { 
  volatile uint8_t SR;
  volatile uint8_t tid;
  /* Msutherl: */
  uint16_t sending_nid;
  uint16_t sending_qp;
  uint64_t slot_idx;
  uint64_t length;
  bool is_nack;
} cq_entry_t;

typedef struct rmc_wq {
  wq_entry_t q[MAX_NUM_WQ];
  uint8_t head;
  volatile uint8_t SR;
  volatile bool connected;
} rmc_wq_t;

typedef struct rmc_cq {
  cq_entry_t q[MAX_NUM_WQ];
  uint8_t tail;
  volatile uint8_t SR;
  volatile bool connected;
} rmc_cq_t;

typedef struct qp_info {
  int node_cnt;
  int this_nid;
} qp_info_t;

typedef struct sslot {
    volatile bool valid;
    uint64_t msg_size;
    uint16_t sending_qp;
    uint16_t wq_entry_idx;
} send_slot_t;

/* Msutherl */
typedef struct rpc_srq_entry {
    // all metadata used for making the CQ entry later upon dispatch
    volatile uint8_t tid;
    uint16_t sending_nid;
    uint16_t sending_qp;
    uint64_t slot_idx;
    uint64_t length;
    bool valid;
} rpc_srq_entry_t;

/* Msutherl */
typedef struct rpc_srq {
    rpc_srq_entry_t q[MAX_NUM_SRQ_SLOTS];
    // head-tail indices
    uint16_t head, tail;
    uint16_t dispatch_slot;
    bool full;
} rpc_srq_t; 

#endif /* H_RMC_DEFINES */
