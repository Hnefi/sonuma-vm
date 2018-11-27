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
 *  RPC test using rmc_send 
 */

#include <vector>
#include <algorithm>
#include <thread>

#include "sonuma.h"

#define ITERS 4
#define OBJ_READ_SIZE 20
#define CTX_0 0
#define CPU_FREQ 2.4

#define THREADS 4

static __inline__ unsigned long long rdtsc(void)
{
    unsigned long hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)lo) | (((unsigned long long)hi)<<32) ;
}

//void handler(uint16_t tid, char* recv_slot, cq_entry_t *head, void *owner) {
void handler(char* rawRecvPointer, rpcArg_t* argPointer) {
  printf("[pong]: Thread %d got pong from nid [%d] w. buf. string: %s\n",
          std::this_thread::get_id(),
          argPointer->sending_nid,
          rawRecvPointer);
}

void messager_thread(int thread_num, int target_nid, int snid, int node_cnt, int rpc_size, int num_iter,
        rmc_wq_t* wq, rmc_cq_t* cq, uint8_t* lbuff, uint8_t** recv_slots,
        uint8_t** sslots, uint8_t** slot_metadata) 
{
    unsigned long long start, end;

    uint64_t lbuff_slot;
    const char tmp[11] = "marks rpc\0";

    for(size_t i = 0; i < num_iter; i++) {
        lbuff_slot = (i * (OBJ_READ_SIZE)) % (PAGE_SIZE - (OBJ_READ_SIZE));

        // write a test string into lbuff
        for(int o = 0; o < 12; o++) {
            *(lbuff + (lbuff_slot+o)) = tmp[o];
        }
        printf("[tid %d] Wrote string %s to lbuff.\n",thread_num,(char*)lbuff);

        int available_slot_index = -1;
        int wait_count = 0;
        send_metadata_t* ptr = nullptr;
        while( available_slot_index < 0 ) {
            ptr = (send_metadata_t*) (slot_metadata[target_nid]);
            available_slot_index = get_send_slot(ptr,MSGS_PER_PAIR);
            if( available_slot_index < 0 ) {
                printf("All slots full, wait #%d....\n",wait_count);
                wait_count++;
                if(wait_count > 100) { 
                    printf("Thread %d, is waiting a horribly long time....\n",thread_num ); 
                    if( wait_count > 10000 ) {
                        printf("Thread %d, is probably deadlocked.....\n",thread_num); 
                        exit(-1);
                    }
                }
            }
        }

        rmc_send(wq, (char*)lbuff, lbuff_slot, OBJ_READ_SIZE,target_nid,thread_num,ptr,available_slot_index);

        uint16_t sending_qp = 0, sending_nid = 0;
        uint16_t slot;
        rmc_poll_cq_rpc(cq, (char**)&recv_slots,&handler,&sending_nid,&sending_qp,&slot,NULL); // handler decrements --op_cnt

        // free slot on send-side
        rmc_recv(wq,sending_nid,sending_qp,slot);
    } // end num iters
}

int main(int argc, char **argv)
{
    rmc_wq_t* wqs[THREADS];
    rmc_cq_t* cqs[THREADS];

    int num_iter = (int)ITERS;
    int num_iter_per_thread = num_iter / THREADS;

    if (argc != 5) {
        fprintf(stdout,"Usage: ./rpc <target_nid> <this_nid> <node_cnt> <rpc_size> \n"); 
        return 1;
    }

    int target_nid = atoi(argv[1]);
    int snid = atoi(argv[2]);
    int node_cnt = atoi(argv[3]);
    int rpc_size = atoi(argv[4]);
    uint64_t ctx_size = PAGE_SIZE * PAGE_SIZE;
    uint64_t buf_size = PAGE_SIZE;

    uint8_t *ctx = NULL;
    uint8_t* lbuff[THREADS];
    uint64_t lbuff_slot;
    uint64_t ctx_offset;

    // FIXME: max node count = 16
    uint8_t* recv_slots[16];
    uint8_t* sslots[16];
    uint8_t* slot_metadata[16];

    std::thread thrs[THREADS];

    int fd = kal_open((char*)RMC_DEV);  
    if(fd < 0) {
        printf("cannot open RMC dev. driver\n");
        return -1;
    }

    // register send/recv state created by RMC
    size_t recv_buffer_size = (MAX_RPC_BYTES ) * MSGS_PER_PAIR ;
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

        // Hard reset send slots
        send_metadata_t* ptr = (send_metadata_t*) (slot_metadata[i]);
        for(int slot_num = 0; slot_num < MSGS_PER_PAIR; slot_num++ ) {
            ptr[slot_num].valid = 1;
        }
    }
    // assumes each thread gets 1 WQ/CQ/Lbuf and the RMC already created them.
    for(int i = 0; i < THREADS; i++ ) {
        char fmt[25];
        sprintf(fmt,"local_buf_ref_%d.txt",i);
        //register local buffer
        lbuff[i] = NULL;
        if(kal_reg_lbuff(fd, &(lbuff[i]), fmt,buf_size/PAGE_SIZE) < 0) {
            printf("Failed to allocate local buffer\n");
            return -1;
        }       

        //register WQs
        if(kal_reg_wq(fd, &(wqs[i]),i) < 0) {
            printf("Failed to register WQ\n");
            return -1;
        }
        //register CQs
        if(kal_reg_cq(fd, &(cqs[i]),i) < 0) {
            printf("Failed to register CQ\n");
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
    fprintf(stdout,"Init done! Will execute %d WQ operations - SYNC! (target_node = %d, snid = %d)\n",num_iter, target_nid,snid);


    // launch all of the messagers
    for(int i = 0; i < THREADS; i++) {
        thrs[i] = std::thread(messager_thread,/*args*/
                              i,target_nid,snid,node_cnt,rpc_size,num_iter_per_thread,
                              wqs[i],cqs[i],lbuff[i],recv_slots,sslots,slot_metadata);
    }
    for(int i = 0; i < THREADS; i++) {
        thrs[i].join();
    }

    return 0;
}
