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
 *  SoftRMC-specific extensions for libsonuma
 */

#include <malloc.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

#include "sonuma.h"

int kal_open(char *kal_name)
{  
  //nothing to do
  return 0;
}

int kal_reg_wq(int fd, rmc_wq_t **wq_ptr, int wq_id)
{
  int shmid;
  
  DLog("[kal_reg_wq] kal_reg_wq called.");
  char fmt[15];
  sprintf(fmt,"wq_ref_%d.txt",wq_id);
  FILE *f = fopen(fmt, "r");
  fscanf(f, "%d", &shmid);
  printf("[kal_reg_wq] ID for the work queue is %d\n", shmid);
  *wq_ptr = (rmc_wq_t *)shmat(shmid, NULL, 0);
  if(*wq_ptr == NULL) {
    printf("[kal_reg_wq] shm attach failed (work queue)\n");
    return -1;
  }

  (*wq_ptr)->connected = true;

  fclose(f);
  
  return 0;
}

int kal_reg_cq(int fd, rmc_cq_t **cq_ptr, int cq_id)
{
  int shmid;
  DLog("[kal_reg_cq] kal_reg_cq called.");
  
  char fmt[15];
  sprintf(fmt,"cq_ref_%d.txt",cq_id);
  FILE *f = fopen(fmt, "r");
  fscanf(f, "%d", &shmid);
  printf("[kal_reg_cq] ID for the completion queue is %d\n", shmid);
  *cq_ptr = (rmc_cq_t *)shmat(shmid, NULL, 0);
  if(*cq_ptr == NULL) {
    printf("[kal_reg_cq] shm attach failed (completion queue)\n");
    return -1;
  }

  (*cq_ptr)->connected = true;
  
  fclose(f);

  return 0;
}

int kal_reg_lbuff(int fd, uint8_t **buff_ptr, const char* lb_name, uint32_t num_pages)
{
  int shmid;
  FILE *f;
  
  if(*buff_ptr == NULL) {
      f = fopen(lb_name, "r");
      fscanf(f, "%d", &shmid);
      printf("[kal_reg_lbuff] ID for the local buffer is %d\n", shmid);

    *buff_ptr = (uint8_t *)shmat(shmid, NULL, 0);
    if(*buff_ptr == NULL) {
      printf("[kal_reg_lbuff] shm attach failed (local buffer)\n");
      return -1;
    }
    
    //memset(*buff_ptr, 0, num_pages*4096); // MSUTHERL: I CAN'T BELIEVE THIS WAS THE PROBLEM THE WHOLE TIME
  } else {
    printf("[kal_ref_lbuff] local buffer has been allocated, return\n");
    return -1;
  }

  fclose(f);
  return 0;
}

int kal_reg_ctx(int fd, uint8_t **ctx_ptr, uint32_t num_pages)
{
  int shmid;
  FILE *f;

  DLog("[kal_reg_ctx] kal_reg_ctx called.");
  
  if(*ctx_ptr == NULL) {
    f = fopen("ctx_ref.txt", "r");

    fscanf(f, "%d", &shmid);
    printf("[kal_reg_ctx] ID for the context memory is %d\n", shmid);

    *ctx_ptr = (uint8_t *)shmat(shmid, NULL, 0);    
    if(*ctx_ptr == NULL) {
      printf("[sonuma] shm attach failed (context)\n");
      return -1;
    }
    
    memset(*ctx_ptr, 0, 4096);
  } else {
    DLog("[kal_reg_ctx] error: context memory allready allocated\n");
    return -1;
  }

  fclose(f);
  
  return 0;
}

/* Msutherl: New version of rmc_send, using paired send/recv slots */
void rmc_send(rmc_wq_t *wq, char *lbuff_ptr, int lbuff_offset, size_t size, int snid, uint8_t sending_qp, send_slot_t* send_slot,uint8_t slot_idx)
{
    uint8_t wq_head = wq->head;
    DLog("[rmc_send] Entering rmc_send.");

    // setup send slot for RMC
    send_slot->valid = 1;
    send_slot->msg_size = size;
    send_slot->sending_qp = sending_qp;

    while (wq->q[wq_head].valid) {} //wait for WQ head to be ready
    
    send_slot->wq_entry_idx = wq_head;
    wq->q[wq_head].buf_addr = (uint64_t)lbuff_ptr;
    wq->q[wq_head].buf_offset = lbuff_offset;
    /*
    *(lbuff_ptr + (lbuff_offset+size)) = 's';
    *(lbuff_ptr + (lbuff_offset+size+1)) = sending_qp;
    *(lbuff_ptr + (lbuff_offset+size+2)) = slot_idx;
    */
#ifdef PRINT_BUFS
    print_cbuf( (char*)lbuff_ptr , size );
#endif
    if(size < 64) wq->q[wq_head].length = 64; //at least 64B
    else wq->q[wq_head].length = size;
    wq->q[wq_head].op = 's';
    wq->q[wq_head].nid = snid;
    wq->q[wq_head].valid = 1;
    wq->q[wq_head].SR = wq->SR;
    // Msutherl:
    wq->q[wq_head].slot_idx = slot_idx;

    wq->head =  wq->head + 1;
    //check if WQ reached its end
    if (wq->head >= MAX_NUM_WQ) {
        wq->head = 0;
        wq->SR ^= 1;
    }
}

void rmc_recv(rmc_wq_t *wq, char* lbuff_ptr,int lbuff_offset,size_t size,int snid,uint8_t sending_qp,uint8_t slot_idx)
{
    // create WQ entry, response for arguments given to CQ
    uint8_t wq_head = wq->head;

    DLog("[rmc_recv] rmc_recv called.");

    while (wq->q[wq_head].valid) {} //wait for WQ head to be ready
    /*
    *(lbuff_ptr + (lbuff_offset+size)) = 'g';
    *(lbuff_ptr + (lbuff_offset+size+1)) = sending_qp;
    *(lbuff_ptr + (lbuff_offset+size+2)) = slot_idx;
    */
#ifdef PRINT_BUFS
    print_cbuf( (char*)lbuff_ptr , size );
#endif
    if(size < 64) wq->q[wq_head].length = 64; //at least 64B
    else wq->q[wq_head].length = size;
    wq->q[wq_head].op = 'g';
    wq->q[wq_head].nid = snid;

    wq->q[wq_head].valid = 1;
    wq->q[wq_head].SR = wq->SR;
    wq->q[wq_head].slot_idx = slot_idx;
        // signal RMC to reuse this slot

    wq->head =  wq->head + 1;

    //check if WQ reached its end
    if (wq->head >= MAX_NUM_WQ) {
        wq->head = 0;
        wq->SR ^= 1;
    }
}
