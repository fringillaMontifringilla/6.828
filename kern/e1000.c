#include "pci.h"
#include <kern/e1000.h>
#include <kern/pmap.h>
#include <inc/string.h>

#define MAC_HIGH 0x5634
#define MAC_LOW 0x12005452

#define TX_QUEUE_SIZE 32
#define TX_BUFFER_SIZE 2048
#define RX_QUEUE_SIZE 256
#define RX_BUFFER_SIZE 2048

#define DEVICE_STATUS_REG 2
#define TDBAL (0x3800 / 4)
#define TDBAH (0x3804 / 4)
#define TDLEN (0x3808 / 4)
#define TDH (0x3810 / 4)
#define TDT (0x3818 / 4)
#define TCTL (0x400 / 4)
#define TCTL_EN (1 << 1)
#define TCTL_PSP (1 << 3)
#define TCTL_COLD(val) ((val) << 12)
#define TCTL_CT(val) ((val) << 4)
#define TIPG (0x410 / 4)
#define TIPG_IPGT(val) (val)
#define TIPG_IPGR1(val) ((val) << 10)
#define TIPG_IPGR2(val) ((val) << 20)
#define RAL_BASE (0x5400 / 4)
#define RAH_BASE (0x5404 / 4)
#define RAH_AV (1 << 31)
#define RAH_AS_MASK (0x3 << 16)
#define RAH_AS_DEST (0x0 << 16)
#define RAH_AS_SRC (0x1 << 16)
#define RAH_RAH(val) (val)
#define RA_GAP 8
#define RA_LIMIT 16
#define MTA_BASE (0x5200 / 4)
#define MTA_LIMIT 128
#define IMS (0xd0 / 4)
#define IMC (0xd8 / 4)
#define INT_ALL_MASK ((1 << 17) - 1)
#define RDBAL (0x2800 / 4)
#define RDBAH (0x2804 / 4)
#define RDLEN (0x2808 / 4)
#define RDH (0x2810 / 4)
#define RDT (0x2818 / 4)
#define RCTL (0x100 / 4)
#define RCTL_EN (1 << 1)
#define RCTL_LPE (1 << 5)
#define RCTL_LBM_MASK (0x3 << 6)
#define RCTL_LBM_NOLOOP (0x0 << 6)
#define RCTL_LBM_LOOP (0x3 << 6)
#define RCTL_RDMTS_MASK (0x3 << 8)
#define RCTL_RDMTS_HALF (0x0 << 8)
#define RCTL_RDMTS_QUAR (0x1 << 8)
#define RCTL_RDMTS_ONE_EIGHTH (0x2 << 8)
#define RCTL_MO_MASK (0x3 << 12)
#define RCTL_MO_47 (0x0 << 12)
#define RCTL_MO_46 (0x1 << 12)
#define RCTL_MO_45 (0x2 << 12)
#define RCTL_MO_43 (0x3 << 12)
#define RCTL_BAM (1 << 15)
#define RCTL_BSIZE_MASK (0x3 << 16)
#define RCTL_BSIZE_2048 (0x0 << 16)
#define RCTL_SECRC (1 << 26)

#define STATUS_DD (1 << 0)
#define CMD_EOP (1 << 0)
#define CMD_RS (1 << 3)
#define CMD_DEXT (1 << 5)

#define RX_STATUS_DD (1 << 0)
#define RX_STATUS_EOP (1 << 1)

#define DEVICE(dev) (((dev) >> 16) & 0xffff)
#define VENDOR(dev) ((dev) & 0xffff)

struct tx_desc{
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint8_t special;
};
struct tx_buffer{
    uint8_t buffer[TX_BUFFER_SIZE];
};
struct rx_desc{
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t err;
    uint16_t special;
};
struct rx_buffer{
    uint8_t buffer[RX_BUFFER_SIZE];
};

struct tx_desc descs[TX_QUEUE_SIZE];
struct tx_buffer buffers[TX_QUEUE_SIZE];
struct rx_desc rxdescs[RX_QUEUE_SIZE];
struct rx_buffer rxbuffers[RX_QUEUE_SIZE];
volatile uint32_t* mmio;
int e1000_82540em_attach(struct pci_func* pcif){
    //enable device
    pci_func_enable(pcif);
    //setup mmio
    mmio = mmio_map_region(pcif -> reg_base[0], pcif -> reg_size[0]);
    cprintf("PCI[%04x:%04x] E1000-82540EM-A device status:%08x\n", VENDOR(pcif->dev_id), DEVICE(pcif->dev_id), mmio[DEVICE_STATUS_REG]);
    //setup tx queue
    for(int i = 0;i < TX_QUEUE_SIZE;i++){
        descs[i].addr = 0;
        descs[i].length = 0;
        descs[i].cso = 0;
        descs[i].cmd = 0;
        descs[i].status = STATUS_DD;
        descs[i].css = 0;
        descs[i].special = 0;
    }
    cprintf("PCI[%04x:%04x] E1000-82540EM-A tx buffer count:%d, size:%d\n", VENDOR(pcif->dev_id), DEVICE(pcif->dev_id), TX_QUEUE_SIZE, TX_BUFFER_SIZE);
    //install tx queue
    mmio[TDBAL] = PADDR(descs);
    mmio[TDBAH] = 0;
    mmio[TDLEN] = TX_QUEUE_SIZE * sizeof(struct tx_desc);
    mmio[TDH] = 0;
    mmio[TDT] = 0;
    mmio[TCTL] = TCTL_EN | TCTL_PSP | TCTL_CT(0x10) | TCTL_COLD(0x40);
    mmio[TIPG] = TIPG_IPGT(10) | TIPG_IPGR1(8) | TIPG_IPGR2(6);
    //setup rx configurations
    mmio[RAL_BASE] = MAC_LOW;
    mmio[RAH_BASE] = RAH_RAH(MAC_HIGH) | RAH_AV | RAH_AS_DEST;
    for(int i = 0;i < MTA_LIMIT;i++)
        mmio[MTA_BASE + i] = 0;
    //disable interrupt now
    mmio[IMC] = INT_ALL_MASK;
    //skip RDTR due to disabled RDMT interrupt
    //setup rx queue
    for(int i = 0;i < RX_QUEUE_SIZE;i++){
        rxdescs[i].addr = PADDR(rxbuffers[i].buffer);
        rxdescs[i].checksum = 0;
        rxdescs[i].length = 0;
        rxdescs[i].err = 0;
        rxdescs[i].status = 0;
        rxdescs[i].special = 0;
    }
    cprintf("PCI[%04x:%04x] E1000-82540EM-A rx buffer count:%d, size:%d\n", VENDOR(pcif->dev_id), DEVICE(pcif->dev_id), RX_QUEUE_SIZE, RX_BUFFER_SIZE);
    //install rx queue
    mmio[RDBAL] = PADDR(rxdescs);
    mmio[RDBAH] = 0;
    mmio[RDLEN] = RX_QUEUE_SIZE * sizeof(struct rx_desc);
    mmio[RDH] = 0;
    mmio[RDT] = RX_QUEUE_SIZE - 1;
    //other rctl settings
    mmio[RCTL] = RCTL_EN | (~RCTL_LPE) | RCTL_LBM_NOLOOP | RCTL_RDMTS_ONE_EIGHTH | RCTL_MO_47 | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC;
    return 0;
}

int e1000_82540em_send(const void* packet, int size){
    if(size > TX_BUFFER_SIZE)
        return -E_PACKET_TOO_BIG;
    uint32_t next = mmio[TDT];
    if(!(descs[next].status & STATUS_DD))
        return -E_TX_OVERFLOW;
    memcpy(buffers[next].buffer, packet, size);
    descs[next].addr = PADDR(&buffers[next].buffer);
    descs[next].length = size;
    descs[next].cmd |= CMD_RS | CMD_EOP;
    descs[next].cmd &= (~CMD_DEXT);
    descs[next].status &= (~STATUS_DD);
    mmio[TDT] = (next + 1) % TX_QUEUE_SIZE;
    return 0;
}

int e1000_82540em_recv(void* buf, int limit){
    static int next = 0;
    int size = 0;
    if(!(rxdescs[next].status & RX_STATUS_DD))
        return -E_RX_NOT_RECV;
    if(!(rxdescs[next].status & RX_STATUS_EOP)){
        size = -E_RX_LONG_PACKET;
        goto end;
    }
    int packet_size = rxdescs[next].length;
    size = (packet_size > limit) ? limit : packet_size;
    memcpy(buf, rxbuffers[next].buffer, size);
end:
    rxdescs[next].status = 0;
    next = (next + 1) % RX_QUEUE_SIZE;
    uint32_t rdt = mmio[RDT];
    mmio[RDT] = (rdt + 1) % RX_QUEUE_SIZE;
    return size;
}
