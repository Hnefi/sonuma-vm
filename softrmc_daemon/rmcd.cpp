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

using namespace std;

//server information
static server_info_t *sinfo;

static volatile bool rmc_active;
static int fd;

//partitioned global address space - one entry per region
static char *ctx[MAX_NODE_CNT];

static int node_cnt, this_nid;

// rpc messaging domain data
static char** recv_slots;
static char** sslots;
static char** avail_slots;

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


int count_valid_cq_entries(volatile rmc_cq_t* cq, int8_t local_cq_head)
{
    int tailIdx = cq->tail;
    int retCount = 0;
    while(cq->q[local_cq_head].SR == cq->SR
            && ( local_cq_head != tailIdx ) ) { // valid, hasn't over-run tail
        retCount++;
        local_cq_head--; // moves backwards to count towards the tail
        // check if WQ reached its end
        if (local_cq_head < 0 ) {
            local_cq_head = MAX_NUM_WQ-1;
        }
    }
    assert( retCount <= MAX_NUM_WQ );
    return retCount;
}

// Msutherl: returns qp number for RMC on server-side to terminate msg. into
// - default policy, shortest queue depth
uint8_t get_server_qp(volatile rmc_cq_t** cqs,uint8_t* local_cq_heads, int num_qps)
{
    unsigned minimum_outstanding_entries = MAX_NUM_WQ; // whole QP
    unsigned shortest_cq = 0;
    volatile rmc_cq_t* cq = NULL;
    for(int i = 0; i < num_qps;i++) {
        cq = cqs[i];
        if( cq->connected ) {
            unsigned nvalid = count_valid_cq_entries(cq,local_cq_heads[i]);
            if( nvalid < minimum_outstanding_entries ) {
                shortest_cq  = i;
                minimum_outstanding_entries = nvalid;
            }
        }
    }
    return shortest_cq;
}

static uint8_t qp_rr = 0;
static unsigned int qp_num_mod;
uint8_t get_server_qp_rrobin() { return (qp_rr++) % qp_num_mod; }

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
  char** tmp_copies;

  if(argc != 4) {
    printf("[main] incorrect number of arguments\n"
            "Usage: ./rmcd <Num. soNUMA Nodes> <ID of this node> <Num QPs to expose>\n");
    exit(1);
  }

  node_cnt = atoi(argv[1]);
  this_nid = atoi(argv[2]);
  unsigned num_qps = atoi(argv[3]);
  qp_num_mod = num_qps;
  
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

  // allocate messaging domain metadata
  tmp_copies = (char**) calloc(node_cnt,sizeof(char*));
  recv_slots = (char**) calloc(node_cnt,sizeof(char*));
  sslots = (char**) calloc(node_cnt,sizeof(char*));
  avail_slots = (char**) calloc(node_cnt,sizeof(char*));

  size_t recv_buffer_size = (MAX_RPC_BYTES) * MSGS_PER_PAIR ;
  size_t n_rbuf_pages = (recv_buffer_size / PAGE_SIZE) + 1;

  size_t send_slots_size = MSGS_PER_PAIR * sizeof(send_slot_t);
  size_t n_sslots_pages = (send_slots_size / PAGE_SIZE) + 1;

  size_t avail_slots_size = MSGS_PER_PAIR * sizeof(send_metadata_t);
  size_t n_avail_slots_pages = (avail_slots_size / PAGE_SIZE) + 1;
  for(int i = 0; i < node_cnt; i++ ) {
      char fmt[20];
      sprintf(fmt,"rqueue_node_%d.txt",i);
      local_buf_alloc(&(recv_slots[i]),fmt,n_rbuf_pages);
      sprintf(fmt,"send_slots_%d.txt",i);
      local_buf_alloc(&(sslots[i]),fmt,n_sslots_pages);
      sprintf(fmt,"avail_slots_%d.txt",i);
      local_buf_alloc(&(avail_slots[i]),fmt,n_avail_slots_pages);
      // for every node pair, init send slots metadata
      send_metadata_t* nodetmp = (send_metadata_t*)(avail_slots[i]);
      for(int tmp = 0; tmp < MSGS_PER_PAIR; tmp++) {
          nodetmp[tmp].valid = 1;
          nodetmp[tmp].sslot_index = tmp;
      }
      // default_rpc structure
      RMC_Message default_size_object(0,0,'s');
      // make a tmp buffer to hold RPC arguments
      tmp_copies[i] = (char*)calloc(MAX_RPC_BYTES + default_size_object.getTotalHeaderBytes(),sizeof(char));
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
                          RMC_Message msg((uint16_t)rpc_id, (uint16_t)qp_num,(uint16_t)curr->slot_idx,curr->op,(local_buffer + (curr->buf_offset)),curr->length,'t');
                          rpc_id++;
                          if( rpc_id == 0 ) rpc_id = 1; // 0 is a magic value used for rmc_recv()
                          uint32_t bytesToSend = msg.getRequiredLenBytes() + msg.getLenParamBytes();
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
                          uint32_t bytesToSend = msg.getRequiredLenBytes() + msg.getLenParamBytes();
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
      //uint32_t max_single_msg_size = MAX_RPC_BYTES + RMC_Message::getTotalHeaderBytes();
      for(i = 0; i < node_cnt; i++) {
          if( i != this_nid ) {
              char* rbuf = tmp_copies[i];
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
              assert( (nrecvd + rec_round_2) >= (int) RMC_Message::getLenParamBytes() );

              // read it and figure out how much else to wait for
              uint32_t msgLengthReceived = ntohl(*((uint32_t*)rbuf));
              DLog("Next msg will come with length: %d\n",msgLengthReceived);
              nrecvd = recv(sinfo[i].fd, (rbuf + RMC_Message::getLenParamBytes()), msgLengthReceived, 0); // block to get it all
              if( nrecvd > 0 ) {
#ifdef PRINT_BUFS
                  DLog("[rmc_poll] got rest of message, nbytes = %d\n",nrecvd);
                  DLog("Printing RPC Buffer after full message received.\n");
                  DumpHex( (char*)rbuf, nrecvd+RMC_Message::getLenParamBytes() );
#endif
                  RMC_Message msgReceived = unpackToRMC_Message(rbuf);
#ifdef PRINT_BUFS
                  if( msgReceived.msg_type == 's' ) {
                      DLog("Printing RPC Buffer after unpack to payload.\n");
                      DumpHex( msgReceived.payload.data() , msgReceived.getRequiredLenBytes() );
                  }
#endif
                  switch( msgReceived.msg_type ) {
                    // check whether it's an rpc send, or recv to already sent rpc
                      case 's':
                          {
                              uint16_t recv_slot = msgReceived.slot;
                              uint16_t sending_qp = msgReceived.senders_qp;
                              // copy tmp buf into actual recv slot (this is emulated
                              // and does not represent modelled zero-copy hardware)
                              char* recv_slot_ptr = recv_slots[i]  // base
                                  + (recv_slot * (MAX_RPC_BYTES));
                              size_t arg_len = msgLengthReceived - msgReceived.getMessageHeaderBytes();
                              memcpy((void*) recv_slot_ptr,msgReceived.payload.data(),arg_len);
#ifdef PRINT_BUFS
                              DLog("[rmc_poll] After memcpy-ing unpacked data, message len: %d", msgReceived.getRequiredLenBytes() );
                              DumpHex( recv_slot_ptr , msgReceived.getRequiredLenBytes() );
#endif
                              uint8_t qp_to_terminate;
                              if( msgReceived.terminate_to_senders_qp == 't' ) {
                                  qp_to_terminate = sending_qp;
                                  cq = cqs[qp_to_terminate];
                                  assert( cq->connected );
                              } else { // rrobin
                                  qp_to_terminate = get_server_qp_rrobin();
                                  cq = cqs[qp_to_terminate];
                              }
                              uint8_t* local_cq_head = &(local_CQ_heads[qp_to_terminate]);
                              uint8_t* local_cq_SR = &(local_CQ_SRs[qp_to_terminate]);
                              cq->q[*local_cq_head].SR = *local_cq_SR;
                              cq->q[*local_cq_head].sending_nid = i;
                              cq->q[*local_cq_head].tid = sending_qp;
                              cq->q[*local_cq_head].slot_idx = recv_slot;
                              cq->q[*local_cq_head].length = arg_len ;
                              DLog("Received rpc SEND (\'s\') at rmc #%d. Receive-side QP info is:\n"
                                      "\t{ qp_to_terminate : %d },\n"
                                      "\t{ local_cq_head : %d },\n"
                                      "\t{ sender's QP : %d },\n"
                                      "\t{ recv_slot : %d },\n",
                                      this_nid, 
                                      qp_to_terminate,
                                      *local_cq_head,
                                      sending_qp,
                                      recv_slot );
                              *local_cq_head += 1;
                              if(*local_cq_head >= MAX_NUM_WQ) {
                                  *local_cq_head = 0;
                                  *local_cq_SR ^= 1;
                              }
                              break;
                          }
                      case 'g':
                          {
#ifdef DEBUG_RMC
                              assert( msgReceived.rpc_id == 0); // for recv/replenish
#endif
                              uint16_t sending_qp = msgReceived.senders_qp;
                              uint8_t slot_to_reuse = msgReceived.slot;
                              DLog("Received rpc RECV (\'g\') at rmc #%d. Send-side QP info is:\n"
                                      "\t{ sender's QP : %d },\n"
                                      "\t{ slot_to_reuse : %d },\n",
                                      this_nid, 
                                      sending_qp,
                                      slot_to_reuse);
                              // Operations: invalidate send slots and avail-tracker
                              /*send_slot_t* slots_from_node = (send_slot_t*)sslots[i];
                              send_slot_t* reuseMe = (slots_from_node + slot_to_reuse);
                              reuseMe->valid = 0;
                              */
                              send_metadata_t* meta_from_node = (send_metadata_t*)avail_slots[i];
                              send_metadata_t* meToo = (meta_from_node + slot_to_reuse);

                              // use CAS to ensure previous value is reset
                              int test_val = 0, new_val = 1;
                              bool success = meToo->valid.compare_exchange_strong(test_val, new_val);
                              if( !success ) {
                                  assert( test_val == 1 );
                                  std::cout << "WE HAD SOME MAJOR FAILURE HERE." << std::endl
                                            << "\t RMC tried to reset send slot #" << (unsigned int) slot_to_reuse
                                            << ", belonging to node #" << i
                                            << ", and found that its value was not 0. Value loaded: " << test_val << std::endl;
                                  printf("Error occurred on rpc RECV (\'g\') at rmc #%d. slot QP info is:\n"
                                          "\t{ sender's QP : %d },\n"
                                          "\t{ slot_to_reuse : %d },\n",
                                          this_nid, 
                                          sending_qp,
                                          slot_to_reuse);
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
  for(int i = 0; i < node_cnt; i++ ) {
      free(tmp_copies[i]);
  }
  free(tmp_copies);
  free(recv_slots);
  free(sslots);
  free(avail_slots);
  return 0;
}

void deactivate_rmc()
{
  rmc_active = false;
}

