#
# Scale-Out NUMA Open Source License
#
# Copyright (c) 2017, Parallel Systems Architecture Lab, EPFL
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:

# * Redistributions of source code must retain the above copyright notice,
#   this list of conditions and the following disclaimer.
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
# * Neither the name of the Parallel Systems Architecture Lab, EPFL,
#   nor the names of its contributors may be used to endorse or promote
#   products derived from this software without specific prior written
#   permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE PARALLEL SYSTEMS ARCHITECTURE LAB,
# EPFL BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
# GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
# THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

SHELL = /bin/sh
CC    = g++
CXX = g++

CFLAGS	+= -I$(LIBSONUMA_PATH) -std=c++11
LDFLAGS	= -L$(LIBSONUMA_PATH) -Wl,-rpath,$(LIBSONUMA_PATH) -lsonuma -lrt

CXXFLAGS = $(CFLAGS)

TARGETS  = bench_sync bench_async bench_server multi_qp_read rpc_sync rpc_service many_to_one
SOURCES = bench_sync.c bench_server.c bench_async.c multi_qp_read.c rpc_sync.c rpc_service.c rpc_service_multiqp.c many_to_one.cc
OBJECTS = $(SOURCES:.c=.o)

TARGETSYNC = bench_sync
SOURCESSYNC = bench_sync.c
OBJECTSSYNC = $(SOURCESSYNC:.c=.o)

RPC_TARG = rpc_sync
RPC_SOURCES = rpc_sync.c
RPC_OBJS = $(RPC_SOURCES:.c=.o)

SERV_TARG = rpc_service 
SERV_SOURCES = rpc_service.c
SERV_OBJS = $(SERV_SOURCES:.c=.o)

MULTI_TARG = rpc_service_multiqp
MULTI_SOURCES = rpc_service_multiqp.c
MULTI_OBJS = $(MULTI_SOURCES:.c=.o)

THREAD_TARG = many_to_one
THREAD_SOURCES = many_to_one.cc
THREAD_OBJS = $(THREAD_SOURCES:.cc=.o)

TARGET1  = bench_server

SOURCES1 = \
	./bench_server.c

OBJECTS1 = $(SOURCES1:.c=.o)

TARGET2  = bench_async

SOURCES2 = \
	./bench_async.c

OBJECTS2 = $(SOURCES2:.c=.o)

TARGET3 = multi_qp_read

SOURCES3 = \
	./multi_qp_read.c

OBJECTS3 = $(SOURCES3:.c=.o)

default: $(TARGETS)

%.o : %.c
	$(CC) $(CFLAGS) -c $^ -o $@ 

$(TARGETSYNC): $(OBJECTSSYNC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) 
	cp ./$@ ../bin/

$(TARGET1): $(OBJECTS1)
	$(CC) $(CFLAGS) -o $(TARGET1) $(OBJECTS1) $(LDFLAGS) 
	cp ./$@ ../bin/

$(TARGET2): $(OBJECTS2)
	$(CC) $(CFLAGS) -o $(TARGET2) $(OBJECTS2) $(LDFLAGS) 
	cp ./$@ ../bin/

$(TARGET3): $(OBJECTS3)
	$(CC) $(CFLAGS) -o $(TARGET3) $(OBJECTS3) $(LDFLAGS) 
	cp ./$@ ../bin/

$(RPC_TARG): $(RPC_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) 
	cp ./$@ ../bin/

$(SERV_TARG): $(SERV_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) 
	cp ./$@ ../bin/

$(MULTI_TARG): $(MULTI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) 
	cp ./$@ ../bin/

$(THREAD_TARG): $(THREAD_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lpthread
	cp ./$@ ../bin/

clean:
	-${RM} ${TARGETS} ${OBJECTS} $(SRCS:.c=.o)

