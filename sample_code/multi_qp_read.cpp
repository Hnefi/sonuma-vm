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
 *  MultiQP Connection Test (within 1 app)
 */

#include <vector>
#include <algorithm>

#include "sonuma.h"

#define ITERS 4096
#define SLOT_SIZE 64
#define OBJ_READ_SIZE 64
#define CTX_0 0
#define CPU_FREQ 2.4

#define MAX_QPS 64

static __inline__ unsigned long long rdtsc(void)
{
  unsigned long hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)lo) | (((unsigned long long)hi)<<32) ;
}

int main(int argc, char **argv)
{
  rmc_wq_t* wqs[MAX_QPS];
  rmc_cq_t* cqs[MAX_QPS];

  int num_iter = (int)ITERS;

  if (argc != 5) {
    fprintf(stdout,"Usage: ./multi_qp_read <target_nid> <op_type> <qp_id_start> <qp_id_end>\n"); 
    return 1;
  }
    
  int snid = atoi(argv[1]);
  char op = *argv[2];
  int qp_start = atoi(argv[3]);
  int qp_end = atoi(argv[4]);
  if( (qp_end - qp_start + 1) > MAX_QPS ) {
      fprintf(stdout,"---- Rqeusted too many QPs, can't do > 64.\n");
      exit(EXIT_FAILURE);
  }
  uint64_t ctx_size = PAGE_SIZE * PAGE_SIZE;
  uint64_t buf_size = PAGE_SIZE;

  uint8_t *ctx = NULL;
  uint8_t* lbuff[MAX_QPS];
  uint64_t lbuff_slot;
  uint64_t ctx_offset;
  
  int fd = kal_open((char*)RMC_DEV);  
  if(fd < 0) {
    printf("cannot open RMC dev. driver\n");
    return -1;
  }

  //register context
  if(kal_reg_ctx(fd, &ctx, ctx_size/PAGE_SIZE) < 0) {
    printf("Failed to allocate context\n");
    return -1;
  } else {
    fprintf(stdout, "Ctx buffer was registered, ctx_size=%ld, %ld pages.\n",
	    ctx_size, ctx_size*sizeof(uint8_t) / PAGE_SIZE);
  }

  for(int i = qp_start; i <= qp_end; i++) {
      //register local buffers
      char fmt[25];
      sprintf(fmt,"local_buf_ref_%d.txt",i);
      lbuff[i] = NULL;
      if(kal_reg_lbuff(fd, &(lbuff[i]), fmt, buf_size/PAGE_SIZE) < 0) {
        printf("Failed to allocate local buffer number %i\n",i);
        return -1;
      } else {
        fprintf(stdout, "Local buffer was mapped to address %p, number of pages is %ld\n",
            lbuff[i], buf_size/PAGE_SIZE);
      }
      //register WQ
      if(kal_reg_wq(fd, &(wqs[i]),i) < 0) {
        printf("Failed to register WQ id: %d\n",i);
        return -1;
      } else {
        fprintf(stdout, "WQ %d was registered.\n",i);
      }

      //register CQ
      if(kal_reg_cq(fd, &(cqs[i]),i) < 0) {
        printf("Failed to register CQ id: %d\n",i);
      } else {
        fprintf(stdout, "CQ %d was registered.\n",i);
      }
  }

  fprintf(stdout,"Init done! Will execute %d WQ operations - SYNC! (snid = %d)\n",
	  num_iter, snid);

  unsigned long long start, end;
  
  lbuff_slot = 0;
  
  for(int qp_id = qp_start; qp_id <= qp_end; qp_id++) {
      for(size_t i = 0; i < num_iter; i++) {
          ctx_offset = (i * PAGE_SIZE) % ctx_size;
          lbuff_slot = (i * sizeof(uint32_t)) % (PAGE_SIZE - OBJ_READ_SIZE);

          start = rdtsc();

          if(op == 'r') {
              rmc_rread_sync(wqs[qp_id], cqs[qp_id], lbuff[qp_id], lbuff_slot, snid, CTX_0, ctx_offset, OBJ_READ_SIZE);
          } else if(op == 'w') {
              rmc_rwrite_sync(wqs[qp_id], cqs[qp_id], lbuff[qp_id], lbuff_slot, snid, CTX_0, ctx_offset, OBJ_READ_SIZE);
          } else
              ;

          end = rdtsc();

          if(op == 'r') {
              printf("read this number: %u\n", ((uint32_t*)lbuff)[lbuff_slot/sizeof(uint32_t)]);
          }
          printf("time to execute this op: %lf ns\n", ((double)end - start)/CPU_FREQ);
      } // end iters
  }// end qps

  return 0;
}
