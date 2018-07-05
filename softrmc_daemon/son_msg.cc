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
 *  Implementation for RMC_Message functions:
 *  - network byte order
 *  - serialize/deserialize
 *  - construct w. or w/o copy
 */
#include "son_msg.h"
#include <sys/types.h>
#include <arpa/inet.h>
#include <cstring>

using namespace std;

void write_buf(char* buf, size_t len)
{
    for(int i = 0; i < len;i++) {
        printf("Buffer[%d] = %c\n",i,buf[i]);
    }
}

uint32_t RMC_Message::total_header_bytes = 9; // FIXME: do this dynamically?

RMC_Message::RMC_Message(uint16_t aQP, uint16_t aSlot, char aType,char* aPayloadPtr,uint32_t aPayLen) :
    message_len(1+2+2+aPayLen), // does not include 4B for len itself
    msg_type(aType),
    senders_qp(aQP),
    slot(aSlot),
    payload(aPayloadPtr,aPayloadPtr+aPayLen),
    payload_len(aPayLen)
{ }

uint32_t
RMC_Message::getRequiredLenBytes() { return message_len; }

// This function assumes a byte is 8b (64 bit system)
void
RMC_Message::pack(char* buf)
{
    uint32_t net_message_len = htonl(this->message_len);
    memcpy(buf,&net_message_len,sizeof(uint32_t));
    buf += sizeof(uint32_t);

    memcpy(buf,&(this->msg_type),sizeof(char));
    buf += sizeof(char);

    uint16_t net_senders_qp = htons(this->senders_qp);
    memcpy(buf,&net_senders_qp,sizeof(uint16_t));
    buf += sizeof(uint16_t);

    uint16_t net_slot = htons(this->slot);
    memcpy(buf,&net_slot,sizeof(uint16_t));
    buf += sizeof(uint16_t);

    // FIXME: this is not sent "portably"
    // - could encode in string if need arises
    memcpy(buf,this->payload.data(),this->payload_len);
    buf += this->payload_len;
}

// Does the reverse of pack(...)
//  - use ntohl() and ntohs()
RMC_Message unpackToRMC_Message(char* buf)
{
    char* aNetworkBuffer = buf;
    uint32_t message_len;
    uint16_t senders_qp, slot;
    char mType;

    uint32_t* mlen_tmptr = (uint32_t*) aNetworkBuffer;
    message_len = ntohl(*mlen_tmptr);
    aNetworkBuffer += sizeof(uint32_t);

    char* mtype_tmptr = aNetworkBuffer;
    mType = *aNetworkBuffer;
    aNetworkBuffer += sizeof(char);

    uint16_t* senderQP_tmptr = (uint16_t*) aNetworkBuffer;
    printf("[senderQP addr %p]: Raw value before ntohs: %d\n",senderQP_tmptr,*senderQP_tmptr);
    senders_qp = ntohs(*senderQP_tmptr);
    printf("[senderQP addr] Value after ntohs: %d\n",senders_qp);
    aNetworkBuffer += sizeof(uint16_t);

    uint16_t* slot_tmptr = (uint16_t*) aNetworkBuffer;
    printf("[slot paddr %p]: Raw value before ntohs: %d\n",slot_tmptr,*slot_tmptr);
    slot = ntohs(*slot_tmptr);
    printf("[slot paddr] Value after ntohs: %d\n",slot);
    printf(" Demultiplexed: message_len %d\n "
           " : mtype %c\n "
           " : senders_qp %d\n "
           " : slot %d\n ",
           message_len, 
           mType,
           senders_qp,
           slot);
    aNetworkBuffer += sizeof(uint16_t);
    return RMC_Message( senders_qp,slot,mType,(buf + RMC_Message::total_header_bytes), (message_len - RMC_Message::total_header_bytes) );
}
