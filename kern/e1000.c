#include "pci.h"
#include <kern/e1000.h>
#include <kern/pmap.h>

#define COMMAND_INIT 0x0000
#define STATUS_INIT 0x0230
#define CLASS_CODE 0x020000
#define REVID 0x00
int e1000_82540em_attach(struct pci_func* pcif){
    pci_func_enable(pcif);
    return 0;
}
