#include <stdlib.h>
#include <memory.h>
#include <stdio.h>
#include <unistd.h>

#include "libpaxos_priv.h"
#include "paxos_udp.h"

// Do some "superficial" check on the received message, 
// for example size should match the expected, proposer/acceptor IDs
// should be within given bounds, etc
// Returns 0 for valid, -1 for bad message
static int validate_paxos_msg(paxos_msg * m, size_t msg_size) {
    size_t expected_size = sizeof(paxos_msg);
    
    //Msg size should match received size
    if (m->data_size + sizeof(paxos_msg) != msg_size) {
        printf("Invalid message, declared size:%lu received size:%u\n", \
            (m->data_size + sizeof(paxos_msg)), (unsigned int)msg_size);
        return -1;
    }
    
    switch(m->type) {
        case accept_acks: {
            accept_ack_batch * aa = (accept_ack_batch *)m->data;
            //Acceptor id out of bounds
            if(aa->acceptor_id < 0 || aa->acceptor_id >= N_OF_ACCEPTORS) {
                printf("Invalida acceptor id:%d\n", aa->acceptor_id);
                return -1;
            }
            // expected_size += ACCEPT_ACK_BATCH_SIZE(aa);
            expected_size += m->data_size;
        }
        break;
        
        case repeat_reqs: {
            repeat_req_batch * rrb = (repeat_req_batch *)m->data;
            expected_size += REPEAT_REQ_BATCH_SIZE(rrb);
        }
        break;
        
        default: {
            printf("Unknow paxos message type:%d\n", m->type);
            return -1;
        }
        
        //Checks message data field size based on message type
        if(msg_size != expected_size) {
            printf("Invalid size for msg_type:%d declared:%u received:%u\n", \
                m->type, (unsigned int)expected_size, (unsigned int)msg_size);
        }
    }
    return 0;
}

//Creates a new non-blocking UDP multicast receiver for the given address/port
udp_receiver * udp_receiver_new(char* address_string, int port) {
    udp_receiver * rec = PAX_MALLOC(sizeof(udp_receiver));

    struct ip_mreq mreq;
    
    memset(&mreq, '\0', sizeof(struct ip_mreq));
    // Set up socket 
    rec->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (rec->sock < 0) {
        perror("receiver socket");
        return NULL;
    }
    
    // Set to reuse address   
    int activate = 1;
    if (setsockopt(rec->sock, SOL_SOCKET, SO_REUSEADDR, &activate, sizeof(int)) != 0) {
        perror("setsockopt, setting SO_REUSEADDR");
        return NULL;
    }

    // Set up membership to multicast group 
    mreq.imr_multiaddr.s_addr = inet_addr(address_string);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(rec->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        perror("setsockopt, setting IP_ADD_MEMBERSHIP");
        return NULL;
    }

    // Set up address 
    struct sockaddr_in * addr_p = &rec->addr;
    addr_p->sin_addr.s_addr = inet_addr(address_string);
    if (addr_p->sin_addr.s_addr == INADDR_NONE) {
        printf("Error setting receiver->addr\n");
        return NULL;
    }
    addr_p->sin_family = AF_INET;
    addr_p->sin_port = htons((uint16_t)port);   

    // Bind the socket 
    if (bind(rec->sock, (struct sockaddr *) &rec->addr, sizeof(struct sockaddr_in)) != 0) {
        perror("bind");
        return NULL;
    }

    // Set non-blocking 
    int flag = fcntl(rec->sock, F_GETFL);
    if(flag < 0) {
        perror("fcntl1");
        return NULL;
    }
    flag |= O_NONBLOCK;
    if(fcntl(rec->sock, F_SETFL, flag) < 0) {
        perror("fcntl2");
        return NULL;
    }
    
    LOG(DBG, ("Socket %d created for address %s:%d (receive mode)\n", rec->sock, address_string, port));
    return rec;
}

//Destroys the given UDP receiver
int udp_receiver_destroy(udp_receiver * rec) {
    int ret = 0;
    
    // Close the socket
    if (close(rec->sock) != 0) {
        printf("Error closing socket\n");
        perror("close");
        ret = -1;
    }
    LOG(DBG, ("Socket %d closed\n", rec->sock));
    
    //Free the structure
    PAX_FREE(rec);
    return ret;
}

//Tries to read the next message from socket into the local buffer.
// This function is registered with libevent and invoked automatically 
// when a new message is available in the system buffer.
// Returns 0 for a valid message, -1 otherwise
int udp_read_next_message(udp_receiver * recv_info) {
    
    //Get the message
    socklen_t addrlen = sizeof(struct sockaddr);
    int msg_size = recvfrom(recv_info->sock,    //Socket to read from
        recv_info->recv_buffer,                 //Where to store the msg
        MAX_UDP_MSG_SIZE,                       //Size of buffer
        MSG_WAITALL,                            //Get the entire message
        (struct sockaddr *)&recv_info->addr,    //Address
        &addrlen);                              //Address length

    //Error in recvfrom
    if (msg_size < 0) {
        perror("recvfrom");
        return -1;
    }
    
    return validate_paxos_msg((paxos_msg*)recv_info->recv_buffer, msg_size);
}
