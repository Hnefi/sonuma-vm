/*
 * Scale-Out NUMA Open Source License
 *
 * Copyright (c) 2017, Parallel Systems Architecture Lab, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:

 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * * Neither the name of the Parallel Systems Architecture Lab, EPFL,
 *   nor the names of its contributors may be used to endorse or promote
 *   products derived from this software without specific prior written
 *   permission.

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
 *
 *  Authors: 
 *  	Stanko Novakovic <stanko.novakovic@epfl.ch>
 */

#ifndef H_SONUMA
#define H_SONUMA

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>

#include "RMCdefines.h"

#ifdef DEBUG
#define DLog(M, ...) fprintf(stdout, "DEBUG %s:%d: " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DLog(M, ...)
#endif

#ifdef DEBUG_PERF
#define DLogPerf(M, ...) fprintf(stdout, "DEBUG %s:%d: " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DLogPerf(M, ...)
#endif

typedef void (async_handler)(uint8_t tid, wq_entry_t *head, void *owner);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This func opens connection with kernel driver (KAL).
 */
int kal_open(char *kal_name);

/**
 * This func registers WQ with KAL or Flexus.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk (pins only for Flexus)
 */
int kal_reg_wq(int fd, rmc_wq_t **wq_ptr);

/**
 * This func registers CQ with KAL or Flexus.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk (pins only for Flexus)
 */
int kal_reg_cq(int fd, rmc_cq_t **cq_ptr);

/**
 * This func registers local buffer with KAL or Flexus.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk (only for Flexus); allocation is done within an app
 */
int kal_reg_lbuff(int fd, uint8_t **buff_ptr, uint32_t num_pages);

/**
 * This func registers context buffer with KAL or Flexus.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk (only for Flexus); allocation is done within an app
 */
int kal_reg_ctx(int fd, uint8_t **ctx_ptr, uint32_t num_pages);


/**
 * This func implements the receive functionality for solicited communication
 */
int rmc_send(rmc_wq_t *wq, rmc_cq_t *cq, char *ctx, char *lbuff_ptr,
	     int lbuff_offset, char *data, int size, int snid);

/**
 * This func implements the receive functionality for solicited communication
 */
int rmc_recv(rmc_wq_t *wq, rmc_cq_t *cq, char *ctx, char *lbuff_ptr,
	     int lbuff_offset, int snid, char *data, int size);

#ifdef __cplusplus
}
#endif

//inline methods

static inline void rmc_rread_sync(rmc_wq_t *wq, rmc_cq_t *cq, uint64_t lbuff_slot, int snid,
				  uint32_t ctx_id, uint64_t ctx_offset, uint64_t length)
{
  uint8_t wq_head = wq->head;
  uint8_t cq_tail = cq->tail;
  
  DLogPerf("[rmc_rread_sync] rmc_rread_sync called.");
  
  wq->q[wq_head].buf_addr = lbuff_slot;
  wq->q[wq_head].cid = ctx_id;
  wq->q[wq_head].offset = ctx_offset;
  if(length < 64)
    wq->q[wq_head].length = 64; //at least 64B
  else
    wq->q[wq_head].length = length; //specify the length of the transfer
  wq->q[wq_head].op = 'r';
  wq->q[wq_head].nid = snid;

  wq->q[wq_head].valid = 1;
  wq->q[wq_head].SR = wq->SR;

  wq->head =  wq->head + 1;

  // check if WQ reached its end
  if (wq->head >= MAX_NUM_WQ) {
    wq->head = 0;
    wq->SR ^= 1;
  }
  
  // wait for a completion of the entry
  while(cq->q[cq_tail].SR != cq->SR) {
  }
  
  // mark the entry as invalid, i.e. completed
  wq->q[cq->q[cq_tail].tid].valid = 0;
  
  cq->tail = cq->tail + 1;

  // check if WQ reached its end
  if (cq->tail >= MAX_NUM_WQ) {
    cq->tail = 0;
    cq->SR ^= 1;
  }

}

static inline void rmc_rwrite_sync(rmc_wq_t *wq, rmc_cq_t *cq, uint64_t lbuff_slot,
				   int snid, uint32_t ctx_id, uint64_t ctx_offset,
				   uint64_t length) {
  uint8_t wq_head = wq->head;
  uint8_t cq_tail = cq->tail;
  
  while (wq->q[wq_head].valid) {} // wait for WQ head to be ready
  
  DLogPerf("[sonuma] rmc_rwrite_sync called in VM mode.");
  
  wq->q[wq_head].buf_addr = lbuff_slot;
  wq->q[wq_head].cid = ctx_id;
  wq->q[wq_head].offset = ctx_offset;
  if(length < 64)
    wq->q[wq_head].length = 64; //at least 64B
  else
    wq->q[wq_head].length = length; //specify the length of the transfer
  wq->q[wq_head].op = 'w';
  wq->q[wq_head].nid = snid;
  //soNUMA v2.1
  wq->q[wq_head].valid = 1;
  wq->q[wq_head].SR = wq->SR;
  
  wq->head =  wq->head + 1;
  // check if WQ reached its end
  if (wq->head >= MAX_NUM_WQ) {
    wq->head = 0;
    wq->SR ^= 1;
  }

  // wait for a completion of the entry
  while(cq->q[cq_tail].SR != cq->SR) {
  }

  // mark the entry as invalid, i.e. completed
  wq->q[cq->q[cq_tail].tid].valid = 0;
  
  cq->tail = cq->tail + 1;
  
  // check if WQ reached its end
  if (cq->tail >= MAX_NUM_WQ) {
    cq->tail = 0;
    cq->SR ^= 1;
  }
}

//CAUTION: make sure you call rmc_check_cq() before this function
static inline void rmc_rread_async(rmc_wq_t *wq, uint64_t lbuff_slot, int snid,
				   uint32_t ctx_id, uint64_t ctx_offset, uint64_t length)
{
  //uint8_t wq_head = wq->head;
  
  DLogPerf("[sonuma] rmc_rread_async called in VM mode.");
  //TODO: insert a new WQ entry
  //create_wq_entry_emu(wq, lbuff_slot, snid, ctx_id, ctx_offset, length);
  
  wq->head =  wq->head + 1;
  // check if WQ reached its end
  if (wq->head >= MAX_NUM_WQ) {
    wq->head = 0;
    wq->SR ^= 1;
  }
}

//CAUTION: make sure you call rmc_check_cq() before this function
static inline void rmc_rwrite_async(rmc_wq_t *wq, uint64_t lbuff_slot, int snid,
				    uint32_t ctx_id, uint64_t ctx_offset, uint64_t length) {
  //uint8_t wq_head = wq->head;
  
  DLogPerf("[sonuma] rmc_rwrite_async called in VM mode.");
  //create_wq_entry_emu(wq, lbuff_slot, snid, ctx_id, ctx_offset, length);

  wq->head =  wq->head + 1;
  // check if WQ reached its end
  if (wq->head >= MAX_NUM_WQ) {
      wq->head = 0;
      wq->SR ^= 1;
  }
}

#endif /* H_SONUMA */
