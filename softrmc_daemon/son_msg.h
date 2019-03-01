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
 *  Header structure for a soNUMA message between RMCs.
 */

#ifndef _SON_MSG_H
#define _SON_MSG_H
#include "RMCdefines.h"
#include <vector>

#ifdef __CPLUSPLUS
extern "C" {
#endif

class RMC_Message {
    // all data in here is uint32 and uint16 (unsigned long & unsigned short)
    // to use the htonl(...) and htons(...) functions
    public:
        uint32_t message_len;       // 4B on wire
        char msg_type;              // 1B on wire
        char terminate_to_senders_qp; // 1B
        uint16_t rpc_id;            // 2B
        uint16_t senders_qp;        // 2B
        uint16_t slot;              // 2B
        std::vector<char> payload; // max 2048B

        // THINGS NOT SENT ON WIRE // 
        uint32_t payload_len; 

        RMC_Message(uint16_t anRPC_ID, uint16_t aQP, uint16_t aSlot, char aType, char* aPayloadPtr,uint32_t payloadLen, char useQP_to_terminate);
        RMC_Message(uint16_t anRPC_ID,uint16_t aQP, uint16_t aSlot, char aType);
        RMC_Message(uint16_t aQP, uint16_t aSlot, char aType);
        void pack(char* buf);
        static uint32_t calcTotalHeaderBytes() {
            uint32_t hbytes = 
                sizeof(uint32_t) + // message_len;       // 4B on wire
                sizeof(char) +     // msg_type;              // 1B on wire
                sizeof(char) +     // terminate_to_senders_qp; // 1B
                sizeof(uint16_t) + // rpc_id;            // 2B
                sizeof(uint16_t) + // senders_qp;        // 2B
                sizeof(uint16_t) ; // slot;              // 2B
            return hbytes;
        }

        uint32_t getPayloadBytes() { return message_len - RMC_Message::calcTotalHeaderBytes(); }
        uint32_t getMessageHeaderBytes() { return RMC_Message::calcTotalHeaderBytes() -getLenParamBytes(); }
        static uint32_t getLenParamBytes() { return sizeof(uint32_t); }
};

RMC_Message unpackToRMC_Message(char* aNetworkBuffer);

#ifdef __CPLUSPLUS
}
#endif

#endif // #ifndef _SON_MSG_H
