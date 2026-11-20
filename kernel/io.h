/* PrismOS — x86 ポート I/O ヘルパー (freestanding) */
#ifndef PRISM_IO_H
#define PRISM_IO_H

#include <stdint.h>

static inline void prism_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t prism_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void prism_halt(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

#endif /* PRISM_IO_H */
