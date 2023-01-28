#include "ns.h"

extern union Nsipc nsipcbuf;
#define BUFSIZE 2048
#define RECV_GAP 10

void
input(envid_t ns_envid)
{
	binaryname = "ns_input";

	// 	- read a packet from the device driver
	//	- send it to the network server
	// Hint: When you IPC a page to the network server, it will be
	// reading from it for a while, so don't immediately receive
	// another packet in to the same physical page.
    char buf[BUFSIZE];
    int packet_size;
    while(1){
        //waiting for valid packet
        while(packet_size = sys_nic_recv(buf, BUFSIZE), packet_size < 0);
        memcpy(nsipcbuf.pkt.jp_data, buf, packet_size);
        nsipcbuf.pkt.jp_len = packet_size;
        ipc_send(ns_envid, NSREQ_INPUT, &nsipcbuf, PTE_P|PTE_U);
        //spin for a while
        for(int i = 0;i < RECV_GAP;i++)
            sys_yield();
    }
}
