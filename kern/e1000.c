#include "pci.h"
#include <kern/e1000.h>
#include <kern/pmap.h>

#define DEVICE_STATUS_REG 2
#define DEVICE(dev) (((dev) >> 16) & 0xffff)
#define VENDOR(dev) ((dev) & 0xffff)

volatile uint32_t* mmio;
int e1000_82540em_attach(struct pci_func* pcif){
    pci_func_enable(pcif);
    mmio = mmio_map_region(pcif -> reg_base[0], pcif -> reg_size[0]);
    cprintf("PCI[%04x:%04x] E1000-82540EM-A device status:%08x\n", VENDOR(pcif->dev_id), DEVICE(pcif->dev_id), mmio[DEVICE_STATUS_REG]);
    return 0;
}
