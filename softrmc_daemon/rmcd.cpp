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
 *  All-software implementation of the RMC
 */

#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/shm.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <vector>
#include <bitset>
#include <algorithm>
#include <iostream>

#include "rmcd.h"
#include "son_msg.h"
#include "sonuma.h"

#define REQ_THRESHOLD 2

using namespace std;

//server information
static server_info_t *sinfo;

static volatile bool rmc_active;
static int fd;

//partitioned global address space - one entry per region
static char *ctx[MAX_NODE_CNT];

static int node_cnt, this_nid;

// rpc messaging domain data
static char* recv_slots;

// RPC srq structure, has space for MAX_NUM_SRQ_SLOTS rpcs
rpc_srq_t rpc_srq;
static int* rpcs_per_core;
unsigned rpc_cores;

// message id for all send() messages.
static uint16_t rpc_id = 1;

// Implementation to return str rep. of WQ entry
// FIXME: assumes buffer has enough space (please buffer-overflow attack this!)
int stringify_wq_entry(wq_entry_t* entry,char* buf)
{
    return sprintf(buf,
            "{ Operation = %c,"
            " SR = %u,"
            " Valid = %u,"
            " LBuf_Addr = %#lx,"
            " LBuf_Offset = %#lx,"
            " Node ID = %d,"
            " Senders QP = %u,"
            " Slot Index = %u,"
            " CTlx Offset = %#lx,"
            " Read Length = %lu }\n"
            , entry->op, entry->SR, entry->valid, entry->buf_addr, entry->buf_offset,
            entry->nid, entry->cid, entry->slot_idx, entry->offset, entry->length);
}

static int
get_avail_core(int* rpc_occupancy_array) {
    for(unsigned i = 0; i < rpc_cores; i++) {
        if( rpc_occupancy_array[i] <= REQ_THRESHOLD ) {
            return i;
        }
    }
    return -1;
}

static void
decrement_core_occupancy(int* rpc_occupancy_array, unsigned core_id) {
    rpc_occupancy_array[core_id]--;
#ifdef DEBUG_RMC
    assert( rpc_occupancy_array[core_id] >= 0 );
#endif
}

static void
increment_core_occupancy(int* rpc_occupancy_array, unsigned core_id) {
    rpc_occupancy_array[core_id]++;
#ifdef DEBUG_RMC
    assert( rpc_occupancy_array[core_id] <= REQ_THRESHOLD );
#endif
}

static
bool srq_empty(rpc_srq_t* srq)
{
    assert(srq);
    return(!srq->full && (srq->head == srq->tail));
}

static
bool srq_full(rpc_srq_t* srq) 
{ 
    assert(srq);
    return srq->full;
}

static
void advance_srq_head(rpc_srq_t* srq)
{
    assert(srq);
    srq->head = (srq->head + 1) % MAX_NUM_SRQ_SLOTS;
    srq->full = (srq->head == srq->tail);
}

static
void advance_srq_tail(rpc_srq_t* srq)
{
    assert(srq);
    srq->tail = (srq->tail + 1) % MAX_NUM_SRQ_SLOTS;
    srq->full = false;
}

static void
advance_dispatch_slot(rpc_srq_t* srq)
{
    assert(srq);
    srq->dispatch_slot = (srq->dispatch_slot + 1) % MAX_NUM_SRQ_SLOTS;
}

static
void advance_srq_tail_multiple(rpc_srq_t* srq)
{
    assert(srq);
    while( srq->q[srq->tail].valid == false && srq->tail != srq->head ) {
        srq->tail = (srq->tail + 1) % MAX_NUM_SRQ_SLOTS;
    }
    srq->full = false;
}

static
int enqueue_in_srq(rpc_srq_t* srq, rpc_srq_entry_t* newEntry)
{
    assert(srq);
    if( !srq_full(srq) ) {
        newEntry->slot_idx = srq->head;
        srq->q[srq->head] = *newEntry;
        srq->q[srq->head].valid = true;
        int old_head = srq->head;
        advance_srq_head(srq);
        return old_head;
    }
    return -1 ;
}

static
bool dequeue_tail_from_srq(rpc_srq_t* srq,rpc_srq_entry_t* returned_entry )
{
    assert(srq);
    if(!srq_empty(srq)) {
        assert(srq->q[srq->tail].valid == true);
        *returned_entry = srq->q[srq->tail];
        srq->q[srq->tail].valid = false;
        advance_srq_tail(srq);
        return true;
    } 
    return false;
}

static
void get_srq_tail_entry(rpc_srq_t* srq, rpc_srq_entry_t* tail_entry)
{
    assert(srq);
    if(!srq_empty(srq)) {
        assert(srq->q[srq->tail].valid == true);
        *tail_entry = srq->q[srq->tail];
    }
}

static
void get_dispatch_entry_from_srq(rpc_srq_t* srq, rpc_srq_entry_t* tail_entry)
{
    assert(srq);
    if(!srq_empty(srq)) {
        assert(srq->q[srq->dispatch_slot].valid == true);
        *tail_entry = srq->q[srq->dispatch_slot];
        advance_dispatch_slot(srq);
    }
}

static
bool reset_srq_entry_invalid(rpc_srq_t* srq, uint16_t idx )
{
    assert(srq);
    if(!srq_empty(srq)) {
        assert(srq->q[idx].valid == true);
        srq->q[idx].valid = false;
        advance_srq_tail_multiple(srq);
        return true;
    } 
    return false;
}

static
void srq_init(rpc_srq_t* srq)
{
    for(size_t i = 0; i < MAX_NUM_SRQ_SLOTS;i++) {
        srq->q[i].valid = false;
    }
    srq->tail = 0;
    srq->head = 0;
    srq->dispatch_slot = 0;
    srq->full = false;
}

// Taken from Beej's guide to Network Programming:
// http://beej.us/guide/bgnet/html/multi/advanced.html
int sendall(int sock_fd, char* buf, unsigned* bytesToSend)
{
    unsigned total = 0;
    int bytesLeft = *bytesToSend;
    int n;

    while( total < *bytesToSend ) {
        n = send(sock_fd, buf+total, bytesLeft, 0);
        if( n == -1 ) break;
        total += n;
        bytesLeft -= n;
    }
    *bytesToSend = total; // actual number of bytes sent goes here
    return n==-1?-1:0;
}

int alloc_wq(rmc_wq_t **qp_wq, int wq_id)
{
  int retcode, i;
  FILE *f;
  
  int shmid = shmget(IPC_PRIVATE, PAGE_SIZE,
			     SHM_R | SHM_W);
  if(-1 != shmid) {
    printf("[alloc_wq] shmget for WQ okay, shmid = %d\n",
	   shmid);
    *qp_wq = (rmc_wq_t *)shmat(shmid, NULL, 0);

    printf("[alloc_wq] shmat completed\n");

    char fmt[15];
    sprintf(fmt,"wq_ref_%d.txt",wq_id);
    f = fopen(fmt, "w");
    fprintf(f, "%d", shmid);
    fclose(f);
  } else {
    printf("[alloc_wq] shmget failed\n");
  }

  rmc_wq_t *wq = *qp_wq; 
  
  if (wq == NULL) {
    printf("[alloc_wq] Work Queue could not be allocated.");
    return -1;
  }
  
  //initialize wq memory
  printf("size of rmc_wq_t: %lu\n", sizeof(rmc_wq_t));
  memset(wq, 0, sizeof(rmc_wq_t));

  printf("[alloc_wq] memset the WQ memory\n");
  
  retcode = mlock((void *)wq, PAGE_SIZE);
  if(retcode != 0) {
    DLog("[alloc_wq] WQueue mlock returned %d", retcode);
    return -1;
  } else {
    DLogNoVar("[alloc_wq] WQ was pinned successfully.");
  }

  //setup work queue
  wq->head = 0;
  wq->SR = 1;
  wq->connected = false;

  for(i=0; i<MAX_NUM_WQ; i++) {
    wq->q[i].SR = 0;
  }

  return 0;
}

int alloc_cq(rmc_cq_t **qp_cq, int cq_id)
{
  int retcode, i;
  FILE *f;
  
  int shmid = shmget(IPC_PRIVATE, PAGE_SIZE,
			     SHM_R | SHM_W);
  if(-1 != shmid) {
    printf("[alloc_cq] shmget for CQ okay, shmid = %d\n", shmid);
    *qp_cq = (rmc_cq_t *)shmat(shmid, NULL, 0);

    char fmt[15];
    sprintf(fmt,"cq_ref_%d.txt",cq_id);

    f = fopen(fmt, "w");
    fprintf(f, "%d", shmid);
    fclose(f);
  } else {
    printf("[alloc_cq] shmget failed\n");
  }

  rmc_cq_t *cq = *qp_cq; 
  
  if (cq == NULL) {
    DLogNoVar("[alloc_cq] Completion Queue could not be allocated.");
    return -1;
  }
  
  //initialize cq memory
  memset(cq, 0, sizeof(rmc_cq_t));
    
  retcode = mlock((void *)cq, PAGE_SIZE);
  if(retcode != 0) {
    DLog("[alloc_cq] CQueue mlock returned %d", retcode);
    return -1;
  } else {
    DLogNoVar("[alloc_cq] CQ was pinned successfully.");
  }

  //setup completion queue
  cq->tail = 0;
  cq->SR = 1;
  cq->connected = false;

  for(i=0; i<MAX_NUM_WQ; i++) {
    cq->q[i].SR = 0;
  }

  return 0;
}

int local_buf_alloc(char **mem,const char* fname,size_t npages)
{
  int retcode;
  FILE *f;
  
  int shmid = shmget(IPC_PRIVATE, npages * PAGE_SIZE,
			     SHM_R | SHM_W);
  if(-1 != shmid) {
    DLog("[local_buf_alloc] shmget for local buffer okay, shmid = %d\n",
	   shmid);
    *mem = (char *)shmat(shmid, NULL, 0);
    f = fopen(fname, "w");
    fprintf(f, "%d", shmid);
    fclose(f);
  } else {
    DLog("[local_buf_alloc] shmget failed for lbuf_name = %s\n",fname);
  }

  if (*mem == NULL) {
    DLogNoVar("[local_buf_alloc] Local buffer could have not be allocated.");
    return -1;
  }
  
  memset(*mem, 0, npages*PAGE_SIZE );
    
  retcode = mlock((void *)*mem, npages*PAGE_SIZE);
  if(retcode != 0) {
    DLog("[local_buf_alloc] mlock returned %d", retcode);
    return -1;
  } else {
    DLogNoVar("[local_buf_alloc] was pinned successfully.");
  }

  DLog("[local_buf_alloc] Successfully created app-mapped lbuf with name = %s\n",
          fname);

  return 0;
}

static int rmc_open(char *shm_name)
{   
  int fd;
  
  printf("[rmc_open] open called in VM mode\n");
  
  if ((fd=open(shm_name, O_RDWR|O_SYNC)) < 0) {
  //if ((fd=open(shm_name, O_RDWR|O_SYNC|O_CREAT, S_IRUSR | S_IWUSR | S_IROTH | S_IWOTH )) < 0) { // MARK: test w. creating file?
    return -1;
  }
  
  return fd;
}

static int soft_rmc_ctx_destroy()
{
  int i;
  
  ioctl_info_t info;
  
  info.op = RUNMAP;
  for(i=0; i<node_cnt; i++) {
    if(i != this_nid) {
      info.node_id = i;
      if(ioctl(fd, 0, (void *)&info) == -1) {
	printf("[soft_rmc_ctx_destroy] failed to unmap a remote region\n");
	return -1;
      }
    }
  }
  
  return 0;
}

static int net_init(int node_cnt, unsigned this_nid, char *filename)
{
  FILE *fp;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  int i = 0;
  char *pch;
  
  printf("[network] net_init <- \n");
  
  sinfo = (server_info_t *)malloc(node_cnt * sizeof(server_info_t));
  
  //retreive ID, IP, DOMID
  fp = fopen(filename, "r");
  if (fp == NULL) {
    printf("[network] Net_init failed to open %s., die\n",filename);
    exit(EXIT_FAILURE);
  }

  //get server information
  while ((read = getline(&line, &len, fp)) != -1) {
    pch = strtok (line,":");
    sinfo[i].nid = atoi(pch);
    printf("ID: %d ", sinfo[i].nid);
    pch = strtok(NULL, ":");
    strcpy(sinfo[i].ip, pch);
    printf("IP: %s ", sinfo[i].ip);
    pch = strtok(NULL, ":");
    sinfo[i].domid = atoi(pch);
    printf("DOMID: %d\n", sinfo[i].domid);
    i++;
  }

  printf("[network] net_init -> \n");
    
  return 0;
}

//allocates local memory and maps remote memory 
int ctx_map(char **mem, unsigned page_cnt)
{
  ioctl_info_t info; 
  int i;
    
  printf("[ctx_map] soft_rmc_alloc_ctx ->\n");
  //unsigned long dom_region_size = page_cnt * PAGE_SIZE;
    
  ctx[this_nid] = *mem;
  
  printf("[ctx_map] registering remote memory, number of remote nodes %d\n", node_cnt-1);
  
  info.op = RMAP;

  //map the rest of pgas
  for(i=0; i<node_cnt; i++) {
      if(i != this_nid) {
          info.node_id = i;
          if(ioctl(fd, 0, (void *)&info) == -1) {
              printf("[ctx_map] ioctl failed\n");
              return -1;
          }

          printf("[ctx_map] mapping memory of node %d\n", i);

          ctx[i] = (char *)mmap(NULL, page_cnt * PAGE_SIZE,
                  PROT_READ | PROT_WRITE,
                  MAP_SHARED, fd, 0);
          if(ctx[i] == MAP_FAILED) {
              close(fd);
              perror("[ctx_map] error mmapping the file");
              exit(EXIT_FAILURE);
          }

#if 0
          //for testing purposes
          for(int j=0; j<(dom_region_size)/sizeof(unsigned long); j++)
              printf("%lu\n", *((unsigned long *)ctx[i]+j));
#endif
      }
  } // end map of pgas
  
  printf("[ctx_map] context successfully created, %lu bytes\n",
	 (unsigned long)page_cnt * PAGE_SIZE * node_cnt);
  
  //activate the RMC
  rmc_active = true;
  
  return 0;
}

int ctx_alloc_grant_map(char **mem, unsigned page_cnt)
{
  unsigned int srv_idx;
  int listen_fd;
  struct sockaddr_in servaddr; //listen
  struct sockaddr_in raddr; //connect, accept
  int optval = 1;
  unsigned n;
  FILE *f;
  
  printf("[ctx_alloc_grant_map] soft_rmc_connect <- \n");

  //allocate the pointer array for PGAS
  fd = rmc_open((char *)"/dev/sonuma_rmc");
  if( fd < 0 ) {
      printf("[ctx_alloc_grant_map]\t rmc_open failed with errno == %d... Killing.\n",errno);
      return fd;
  }

  //first allocate memory
  unsigned long *ctxl;
  unsigned long dom_region_size = page_cnt * PAGE_SIZE;

  int shmid = shmget(IPC_PRIVATE, dom_region_size*sizeof(char), SHM_R | SHM_W);
  if(-1 != shmid) {
    printf("[ctx_alloc_grant_map] shmget okay, shmid = %d\n", shmid);
    *mem = (char *)shmat(shmid, NULL, 0);

    f = fopen("ctx_ref.txt", "w");
    fprintf(f, "%d", shmid);
    fclose(f);
  } else {
    printf("[ctx_alloc_grant_map] shmget failed\n");
  }

  if(*mem != NULL) {
    printf("[ctx_alloc_grant_map] memory for the context allocated\n");
    memset(*mem, 0, dom_region_size);
    mlock(*mem, dom_region_size);
  }
  
  printf("[ctx_alloc_grant_map] managed to lock pages in memory\n");
  
  ctxl = (unsigned long *)*mem;

  //snovakov:need to do this to fault the pages into memory
  for(unsigned int i=0; i<(dom_region_size*sizeof(char))/8; i++) {
    ctxl[i] = 0;
  }

  //register this memory with the kernel driver
  ioctl_info_t info;
  info.op = MR_ALLOC;
  info.ctx = (unsigned long)*mem;
  
  if(ioctl(fd, 0, &info) == -1) {
    perror("kal ioctl failed");
    return -1;
  }
  
  //initialize the network
  net_init(node_cnt, this_nid, (char *)"/root/servers.txt");
  
  //listen
  listen_fd = socket(AF_INET, SOCK_STREAM, 0);

  bzero(&servaddr,sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  servaddr.sin_port=htons(PORT);
    
  if((setsockopt(listen_fd, SOL_SOCKET,SO_REUSEPORT,&optval, sizeof(optval))) == -1) {
    perror("Error on setsockopt.\n");
    exit(EXIT_FAILURE);
  }

  if(bind(listen_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
    perror("Address binding error in ctx_create_global_space.\n");
    exit(EXIT_FAILURE);
  }

  if(listen(listen_fd, 1024) == -1) {
    printf("[ctx_alloc_grant_map] Listen call error\n");
    exit(EXIT_FAILURE);
  }

  for(int i=0; i<node_cnt; i++) {
    if(i != this_nid) {
      if(i > this_nid) {
          printf("[ctx_alloc_grant_map] server accept..\n");
          char *remote_ip;

          socklen_t slen = sizeof(raddr);

          sinfo[i].fd = accept(listen_fd, (struct sockaddr*)&raddr, &slen);

          //retrieve nid of the remote node
          remote_ip = inet_ntoa(raddr.sin_addr);

          printf("[ctx_alloc_grant_map] Connect received from %s, on port %d\n",
                  remote_ip, raddr.sin_port);

          //receive the reference to the remote memory
          while(1) {
              n = recv(sinfo[i].fd, (char *)&srv_idx, sizeof(int), 0);
              if(n == sizeof(int)) {
                  printf("[ctx_alloc_grant_map] received the node_id\n");
                  break;
              }
          }

          printf("[ctx_alloc_grant_map] server ID is %u\n", srv_idx);
      } else {
          printf("[ctx_alloc_grant_map] server connect..\n");

          memset(&raddr, 0, sizeof(raddr));
          raddr.sin_family = AF_INET;
          inet_aton(sinfo[i].ip, &raddr.sin_addr);
          raddr.sin_port = htons(PORT);

          sinfo[i].fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

          while(1) {
              if(connect(sinfo[i].fd, (struct sockaddr *)&raddr, sizeof(raddr)) == 0) {
                  printf("[ctx_alloc_grant_map] Connected to %s\n", sinfo[i].ip);
                  break;
              }
          }
          unsigned n = send(sinfo[i].fd, (char *)&this_nid, sizeof(int), 0); //MSG_DONTWAIT
          if(n < sizeof(int)) {
              printf("[ctx_alloc_grant_map] ERROR: couldn't send the node_id\n");
              return -1;
          }

          srv_idx = i;
      }

      //first get the reference for this domain
      info.op = GETREF;
      info.node_id = srv_idx;
      info.domid = sinfo[srv_idx].domid;
      if(ioctl(fd, 0, (void *)&info) == -1) {
          printf("[ctx_alloc_grant_map] failed to unmap a remote region\n");
          return -1;
      }

      //send the reference to the local memory
      unsigned n = send(sinfo[srv_idx].fd, (char *)&info.desc_gref, sizeof(int), 0); //MSG_DONTWAIT
      if(n < sizeof(int)) {
          printf("[ctx_alloc_grant_map] ERROR: couldn't send the grant reference\n");
          return -1;
      }

      printf("[ctx_alloc_grant_map] grant reference sent: %u\n", info.desc_gref);

      //receive the reference to the remote memory
      while(1) {
          n = recv(sinfo[srv_idx].fd, (char *)&info.desc_gref, sizeof(int), 0);
          if(n == sizeof(int)) {
              printf("[ctx_alloc_grant_map] received the grant reference\n");
              break;
          }
      }

      printf("[ctx_alloc_grant_map] grant reference received: %u\n", info.desc_gref);
    
      //put the ref for this domain
      info.op = PUTREF;
      if(ioctl(fd, 0, (void *)&info) == -1) {
	printf("[ctx_alloc_grant_map] failed to unmap a remote region\n");
	return -1;
      }
    }
  } //for loop over all node_cnt

  //now memory map all the regions
  ctx_map(mem, page_cnt);
  
  return 0;
}

int main(int argc, char **argv)
{
  int i;
  
  // change this to allocate many QPs
  volatile rmc_wq_t** wqs = NULL;
  volatile rmc_cq_t** cqs = NULL;

  //local context region
  char *local_mem_region;

  //local buffer
  char **local_buffers;

  // temporary copy buffers
  char* tmp_copies;

  if(argc != 4) {
    printf("[main] incorrect number of arguments\n"
            "Usage: ./rmcd <Num. soNUMA Nodes> <ID of this node> <Num QPs to expose>\n");
    exit(1);
  }

  node_cnt = atoi(argv[1]);
  this_nid = atoi(argv[2]);
  unsigned num_qps = atoi(argv[3]);
  
  wqs = (volatile rmc_wq_t**) calloc(num_qps,sizeof(rmc_wq_t*));
  cqs = (volatile rmc_cq_t**) calloc(num_qps,sizeof(rmc_cq_t*));
  local_buffers = (char**) calloc(num_qps,sizeof(char*));

  //allocate a queue pair, arg bufs
  for(unsigned int i = 0; i < num_qps; i++) {
      alloc_wq((rmc_wq_t **)&wqs[i],i);
      alloc_cq((rmc_cq_t **)&cqs[i],i);
      //allocate local buffers
      char fmt[20];
      sprintf(fmt,"local_buf_ref_%d.txt",i);
      local_buf_alloc(&local_buffers[i],fmt,1);
  }

  srq_init(&rpc_srq);

  size_t recv_buffer_size = MAX_NUM_SRQ_SLOTS * MAX_RPC_BYTES;
  size_t n_rbuf_pages = (recv_buffer_size / PAGE_SIZE) + 1;

  /* Refactor for NACK implmentation. 1 SRQ, 1 set of receive slots. */
  recv_slots = nullptr;
  char fmt[20];
  sprintf(fmt,"recv_slots_node%d.txt",this_nid);
  local_buf_alloc(&recv_slots,fmt,n_rbuf_pages);

  // make a tmp buffer to hold RPC arguments
  tmp_copies = (char*)calloc(MAX_RPC_BYTES + RMC_Message::calcTotalHeaderBytes(),sizeof(char));

  // assume num qps == num cores, to create the rpcs_per_core
  rpcs_per_core = (int*) malloc(num_qps*sizeof(int));
  rpc_cores = num_qps;
  for(size_t i = 0; i < num_qps; i++) {
      rpcs_per_core[i] = 0;
  }

 //create the global address space
  if(ctx_alloc_grant_map(&local_mem_region, MAX_REGION_PAGES) == -1) {
    printf("[main] context not allocated\n");
    return -1;
  }
  
  // WQ and CQ ptrs (per pair)
  uint8_t* local_WQ_tails= (uint8_t*)calloc(num_qps,sizeof(uint8_t));
  uint8_t* local_WQ_SRs = (uint8_t*)calloc(num_qps,sizeof(uint8_t));

  //CQ ptrs
  uint8_t* local_CQ_heads = (uint8_t*)calloc(num_qps,sizeof(uint8_t));
  uint8_t* local_CQ_SRs = (uint8_t*)calloc(num_qps,sizeof(uint8_t));
  
  uint8_t* compl_idx = (uint8_t*)calloc(num_qps,sizeof(uint8_t));
  
  volatile wq_entry_t* curr;
  for(unsigned int ii = 0; ii < num_qps; ii++) { // init per-qp structures
      local_WQ_tails[ii] = 0;
      local_WQ_SRs[ii] = 1;
      local_CQ_heads[ii] = 0;
      local_CQ_SRs[ii] = 1;
  }
  
#ifdef DEBUG_PERF_RMC
  struct timespec start_time, end_time;
  uint64_t start_time_ns, end_time_ns;
  vector<uint64_t> stimes;
#endif

  // local pointers for wq/cq polling
  volatile rmc_wq_t* wq;
  volatile rmc_cq_t* cq;
  char* local_buffer;
  
  while(rmc_active) {
      /* New architecture:
       * - Do one round of QP polling and process all entries.
       * - Then use poll() to look at all sockets for incoming rmc-rmc transfers
       */
      for(unsigned int qp_num = 0; qp_num < num_qps; qp_num++) { // QP polling round
          wq = wqs[qp_num];
          cq = cqs[qp_num];
          local_buffer = local_buffers[qp_num];
          uint8_t* local_wq_tail = &(local_WQ_tails[qp_num]);
          uint8_t* local_cq_head = &(local_CQ_heads[qp_num]);
          uint8_t* local_wq_SR = &(local_WQ_SRs[qp_num]);
          uint8_t* local_cq_SR = &(local_CQ_SRs[qp_num]);
          while ( wq->connected == true && // poll only connected WQs;
                  (wq->q[*local_wq_tail].SR == *local_wq_SR) ) {
              DLog("[main] : Processing WQ entry [%d] (local_wq_tail) from QP number %d. QP->SR = %d, local_wq_SR = %d\n",*local_wq_tail,qp_num,wq->SR,*local_wq_SR);

#ifdef DEBUG_PERF_RMC
              clock_gettime(CLOCK_MONOTONIC, &start_time);
#endif
              curr = &(wq->q[*local_wq_tail]);
#ifdef DEBUG_RMC // used ifdef here to avoid stringify every single time
              // (even in perf-mode)
              char wq_entry_buf[180];
              int stringify_return = stringify_wq_entry((wq_entry_t*)curr,wq_entry_buf);
              if( stringify_return < 0 ) {
                  DLogNoVar("COULD NOT STRINGIFY CURR!!!!\n");
              } else {
                  DLog("%s",wq_entry_buf);
              }
#endif

              switch(curr->op) {
                  // Msutherl: r/w are 1 sided ops, s/g are send/receive
                  case 'r':
                      memcpy((uint8_t *)(local_buffer + curr->buf_offset),
                              ctx[curr->nid] + curr->offset,
                              curr->length);
                      break;
                  case 'w':
                      memcpy(ctx[curr->nid] + curr->offset,
                              (uint8_t *)(local_buffer + curr->buf_offset),
                              curr->length);	
                      break;
                  case 's':
                      {
                          // send rmc->rmc rpc
                          int receiver = curr->nid;
#ifdef PRINT_BUFS
                          DLog("Printing RPC Buffer IN rmcd, BEFORE serialization.\n");
                          DumpHex( (char*)(local_buffer + curr->buf_offset), curr->length);
#endif
                          // 1) Take QP metadata and create RMC_Message class
                          // 2) Serialize/pack
                          // 3) sendall() to push all of the bytes out
                          char qp_terminate_char = ( curr->send_qp_terminate == true ) ? 't' : 'f';
                          uint16_t qp_num_conditional = ( curr->send_qp_terminate == true ) ? curr->qp_num_at_receiver : qp_num ;
                          RMC_Message msg((uint16_t)rpc_id,qp_num_conditional,(uint16_t)curr->slot_idx,curr->op,(local_buffer + (curr->buf_offset)),curr->length,qp_terminate_char);
                          rpc_id++;
                          if( rpc_id == 0 ) rpc_id = 1; // 0 is a magic value used for rmc_recv()
                          uint32_t bytesToSend = msg.message_len;
                          uint32_t copy = bytesToSend;
                          char* packedBuffer = new char[bytesToSend];
                          msg.pack(packedBuffer);
#ifdef PRINT_BUFS
                          DLog("Printing RPC Buffer after pack.\n");
                          DumpHex(packedBuffer, bytesToSend);
#endif
                          int retval = sendall(sinfo[receiver].fd,packedBuffer,&bytesToSend);
                          if( retval < 0 ) {
                              perror("[rmc_rpc] send failed, w. error:");
                          } else if ( bytesToSend < copy) {
                              printf("Only sent %d of %d bytes.... Do something about it!!!!\n",bytesToSend,copy);
                          } else {}
                          delete packedBuffer;
                          break;
                      }
                  case 'g':
                      {
                          int receiver = curr->nid;
                          // 1) Take QP metadata and create RMC_Message class
                          // 2) Serialize/pack
                          // 3) sendall() to push all of the bytes out
                          RMC_Message msg((uint16_t)qp_num,(uint16_t)curr->slot_idx,curr->op);
                          uint32_t bytesToSend = msg.message_len;
                          uint32_t copy = bytesToSend;
                          char* packedBuffer = new char[bytesToSend];
                          msg.pack(packedBuffer);
                          int retval = sendall(sinfo[receiver].fd,packedBuffer,&bytesToSend);
                          if( retval < 0 ) {
                              perror("[rmc_rpc] send failed, w. error:");
                          } else if ( bytesToSend < copy) {
                              printf("Only sent %d of %d bytes.... Do something about it!!!!\n",bytesToSend,copy);
                          } else {}
                          delete packedBuffer;

                          /* Flow control completed, update the core occupancy table */
                          if( curr->dispatch_on_recv ) {
                              decrement_core_occupancy(rpcs_per_core,qp_num);
                          }
                          assert( reset_srq_entry_invalid(&rpc_srq, curr->slot_idx) );
                          break;
                      }
                  case 'a': ;
                      // TODO: model rendezvous method of transfer
                  default:
                      DLogNoVar("Un-implemented op. in WQ entry. drop it on the floor.\n");
              } // switch WQ entry types

#ifdef DEBUG_PERF_RMC
              clock_gettime(CLOCK_MONOTONIC, &end_time);
              start_time_ns = BILLION * start_time.tv_sec + start_time.tv_nsec;
              end_time_ns = BILLION * end_time.tv_sec + end_time.tv_nsec;
              printf("[main] memcpy latency: %u ns\n", end_time_ns - start_time_ns);
#endif

#ifdef DEBUG_PERF_RMC
              clock_gettime(CLOCK_MONOTONIC, &start_time);
#endif

              // notify the application
              if ( curr->op == 'w' || curr->op == 'r' ) {
                    //|| curr->op == 's' ) { 
                  *compl_idx = *local_wq_tail;
                  *local_wq_tail += 1;

                  if (*local_wq_tail >= MAX_NUM_WQ) {
                      *local_wq_tail = 0;
                      *local_wq_SR ^= 1;
                  }

                  cq->q[*local_cq_head].tid = *compl_idx;
                  cq->q[*local_cq_head].SR = *local_cq_SR;

                  *local_cq_head += 1;
                  if(*local_cq_head >= MAX_NUM_WQ) {
                      *local_cq_head = 0;
                      *local_cq_SR ^= 1;
                  }
              } else if (curr->op == 's' || curr->op == 'g') {
                  // rmc send/recv. mark WQ as processed
                  *local_wq_tail += 1;
                  if (*local_wq_tail >= MAX_NUM_WQ) {
                      *local_wq_tail = 0;
                      *local_wq_SR ^= 1;
                  }
                  //mark the entry as invalid, i.e. completed
                  curr->valid = 0;
              } else {
                  DLogNoVar("Un-implemented op. in WQ entry. drop it on the floor.\n");
              }

#ifdef DEBUG_PERF_RMC
              clock_gettime(CLOCK_MONOTONIC, &end_time);
              start_time_ns = BILLION * start_time.tv_sec + start_time.tv_nsec;
              end_time_ns = BILLION * end_time.tv_sec + end_time.tv_nsec;
              printf("[main] notification latency: %u ns\n", end_time_ns - start_time_ns);
#endif

#ifdef DEBUG_PERF_RMC
              stimes.insert(stimes.begin(), end_time_ns - start_time_ns);

              if(stimes.size() == 100) {
                  long sum = 0;
                  sort(stimes.begin(), stimes.end());
                  for(int i=0; i<100; i++)
                      sum += stimes[i];

                  while (!stimes.empty())
                      stimes.pop_back();
              }
#endif
          } // end process all entries in this WQ
      }// end loop over qps

      // Msutherl: check all sockets (sinfos) for outstanding rpc
      for(i = 0; i < node_cnt; i++) {
          if( i != this_nid ) {
              memset(tmp_copies, 0, MAX_RPC_BYTES + RMC_Message::calcTotalHeaderBytes());
              char* rbuf = tmp_copies;
              // recv 4 bytes (header size) 
              int nrecvd = recv(sinfo[i].fd, rbuf, RMC_Message::getLenParamBytes() , MSG_DONTWAIT);
              int rec_round_2 = 0;
              if( nrecvd > 0 && nrecvd < (int) RMC_Message::getLenParamBytes() ) { 
                  DLog("[rmc_poll] got partial len from header, nbytes = %d\n",nrecvd);
                  rec_round_2 = recv(sinfo[i].fd, (rbuf+nrecvd), RMC_Message::getLenParamBytes() - nrecvd , 0); // block to get the rest of the header
                  if( rec_round_2 < 0 ) {
                      perror("[rmc_poll] Failed on recv(...) waiting for rest of length\n");
                  }
              } else if( nrecvd < 0 ) {
                  continue;
                  //perror("[rmc_poll] Failed on recv(...) waiting for first byte...\n");
              }

              // otherwise, we now have a full header
              assert( (nrecvd + rec_round_2) == (int) RMC_Message::getLenParamBytes() );

              // read it and figure out how much else to wait for
              uint32_t msgLengthReceived = ntohl(*((uint32_t*)rbuf));
              DLog("Next msg will come with length: %d\n",msgLengthReceived);
              nrecvd = recv(sinfo[i].fd, (rbuf + RMC_Message::getLenParamBytes()), msgLengthReceived - RMC_Message::getLenParamBytes(), 0); // block to get it all - *** Note: subtract getLenParamBytes() because I GOT THOSE BYTES ABOVE
              if( nrecvd > 0 ) {
#ifdef PRINT_BUFS
                  DLog("[rmc_poll] got rest of message, nbytes = %d\n",nrecvd);
                  DLog("Printing RPC Buffer after full message received.\n");
                  DumpHex( (char*)rbuf, nrecvd+RMC_Message::getLenParamBytes() );
#endif
                  RMC_Message msgReceived = unpackToRMC_Message(rbuf);
#ifdef PRINT_BUFS
                  if( msgReceived.msg_type == 's' ) {
                      DLog("Printing RPC Buffer after unpack to payload, message_len = %d.\n",msgReceived.message_len);
                      DumpHex( msgReceived.payload.data() , msgReceived.message_len );
                  }
#endif
                  switch( msgReceived.msg_type ) {
                      // check whether it's an rpc send, or recv to already sent rpc
                      case 's':
                          {
                              if( msgReceived.terminate_to_senders_qp == 't' ) {
                                  // this is an rpc response, goes to a specific QP
                                  uint16_t qp_to_terminate = msgReceived.senders_qp;
                                  /* 
                                   * 1) Create the srq metadata entry
                                   * 2) Copy tmp buf into rpc srq slot
                                   * 3) Direct dispatch to QP.
                                   */
                                  bool slot_available = !srq_full(&rpc_srq);
                                  if( slot_available ) {
                                      rpc_srq_entry_t newEntry;
                                      newEntry.sending_nid = i;
                                      newEntry.length = msgReceived.payload_len;
                                      int srq_slot = enqueue_in_srq(&rpc_srq,&newEntry);
                                      assert(srq_slot >= 0); // checked avail. slot above
                                      DLog("@ node %u, creating new DIRECT DISP. srq entry:\n"
                                              "\t{ sending_nid : %u },\n"
                                              "\t{ slot_idx : %u },\n"
                                              "\t{ length : %u },\n",
                                              this_nid, 
                                              newEntry.sending_nid,
                                              newEntry.slot_idx,
                                              newEntry.length);

                                      // 2) Copy into recv slots
                                      uint16_t recv_slot = newEntry.slot_idx;
                                      // copy tmp buf into actual recv slot (this is emulated and does not represent modelled zero-copy hardware)
                                      char* recv_slot_ptr = recv_slots  // base
                                          + (recv_slot * (MAX_RPC_BYTES));
                                      size_t arg_len = msgReceived.payload_len;
                                      memcpy((void*) recv_slot_ptr,msgReceived.payload.data(),arg_len);
#ifdef PRINT_BUFS
                                      DLog("[rmc_poll] After memcpy-ing unpacked data, message len: %d", msgReceived.message_len );
                                      DumpHex( recv_slot_ptr , msgReceived.message_len );
#endif
                                      // 3) Direct dispatch to QP
                                      cq = cqs[qp_to_terminate];
                                      assert( cq->connected );

                                      uint8_t* local_cq_head = &(local_CQ_heads[qp_to_terminate]);
                                      uint8_t* local_cq_SR = &(local_CQ_SRs[qp_to_terminate]);
                                      cq->q[*local_cq_head].sending_nid = i;
                                      cq->q[*local_cq_head].slot_idx = recv_slot;
                                      cq->q[*local_cq_head].length = msgReceived.getPayloadBytes();

                                      // SR is what app polls on, set it last
                                      cq->q[*local_cq_head].SR = *local_cq_SR;

                                      DLog("Received rpc SEND RESPONSE (\'s\') at rmc #%d. Receive-side QP info is:\n"
                                              "\t{ qp_to_terminate : %d },\n"
                                              "\t{ local_cq_head : %d },\n"
                                              "\t{ recv_slot : %d },\n",
                                              this_nid, 
                                              qp_to_terminate,
                                              *local_cq_head,
                                              srq_slot);
                                      *local_cq_head += 1;
                                      if(*local_cq_head >= MAX_NUM_WQ) {
                                          *local_cq_head = 0;
                                          *local_cq_SR ^= 1;
                                      }
                                      break;
                                  } else { // rpc. response with no srq space??
                                      assert(false); // FAIL!
                                  }
                              } else {
                                  // this is a new rpc request
                                  bool slot_available = !srq_full(&rpc_srq);
                                  if( slot_available ) {
                                      /* 
                                       * 1) Create the srq metadata entry
                                       * 2) Copy tmp buf into rpc srq slot
                                       * 3) Check for a dispatch decision
                                       */
                                      // this is an rpc request, goes to the srq
                                      rpc_srq_entry_t newEntry;
                                      newEntry.sending_nid = i;
                                      newEntry.sending_qp = msgReceived.senders_qp;
                                      newEntry.length = msgReceived.payload_len;
                                      int srq_slot = enqueue_in_srq(&rpc_srq,&newEntry);
                                      assert(srq_slot >= 0);
                                      DLog("@ node %u, creating new srq entry:\n"
                                              "\t{ sending_nid : %u },\n"
                                              "\t{ sending_qp: %u },\n"
                                              "\t{ slot_idx : %u },\n"
                                              "\t{ length : %u },\n",
                                              this_nid, 
                                              newEntry.sending_nid,
                                              newEntry.sending_qp,
                                              newEntry.slot_idx,
                                              newEntry.length);

                                      // 2) Copy into recv slots
                                      uint16_t recv_slot = newEntry.slot_idx;
                                      // copy tmp buf into actual recv slot (this is emulated and does not represent modelled zero-copy hardware)
                                      char* recv_slot_ptr = recv_slots  // base
                                          + (recv_slot * (MAX_RPC_BYTES));
                                      memcpy((void*) recv_slot_ptr,msgReceived.payload.data(),msgReceived.payload_len);
#ifdef PRINT_BUFS
                                      DLog("[rmc_poll] After memcpy-ing unpacked data, message len: %d", msgReceived.message_len );
                                      DumpHex( recv_slot_ptr , msgReceived.message_len );
#endif
                                       // 3) Check for a dispatch decision
                                      int dispatch_core_id = get_avail_core(rpcs_per_core);
                                      if( dispatch_core_id >= 0 ) {
                                          // dispatch head of srq
                                          rpc_srq_entry_t rpc_to_dispatch;

                                          get_dispatch_entry_from_srq(&rpc_srq,&rpc_to_dispatch);
                                          assert(rpc_to_dispatch.valid);

                                          // create CQ entry to send rpc to the core.
                                          cq = cqs[dispatch_core_id];
                                          assert( cq->connected );
                                          uint8_t* local_cq_head = &(local_CQ_heads[dispatch_core_id]);
                                          uint8_t* local_cq_SR = &(local_CQ_SRs[dispatch_core_id]);
                                          cq->q[*local_cq_head].sending_nid = rpc_to_dispatch.sending_nid;
                                          cq->q[*local_cq_head].sending_qp = rpc_to_dispatch.sending_qp;
                                          cq->q[*local_cq_head].slot_idx = rpc_to_dispatch.slot_idx;
                                          cq->q[*local_cq_head].length = rpc_to_dispatch.length;
                                          // SR is what app polls on, set it last
                                          cq->q[*local_cq_head].SR = *local_cq_SR;

                                          DLog("@ node %u, DISPATCHING TO:\n"
                                                  "\t{ qp_to_dispatch: %u },\n"
                                                  "\t{ sending_nid : %u },\n"
                                                  "\t{ sender's QP : %u },\n"
                                                  "\t{ slot_idx : %u },\n",
                                                  this_nid, 
                                                  dispatch_core_id,
                                                  rpc_to_dispatch.sending_nid,
                                                  rpc_to_dispatch.sending_qp,
                                                  rpc_to_dispatch.slot_idx );
                                          *local_cq_head += 1;
                                          if(*local_cq_head >= MAX_NUM_WQ) {
                                              *local_cq_head = 0;
                                              *local_cq_SR ^= 1;
                                          }
                                          increment_core_occupancy(rpcs_per_core,dispatch_core_id);
                                      } else {
                                          DLog("@ node %d, all cores OCCUPIED....\n",this_nid);
                                      }
                                  } else { // no slots in SRQ available
                                      /* Construct NACK msg and return it to requesting RMC */
                                      RMC_Message nack_msg(msgReceived.rpc_id,msgReceived.senders_qp,0/*slot irrelevant*/,'n');
                                      uint32_t bytesToSend = nack_msg.message_len;
                                      uint32_t copy = bytesToSend;
                                      char* packedBuffer = new char[bytesToSend];
                                      nack_msg.pack(packedBuffer);
                                      int retval = sendall(sinfo[i].fd,packedBuffer,&bytesToSend);
                                      if( retval < 0 ) {
                                          perror("[rmc_rpc] sending NACK failed, w. error:");
                                      } else if ( bytesToSend < copy) {
                                          printf("Only sent %d of %d bytes.... Do something about it!!!!\n",bytesToSend,copy);
                                      } else {}
                                      delete packedBuffer;
                                  }
                                  break;
                              } // end dispatch of new rpc
                          } // end case 's'
                      case 'g':
                          {
                              /* What is the role of rmc_recv() now with no
                               * explicit send slot to reset?
                               * - it's essentially an explicit ACK
                               */
#ifdef DEBUG_RMC
                              assert( msgReceived.rpc_id == 0 ); // for recv/replenish
#endif
                              DLog("Received rpc RECV (\'g\') at rmc #%d. Send-side QP info is:\n"
                                      "\t{ sender's QP : %d },\n"
                                      "\t{ slot_to_reuse : %d },\n",
                                      this_nid, 
                                      msgReceived.senders_qp,
                                      msgReceived.slot);
                              break;
                          }
                      case 'n':
                          {
                              /* Have to inform the upper level app that the rpc failed.
                               * (through a CQ entry)
                               * Params: 
                               * - QP to terminate to (contained in the message)
                               * - nack code
                               */
                              uint16_t qp_to_terminate = msgReceived.senders_qp;
                              cq = cqs[qp_to_terminate];
                              assert( cq->connected );
                              uint8_t* local_cq_head = &(local_CQ_heads[qp_to_terminate]);
                              uint8_t* local_cq_SR = &(local_CQ_SRs[qp_to_terminate]);
                              cq->q[*local_cq_head].sending_nid = i;
                              cq->q[*local_cq_head].is_nack = true;
                              // SRQ is what app polls on, set it last
                              cq->q[*local_cq_head].SR = *local_cq_SR;

                              DLog("Received rpc NACK (\'n\') at rmc #%d. Receive-side QP info is:\n"
                                      "\t{ qp_to_terminate : %d },\n"
                                      "\t{ nacking-rmc : %d },\n",
                                      this_nid, 
                                      qp_to_terminate,
                                      i
                                      );
                              *local_cq_head += 1;
                              if(*local_cq_head >= MAX_NUM_WQ) {
                                  *local_cq_head = 0;
                                  *local_cq_SR ^= 1;
                              }
                              break;
                          }
                      default:
                        DLogNoVar("Garbage op. in stream recv. from socket.... drop it on the floor.\n");
                  }
              } else { } // perror("[rmc_poll] got error:\n");
          }
      }
  } // end active rmc loop
  
  soft_rmc_ctx_destroy();
  
  printf("[main] RMC deactivated\n");

  // free memory
  free(wqs);
  free(cqs);
  free(local_buffers);
  free(local_WQ_tails);
  free(local_WQ_SRs);
  free(local_CQ_heads);
  free(local_CQ_SRs);
  free(compl_idx);
  free(sinfo);
  free(tmp_copies);
  free(recv_slots);
  return 0;
}

void deactivate_rmc()
{
  rmc_active = false;
}

