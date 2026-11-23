/* PrismOS — ARM 用 Flattened Device Tree (FDT) の最小パーサ
 *
 * バイナリはビッグエンディアン。構造ブロックのトークンを
 * 辿り、"memory" で始まる名前のノードの reg を読む。
 */
#include "fdt.h"

#include <stddef.h>

#define FDT_MAGIC 0xd00dfeedu
#define FDT_BEGIN_NODE 1u
#define FDT_END_NODE 2u
#define FDT_PROP 3u
#define FDT_NOP 4u
#define FDT_END 9u

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int name_starts(const char *a, const char *prefix) {
    while (*prefix) {
        if (*a != *prefix) {
            return 0;
        }
        a++;
        prefix++;
    }
    return 1;
}

static uint64_t read_cells(const uint8_t *p, uint32_t cells) {
    uint64_t v = 0;
    uint32_t i;
    for (i = 0; i < cells; i++) {
        v = (v << 32) | be32(p + i * 4);
    }
    return v;
}

int arm_fdt_memory(uint64_t dtb_addr, uint64_t *out_base,
                   uint64_t *out_size) {
    const uint8_t *dtb = (const uint8_t *)(uintptr_t)dtb_addr;
    uint32_t off_struct, off_strings;
    const uint8_t *sp, *strings;
    uint32_t addr_cells = 2, size_cells = 1;
    int depth = 0;
    int in_memory = 0;
    int guard = 0;

    if (dtb_addr == 0) {
        return -1;
    }
    if (be32(dtb) != FDT_MAGIC) {
        return -1;
    }
    off_struct = be32(dtb + 8);
    off_strings = be32(dtb + 12);
    sp = dtb + off_struct;
    strings = dtb + off_strings;

    while (guard++ < 100000) {
        uint32_t token = be32(sp);
        sp += 4;
        if (token == FDT_END) {
            break;
        } else if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)sp;
            size_t len = 0;
            while (name[len] != '\0') {
                len++;
            }
            sp += (len + 1 + 3) & ~(size_t)3;
            depth++;
            in_memory = (depth == 2 && name_starts(name, "memory"));
        } else if (token == FDT_END_NODE) {
            if (depth == 2) {
                in_memory = 0;
            }
            depth--;
            if (depth == 0) {
                break;
            }
        } else if (token == FDT_PROP) {
            uint32_t len = be32(sp);
            uint32_t nameoff = be32(sp + 4);
            const uint8_t *val = sp + 8;
            const char *pname = (const char *)(strings + nameoff);
            sp += 8 + ((len + 3) & ~3u);
            if (depth == 1 && name_eq(pname, "#address-cells") && len == 4) {
                addr_cells = be32(val);
            } else if (depth == 1 && name_eq(pname, "#size-cells") &&
                       len == 4) {
                size_cells = be32(val);
            } else if (in_memory && name_eq(pname, "reg")) {
                uint32_t need = (addr_cells + size_cells) * 4;
                if (len >= need && addr_cells <= 2 && size_cells <= 2) {
                    *out_base = read_cells(val, addr_cells);
                    *out_size = read_cells(val + addr_cells * 4, size_cells);
                    return 0;
                }
            }
        } else if (token == FDT_NOP) {
            continue;
        } else {
            return -1; /* 壊れた木 */
        }
    }
    return -1;
}

uint32_t arm_fdt_total_size(uint64_t dtb_addr) {
    const uint8_t *dtb = (const uint8_t *)(uintptr_t)dtb_addr;
    if (dtb_addr == 0 || be32(dtb) != FDT_MAGIC) {
        return 0;
    }
    return be32(dtb + 4);
}
