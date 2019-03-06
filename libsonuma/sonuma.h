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
#define RMC_DEV "/dev/sonuma_rmc"

#ifdef DEBUG_RMC
#define DLog(M, ...) fprintf(stdout, "DEBUG %s:%d: " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define DLogNoVar(M) fprintf(stdout, "DEBUG %s:%d: " M "\n", __FILE__, __LINE__)
#else
#define DLog(M, ...)
#define DLogNoVar(M)
#endif

#ifdef DEBUG_PERF
#define DLogPerf(M, ...) fprintf(stdout, "DEBUG %s:%d: " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define DLogPerf(M, ...)
#endif

typedef struct rpcArgument {
    uint16_t sending_nid;
    cq_entry_t* head;
    void* pointerToAppData; // cast me
    bool is_nack;
} rpcArg_t;

typedef void (async_handler)(uint8_t tid, wq_entry_t *head, void *owner);
typedef void (rpc_handler)(uint16_t sending_nid, char* recv_slot, cq_entry_t *head, void *owner);
typedef void (receiveCallback)(uint8_t* rawRecvBufferPtr, rpcArg_t* argPointer);

/* Helper */
// Shamelessly imported from: https://gist.github.com/ccbrown/9722406#file-dumphex-c
// - thanks @ccbrown! : https://github.com/ccbrown
//
static void DumpHex(const void* data, size_t size) {
	char ascii[17];
	size_t i, j;
	ascii[16] = '\0';
	for (i = 0; i < size; ++i) {
		printf("%02X ", ((unsigned char*)data)[i]);
		if (((unsigned char*)data)[i] >= ' ' && ((unsigned char*)data)[i] <= '~') {
			ascii[i % 16] = ((unsigned char*)data)[i];
		} else {
			ascii[i % 16] = '.';
		}
		if ((i+1) % 8 == 0 || i+1 == size) {
			printf(" ");
			if ((i+1) % 16 == 0) {
				printf("|  %s \n", ascii);
			} else if (i+1 == size) {
				ascii[(i+1) % 16] = '\0';
				if ((i+1) % 16 <= 8) {
					printf(" ");
				}
				for (j = (i+1) % 16; j < 16; ++j) {
					printf("   ");
				}
				printf("|  %s \n", ascii);
			}
		}
	}
}


/**
 * This func opens connection with kernel driver (KAL).
 */
int kal_open(char *kal_name);

/**
 * This func registers WQ with KAL.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk.
 */
int kal_reg_wq(int fd, rmc_wq_t **wq_ptr, int wq_id);

/**
 * This func registers CQ with KAL.
 * Warning: it allocates memory for WQ and pins the memory
 *          to avoid swapping to the disk.
 */
int kal_reg_cq(int fd, rmc_cq_t **cq_ptr, int cq_id);

/**
 * This func registers local buffer with KAL.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk.
 */
//int kal_reg_lbuff(int fd, uint8_t **buff_ptr, uint32_t num_pages,int lb_id);
int kal_reg_lbuff(int fd, uint8_t **buff_ptr, const char* lb_name, uint32_t num_pages);

/**
 * This func registers context buffer with KAL.
 * Warning: the func pins the memory to avoid swapping to
 *          the disk.
 */
int kal_reg_ctx(int fd, uint8_t **ctx_ptr, uint32_t num_pages);

/* Msutherl: beta-implementations for send/recv. */
void rmc_recv(rmc_wq_t *wq,int snid,uint16_t sending_qp,uint16_t slot_idx,bool dispatch);

/* Msutherl: New version of rmc_send, using paired send/recv slots */
void rmc_send(rmc_wq_t *wq, char *lbuff_ptr, int lbuff_offset, size_t size, int snid, uint16_t sending_qp,bool send_qp_terminate);

//inline methods
static inline void rmc_rread_sync(rmc_wq_t *wq, rmc_cq_t *cq, uint8_t *lbuff_base,
				  uint64_t lbuff_offset, int snid, uint32_t ctx_id,
				  uint64_t ctx_offset, uint64_t length)
{
  uint8_t wq_head = wq->head;
  uint8_t cq_tail = cq->tail;
  
  DLogPerf("[rmc_rread_sync] rmc_rread_sync called, snid = %d.",snid);

  while (wq->q[wq_head].valid) {} //wait for WQ head to be ready

  wq->q[wq_head].buf_addr = (uint64_t)lbuff_base;
  wq->q[wq_head].buf_offset = lbuff_offset;
  wq->q[wq_head].cid = ctx_id;
  wq->q[wq_head].offset = ctx_offset;
  if(length < 64)
    wq->q[wq_head].length = 64; //at least 64B
  else
    wq->q[wq_head].length = length;
  wq->q[wq_head].op = 'r';
  wq->q[wq_head].nid = snid;

  wq->q[wq_head].valid = 1;
  wq->q[wq_head].SR = wq->SR;

  wq->head =  wq->head + 1;

  //check if WQ reached its end
  if (wq->head >= MAX_NUM_WQ) {
    wq->head = 0;
    wq->SR ^= 1;
  }
  
  //wait for a completion of the entry
  while(cq->q[cq_tail].SR != cq->SR) {
  }
  
  //mark the entry as invalid, i.e. completed
  wq->q[cq->q[cq_tail].tid].valid = 0;
  
  cq->tail = cq->tail + 1;

  //check if WQ reached its end
  if (cq->tail >= MAX_NUM_WQ) {
    cq->tail = 0;
    cq->SR ^= 1;
  }
}

static inline void rmc_rwrite_sync(rmc_wq_t *wq, rmc_cq_t *cq, uint8_t *lbuff_base,
				   uint64_t lbuff_offset, int snid, uint32_t ctx_id,
				   uint64_t ctx_offset, uint64_t length)
{
  uint8_t wq_head = wq->head;
  uint8_t cq_tail = cq->tail;
  
  while (wq->q[wq_head].valid) {} //wait for WQ head to be ready
  
  DLogPerf("[sonuma] rmc_rwrite_sync called, snid = %d.",snid);

  wq->q[wq_head].buf_addr = (uint64_t)lbuff_base;
  wq->q[wq_head].buf_offset = lbuff_offset;
  wq->q[wq_head].cid = ctx_id;
  wq->q[wq_head].offset = ctx_offset;
  if(length < 64)
    wq->q[wq_head].length = 64; //at least 64B
  else
    wq->q[wq_head].length = length;
  wq->q[wq_head].op = 'w';
  wq->q[wq_head].nid = snid;

  wq->q[wq_head].valid = 1;
  wq->q[wq_head].SR = wq->SR;
  
  wq->head =  wq->head + 1;

  //check if WQ reached its end
  if (wq->head >= MAX_NUM_WQ) {
    wq->head = 0;
    wq->SR ^= 1;
  }

  //wait for a completion of the entry
  while(cq->q[cq_tail].SR != cq->SR) {
  }

  //mark the entry as invalid
  wq->q[cq->q[cq_tail].tid].valid = 0;
  
  cq->tail = cq->tail + 1;
  
  //check if WQ reached its end
  if (cq->tail >= MAX_NUM_WQ) {
    cq->tail = 0;
    cq->SR ^= 1;
  }
}

//CAUTION: make sure you call rmc_check_cq() before this function
static inline void rmc_rread_async(rmc_wq_t *wq, uint8_t *lbuff_base, uint64_t lbuff_offset,int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length)
{
  DLogPerf("[sonuma] rmc_rread_async called, snid = %d",snid);
  
  uint8_t wq_head = wq->head;
  
  wq->q[wq_head].buf_addr = (uint64_t)lbuff_base;
  wq->q[wq_head].buf_offset = lbuff_offset;
  wq->q[wq_head].cid = ctx_id;
  wq->q[wq_head].offset = ctx_offset;
  if(length < 64)
    wq->q[wq_head].length = 64; //at least 64B
  else
    wq->q[wq_head].length = length;
  wq->q[wq_head].op = 'r';
  wq->q[wq_head].nid = snid;

  wq->q[wq_head].valid = 1;
  wq->q[wq_head].SR = wq->SR;
  
  wq->head =  wq->head + 1;
  
  //check if WQ reached its end
  if (wq->head >= MAX_NUM_WQ) {
    wq->head = 0;
    wq->SR ^= 1;
  }
}

//CAUTION: make sure you call rmc_check_cq() before this function
static inline void rmc_rwrite_async(rmc_wq_t *wq, uint8_t *lbuff_base, uint64_t lbuff_offset,
				    int snid, uint32_t ctx_id, uint64_t ctx_offset, uint64_t length)
{  
  DLogPerf("[sonuma] rmc_rwrite_async called, snid = %d",snid);
  
  uint8_t wq_head = wq->head;

  wq->q[wq_head].buf_addr = (uint64_t)lbuff_base;
  wq->q[wq_head].buf_offset = lbuff_offset;
  wq->q[wq_head].cid = ctx_id;
  wq->q[wq_head].offset = ctx_offset;
  if(length < 64)
    wq->q[wq_head].length = 64; //at least 64B
  else
    wq->q[wq_head].length = length;
  wq->q[wq_head].op = 'w';
  wq->q[wq_head].nid = snid;

  wq->q[wq_head].valid = 1;
  wq->q[wq_head].SR = wq->SR;

  wq->head =  wq->head + 1;
  
  //check if WQ reached its end
  if (wq->head >= MAX_NUM_WQ) {
      wq->head = 0;
      wq->SR ^= 1;
  }
}

static inline int rmc_check_cq(rmc_wq_t *wq, rmc_cq_t *cq, async_handler *handler, void *owner)
{
  uint8_t tid;
  uint8_t wq_head = wq->head;
  uint8_t cq_tail = cq->tail;

  //in the outer loop we wait for a free entry in the WQ head
  do { 
    //in the inner loop we iterate over completed entries in the CQ
    while (cq->q[cq_tail].SR == cq->SR) {
      tid = cq->q[cq_tail].tid;
      wq->q[tid].valid = 0;
      
      cq->tail = cq->tail + 1;

      //check if WQ reached its end
      if (cq->tail >= MAX_NUM_WQ) {
	cq->tail = 0;
	cq->SR ^= 1;
      }

      cq_tail = cq->tail;
      
      handler(tid, &(wq->q[tid]), owner);
    }
  } while (wq->q[wq_head].valid);
  
  return 0;
}

static inline int rmc_drain_cq(rmc_wq_t *wq, rmc_cq_t *cq, async_handler *handler, void *owner)
{
  uint8_t tid;
  uint8_t cq_tail = cq->tail;
  
  while(cq->q[cq_tail].SR == cq->SR) {
    tid = cq->q[cq_tail].tid;
    wq->q[tid].valid = 0;
    
    cq->tail = cq->tail + 1;
    
    // check if WQ reached its end
    if (cq->tail >= MAX_NUM_WQ) {
      cq->tail = 0;
      cq->SR ^= 1;
    }
    
    cq_tail = cq->tail;
    
    handler(tid, &(wq->q[tid]), owner);
  }
  
  return 0;
}

static inline void rmc_poll_cq_rpc(rmc_cq_t* cq, uint8_t* recv_slot_base, receiveCallback* theRPC, uint16_t* sending_nid, uint16_t* sending_qp,uint16_t* slot_idx,void* argPointerHack)
{
    uint8_t cq_tail = cq->tail;

    DLog("Polling CQ[%d].SR = %d. CQ->SR = %d\n",
            cq_tail, cq->q[cq_tail].SR, cq->SR);
    // wait for entry to arrive in cq
    while(cq->q[cq_tail].SR != cq->SR ) { }
    DLog("Valid entry in CQ (index %d)! Entry SR = %d, Q. SR = %d. recv. bufs index = %d\n",cq_tail,cq->q[cq_tail].SR,cq->SR,cq->q[cq_tail].slot_idx);
    // call handler and set nid for sending wq in return
    *sending_nid = cq->q[cq_tail].sending_nid;
    *sending_qp = cq->q[cq_tail].sending_qp;
    *slot_idx = cq->q[cq_tail].slot_idx;

    uint8_t* rpc_recv_slot = recv_slot_base + ((*slot_idx) * MAX_RPC_BYTES);
    // marshal rpc structure
    rpcArg_t args;
    args.sending_nid = *sending_nid;
    args.head = &(cq->q[cq_tail]);
    args.pointerToAppData = argPointerHack;
    args.is_nack = cq->q[cq_tail].is_nack ;

#ifdef PRINT_BUFS
    DLog("About to call back to the RPC handler itself. Sending NID: %d, slot_idx: %d, length: %d",*sending_nid, *slot_idx, cq->q[cq_tail].length);
    DumpHex( (rpc_recv_slot ), cq->q[cq_tail].length );
#endif
    theRPC((rpc_recv_slot ), &args);

    cq->tail = cq->tail + 1;
    //check if CQ reached its end
    if (cq->tail >= MAX_NUM_WQ) {
        cq->tail = 0;
        cq->SR ^= 1;
    }
}

static inline void rmc_test_cq_rpc(rmc_cq_t* cq, uint8_t* recv_slot_base, receiveCallback* theRPC,int* sending_nid, uint16_t* sending_qp,uint16_t* slot_idx)
{
  uint8_t cq_tail = cq->tail;

  // wait for entry to arrive in cq
  if(cq->q[cq_tail].SR == cq->SR ) { 
      DLog("Valid entry in CQ (index %d)! Entry SR = %d, Q. SR = %d. recv_buf index = %d\n",cq_tail,cq->q[cq_tail].SR,cq->SR,cq->q[cq_tail].slot_idx);
      // call handler and set nid for sending wq in return
      *sending_nid = cq->q[cq_tail].sending_nid;
      *sending_qp = cq->q[cq_tail].sending_qp;
      *slot_idx = cq->q[cq_tail].slot_idx;

    uint8_t* rpc_recv_slot = recv_slot_base + ((*slot_idx) * MAX_RPC_BYTES);
      // marshal rpc structure
      rpcArg_t args;
      args.sending_nid = *sending_nid;
      args.head = &(cq->q[cq_tail]);
      args.pointerToAppData = NULL;
      args.is_nack = cq->q[cq_tail].is_nack ;
      theRPC((rpc_recv_slot ), &args);

      cq->tail = cq->tail + 1;
      //check if CQ reached its end
      if (cq->tail >= MAX_NUM_WQ) {
          cq->tail = 0;
          cq->SR ^= 1;
      }
  } else *sending_nid = -1;
}

#endif /* H_SONUMA */
