#include "pci.h"
#include <kern/e1000.h>
#include <kern/pmap.h>
#include <inc/string.h>

#define TX_QUEUE_SIZE 32
#define TX_BUFFER_SIZE 1600

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

#define STATUS_DD (1 << 0)
#define CMD_EOP (1 << 0)
#define CMD_RS (1 << 3)
#define CMD_DEXT (1 << 5)

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

struct tx_desc descs[TX_QUEUE_SIZE];
struct tx_buffer buffers[TX_QUEUE_SIZE];
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
