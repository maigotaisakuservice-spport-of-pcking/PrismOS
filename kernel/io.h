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

static inline void prism_outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t prism_inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void prism_outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t prism_inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void prism_halt(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static inline void prism_cli(void) {
    __asm__ volatile("cli");
}

static inline void prism_sti(void) {
    __asm__ volatile("sti");
}

#endif /* PRISM_IO_H */
