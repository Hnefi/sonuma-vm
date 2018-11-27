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
 * Msutherl:
 *  RPC service that polls CQs, reads buffers, and sends responses.
 */

#include <vector>
#include <algorithm>

#include "sonuma.h"

#define ITERS 4
#define OBJ_READ_SIZE 64
#define CTX_0 0
#define CPU_FREQ 2.4

static uint64_t op_cnt;

static __inline__ unsigned long long rdtsc(void)
{
  unsigned long hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)lo) | (((unsigned long long)hi)<<32) ;
}

//void handler(uint16_t tid, char* recv_slot, cq_entry_t *head, void *owner) {
void handler(char* rawRecvPointer, rpcArg_t* argPointer) {
  printf("[rpc_handler]: Application got RPC from nid [%d] w. buf. string: %s\n",
          argPointer->sending_nid,
          rawRecvPointer);
  op_cnt--;
}

int main(int argc, char **argv)
{
  rmc_wq_t *wq;
  rmc_cq_t *cq;

  int num_iter = (int)ITERS;
  op_cnt = num_iter;

  if (argc != 4) {
    fprintf(stdout,"Usage: ./rpc_service <this_nid> <total_nodes> <local_qp_id>\n"); 
    return 1;
  }
    
  int this_nid = atoi(argv[1]);
  int node_cnt = atoi(argv[2]);
  int qp_id = atoi(argv[3]);
  uint64_t ctx_size = PAGE_SIZE * PAGE_SIZE;
  uint64_t buf_size = PAGE_SIZE;

  // FIXME: max node count = 16
  uint8_t* recv_slots[16];
  uint8_t* sslots[16];
  uint8_t* slot_metadata[16];

  uint8_t *ctx = NULL;
  uint8_t *lbuff = NULL;
  uint64_t lbuff_slot;
  uint64_t ctx_offset;
  
  int fd = kal_open((char*)RMC_DEV);  
  if(fd < 0) {
    printf("cannot open RMC dev. driver\n");
    return -1;
  }

  char fmt[25];
  sprintf(fmt,"local_buf_ref_%d.txt",0);
  //register local buffer
  if(kal_reg_lbuff(fd, &lbuff, fmt,buf_size/PAGE_SIZE) < 0) {
    printf("Failed to allocate local buffer\n");
    return -1;
  } else {
    fprintf(stdout, "Local buffer was mapped to address %p, number of pages is %ld\n",
	    lbuff, buf_size/PAGE_SIZE);
  }

  // register send/recv state created by RMC
  size_t recv_buffer_size = (MAX_RPC_BYTES) * MSGS_PER_PAIR ;
  size_t n_rbuf_pages = (recv_buffer_size / PAGE_SIZE) + 1;

  size_t send_slots_size = MSGS_PER_PAIR * sizeof(send_slot_t);
  size_t n_sslots_pages = (send_slots_size / PAGE_SIZE) + 1;

  size_t avail_slots_size = MSGS_PER_PAIR * sizeof(send_metadata_t);
  size_t n_avail_slots_pages = (avail_slots_size / PAGE_SIZE) + 1;
  for(int i = 0; i < node_cnt; i++) {
      char fmt[25];
      sprintf(fmt,"rqueue_node_%d.txt",i);
      recv_slots[i] = NULL;
      if(kal_reg_lbuff(fd,&(recv_slots[i]),fmt,n_rbuf_pages) < 0) {
          printf("Failed to allocate receive slots for node %d\n",i);
          return -1;
      }      
      sprintf(fmt,"send_slots_%d.txt",i);
      sslots[i] = NULL;
      if(kal_reg_lbuff(fd,&(sslots[i]),fmt,n_sslots_pages) < 0) {
          printf("Failed to allocate send slots for node %d\n",i);
          return -1;
      }      
      sprintf(fmt,"avail_slots_%d.txt",i);
      slot_metadata[i] = NULL;
      if(kal_reg_lbuff(fd,&(slot_metadata[i]),fmt,n_avail_slots_pages) < 0) {
          printf("Failed to allocate slot metadata for node %d\n",i);
          return -1;
      }      
  }

  //register context
  if(kal_reg_ctx(fd, &ctx, ctx_size/PAGE_SIZE) < 0) {
    printf("Failed to allocate context\n");
    return -1;
  } else {
    fprintf(stdout, "Ctx buffer was registered, ctx_size=%ld, %ld pages.\n",
	    ctx_size, ctx_size*sizeof(uint8_t) / PAGE_SIZE);
  }

  //register WQ
  if(kal_reg_wq(fd, &wq,qp_id) < 0) {
    printf("Failed to register WQ\n");
    return -1;
  } else {
    fprintf(stdout, "WQ was mapped to address %p\n", wq);
  }

  //register CQ
  if(kal_reg_cq(fd, &cq,qp_id) < 0) {
    printf("Failed to register CQ\n");
  } else {
    fprintf(stdout, "CQ was mapped to address %p\n", cq);
  }
  
  fprintf(stdout,"Init done! Will receive %d CQ RPCs!  (this_nid = %d)\n",num_iter, this_nid);

  unsigned long long start, end;
  
  lbuff_slot = 0;
  uint16_t sending_qp = 0, sending_nid = 0;
  uint16_t slot;
  while( op_cnt > 0 ) {
      printf("Loop op_count = %d\n",op_cnt);
      rmc_poll_cq_rpc(cq, (char**)&recv_slots,&handler,&sending_nid,&sending_qp,&slot,NULL); // handler decrements --op_cnt
      printf("Returned from poll_cq_rpc...\n");

      // free slot on send-side
      rmc_recv(wq,sending_nid,sending_qp,slot);

      // copy into local buffer to send back.
      memcpy(lbuff, recv_slots[sending_nid] + (MAX_RPC_BYTES)*slot, OBJ_READ_SIZE);
      // transform "rpc" to "ret"
      char transform[4] = "ret";
      unsigned offset = 6, idx = 0;
      while(idx++ < 3) {
          *(lbuff+offset+idx) = transform[idx];
      }

      // now send lbuff the other way
      int available_slot_index = -1;
      int wait_count = 0;
      send_metadata_t* ptr = nullptr;
      while( available_slot_index < 0 ) {
          ptr = (send_metadata_t*) (slot_metadata[sending_nid]);
          available_slot_index = get_send_slot(ptr,MSGS_PER_PAIR);
          if( available_slot_index < 0 ) {
              printf("All slots full, wait #%d....\n",wait_count);
              wait_count++;
              if(wait_count > 100) { printf("ROMES, something's horribly wrong....\n"); exit(1); }
          }
      }
      start = rdtsc();
      rmc_send(wq, (char*)lbuff, lbuff_slot, OBJ_READ_SIZE,sending_nid,qp_id,ptr,available_slot_index);
  }
 
  return 0;
}
