#ifndef JOS_KERN_E1000_H
#define JOS_KERN_E1000_H
#endif  // SOL >= 6

#include <kern/pci.h>
#define PCI_82540EM_VENDOR 0x8086
#define PCI_82540EM_DESKTOP_DEVICE 0x100e
#define PCI_82540EM_MOBILE_DEVICE 0X1015

#define E_PACKET_TOO_BIG 1
#define E_TX_OVERFLOW 2

#define PCI_82540EM_DESKTOP_ATTACH \
    { PCI_82540EM_VENDOR, PCI_82540EM_DESKTOP_DEVICE, e1000_82540em_attach }
int e1000_82540em_attach(struct pci_func* pcif);
int e1000_82540em_send(const void* packet, int size);
