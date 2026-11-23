/* PrismOS - e1000 network card driver (82540EM, polling).
 *
 * Finds the card on PCI, sets up TX/RX descriptor rings in
 * pages from the physical allocator, and moves raw Ethernet
 * frames. No interrupts: the stack polls e1000_recv().
 * The card's registers are reached through the bootloader
 * HHDM mapping (physical address + offset = virtual).
 *
 * The card is the QEMU default network card; with the
 * default user networking it is connected to a virtual
 * network that offers DHCP, DNS and a gateway.
 */
#ifndef PRISM_E1000_H
#define PRISM_E1000_H

#include <stddef.h>
#include <stdint.h>

/* Initialise the card. hhdm_offset is the bootloader offset
 * (physical + offset = virtual). Returns 0 on success. */
int e1000_init(uint64_t hhdm_offset);
int e1000_ready(void);

/* The card's 6-byte hardware address. */
const uint8_t *e1000_ethaddr(void);
/* Link up (STATUS bit) when the card reports it. */
int e1000_link_up(void);

/* Send one frame (14..1514 bytes). Returns 0 on success. */
int e1000_send(const uint8_t *frame, size_t len);
/* Receive one frame into buf (at most max_len). Returns the
 * length, or 0 when nothing is waiting, or -1 on error. */
int e1000_recv(uint8_t *buf, size_t max_len);

#endif /* PRISM_E1000_H */
