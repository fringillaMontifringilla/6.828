#include "ns.h"

extern union Nsipc nsipcbuf;

void
output(envid_t ns_envid)
{
	binaryname = "ns_output";

	// 	- read a packet from the network server
	//	- send the packet to the device driver
    while(1){
        envid_t from_envid;
        int32_t result = ipc_recv(&from_envid, &nsipcbuf, 0);
        if(result < 0)
            return;
        if(from_envid != ns_envid)
            return;
        //now receive a packet
        switch(result){
            case NSREQ_OUTPUT:
                int r = sys_nic_transmit(nsipcbuf.pkt.jp_data, nsipcbuf.pkt.jp_len);
                while(r)
                    r = sys_nic_transmit(nsipcbuf.pkt.jp_data, nsipcbuf.pkt.jp_len);
                break;
        }
    }
}
