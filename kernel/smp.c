/* PrismOS — SMP の実装。 */

#include "smp.h"

#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "speaker.h"
#include "string.h"

#define SMP_MAX_CPUS 16
#define LAPIC_ID 0x20
#define LAPIC_VERSION 0x30
#define LAPIC_EOI 0xB0
#define LAPIC_SVR 0xF0
#define LAPIC_ICR_LOW 0x300
#define LAPIC_ICR_HIGH 0x310

#define TRAMP_PHYS 0x8000ULL

extern const uint8_t ap_trampoline[];
extern const uint8_t ap_trampoline_end[];
extern uint64_t ap_tramp_cr3_slot;
extern uint64_t ap_tramp_stack_slot;
extern uint64_t ap_tramp_entry_slot;

static uint64_t smp_hhdm;
static volatile uint32_t *lapic;
static int cpu_count;
static uint8_t cpu_apic[SMP_MAX_CPUS];
static int cpu_bsp[SMP_MAX_CPUS];
static volatile uint32_t cpu_pings[SMP_MAX_CPUS];
/* 実作業分担のスロット。state: 0=空き 1=実行待ち/中
 * 2=完了 (BSP が読み取ったら 0 に戻す)。 */
static volatile smp_work_fn cpu_job_fn[SMP_MAX_CPUS];
static void *volatile cpu_job_arg[SMP_MAX_CPUS];
static volatile int cpu_job_state[SMP_MAX_CPUS];
static volatile uint32_t cpu_jobs_done[SMP_MAX_CPUS];
static volatile int ap_checkins;
static uint8_t bsp_apic_id;

static void dbg_hex8(uint8_t v) {
    const char *hex = "0123456789abcdef";
    char b[3];
    b[0] = hex[v >> 4];
    b[1] = hex[v & 0xF];
    b[2] = '\0';
    serial_write(b);
}

static uint32_t lapic_read(uint32_t off) {
    return lapic[off / 4];
}
static void lapic_write(uint32_t off, uint32_t v) {
    lapic[off / 4] = v;
}
static uint8_t my_apic_id(void) {
    return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
}

static int acpi_checksum(const uint8_t *p, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += p[i];
    }
    return (sum & 0xFF) == 0;
}

static uint64_t read_cr3_now(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v & 0x000FFFFFFFFFF000ULL;
}

static void wait_ms_approx(uint32_t ms) {
    /* PIT チャンネル 2 は 1193182 Hz。 */
    speaker_wait_ticks(1193182u / 1000 * ms);
}

static void spin_us_approx(uint32_t us) {
    /* 大まかなビジーウェイト (IPI の間隔用)。 */
    volatile uint32_t n = us * 30;
    while (n--) {
        __asm__ volatile("pause");
    }
}

/* AP の入口 (トランポリンから跳んでくる)。 */
static void ap_main(void) {
    gdt_load_existing();
    idt_load_existing();
    /* 自分の LAPIC を有効化する。 */
    lapic_write(LAPIC_SVR, 0x1FF);
    uint8_t id = my_apic_id();
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_apic[i] == id) {
            cpu_pings[i] = 0;
        }
    }
    __asm__ volatile("mfence" ::: "memory");
    ap_checkins++;
    serial_write("[smp] AP online, APIC id ");
    {
        char b[4];
        int n = 0;
        if (id >= 100) {
            b[n++] = (char)('0' + id / 100);
        }
        if (id >= 10) {
            b[n++] = (char)('0' + (id / 10) % 10);
        }
        b[n++] = (char)('0' + id % 10);
        b[n] = '\0';
        serial_write(b);
    }
    serial_write_line("");
    __asm__ volatile("sti");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void send_ipi(uint8_t dest, uint32_t low);

void smp_ipi_handler(void) {
    if (lapic == NULL) {
        return;
    }
    uint8_t id = my_apic_id();
    for (int i = 0; i < cpu_count; i++) {
        if (cpu_apic[i] == id) {
            cpu_pings[i]++;
            /* 投げられた仕事があればこの場で実行する。 */
            if (cpu_job_state[i] == 1) {
                smp_work_fn fn = cpu_job_fn[i];
                void *arg = cpu_job_arg[i];
                if (fn != NULL) {
                    fn(arg);
                }
                cpu_jobs_done[i]++;
                cpu_job_state[i] = 2;
            }
        }
    }
    lapic_write(LAPIC_EOI, 0);
}

int smp_post_work(int index, smp_work_fn fn, void *arg) {
    if (lapic == NULL || index < 0 || index >= cpu_count ||
        cpu_bsp[index] || fn == NULL) {
        return -1;
    }
    if (cpu_job_state[index] == 1) {
        return -1;
    }
    cpu_job_arg[index] = arg;
    cpu_job_fn[index] = fn;
    cpu_job_state[index] = 1;
    send_ipi(cpu_apic[index], 0x40);
    return 0;
}

int smp_work_finished(int index) {
    if (index < 0 || index >= cpu_count) {
        return 0;
    }
    if (cpu_job_state[index] == 2) {
        cpu_job_state[index] = 0;
        return 1;
    }
    return 0;
}

uint32_t smp_work_completed(int index) {
    return (index >= 0 && index < cpu_count) ? cpu_jobs_done[index]
                                             : 0;
}

void smp_hash_work(void *arg) {
    struct smp_hash_job *job = (struct smp_hash_job *)arg;
    uint64_t h = 1469598103934665603ULL;
    for (size_t k = 0; k < job->n; k++) {
        h ^= job->p[k];
        h *= 1099511628211ULL;
    }
    job->hash = h;
}

static void send_ipi(uint8_t dest, uint32_t low) {
    lapic_write(LAPIC_ICR_HIGH, (uint32_t)dest << 24);
    lapic_write(LAPIC_ICR_LOW, low);
    for (int i = 0; i < 100000; i++) {
        if ((lapic_read(LAPIC_ICR_LOW) & (1 << 12)) == 0) {
            break;
        }
        __asm__ volatile("pause");
    }
}

void smp_ping_all(void) {
    if (lapic == NULL) {
        return;
    }
    for (int i = 0; i < cpu_count; i++) {
        if (!cpu_bsp[i]) {
            send_ipi(cpu_apic[i], 0x40); /* ベクタ 0x40、固定 */
        }
    }
}

int smp_cpu_count(void) {
    return cpu_count;
}
uint8_t smp_apic_id(int index) {
    return (index >= 0 && index < cpu_count) ? cpu_apic[index] : 0;
}
int smp_is_bsp(int index) {
    return (index >= 0 && index < cpu_count) ? cpu_bsp[index] : 0;
}
uint32_t smp_ping_count(int index) {
    return (index >= 0 && index < cpu_count) ? cpu_pings[index] : 0;
}

/* ACPI テーブルは HHDM に載っていない領域 (BIOS 領域や
 * 回収可能メモリ) にあることがあるので、読む前に必ず
 * paging_map_mmio で載せる。 */
static const uint8_t *acpi_ptr(uint64_t phys, uint64_t len) {
    if (paging_map_mmio(phys & ~0xFFFULL,
                        len + (phys & 0xFFFULL),
                        smp_hhdm) != 0) {
        return NULL;
    }
    return (const uint8_t *)(uintptr_t)(phys + smp_hhdm);
}

int smp_init(uint64_t rsdp_addr, uint64_t hhdm_offset) {
    smp_hhdm = hhdm_offset;
    cpu_count = 1;
    cpu_bsp[0] = 1;
    if (rsdp_addr == 0) {
        serial_write_line("[smp] no RSDP; single CPU");
        return 1;
    }
    uint64_t rsdp_phys = rsdp_addr >= hhdm_offset
                             ? rsdp_addr - hhdm_offset
                             : rsdp_addr;
    const uint8_t *rsdp = acpi_ptr(rsdp_phys, 36);
    if (rsdp == NULL) {
        serial_write_line("[smp] RSDP map failed; single CPU");
        return 1;
    }
    if (memcmp(rsdp, "RSD PTR ", 8) != 0 ||
        !acpi_checksum(rsdp, 20)) {
        serial_write_line("[smp] RSDP invalid; single CPU");
        return 1;
    }
    uint8_t revision = rsdp[15];
    uint64_t sdt_phys;
    int xsdt = 0;
    if (revision >= 2) {
        uint64_t x = 0;
        for (int i = 0; i < 8; i++) {
            x |= (uint64_t)rsdp[24 + i] << (8 * i);
        }
        sdt_phys = x;
        xsdt = 1;
    } else {
        uint32_t r = 0;
        for (int i = 0; i < 4; i++) {
            r |= (uint32_t)rsdp[16 + i] << (8 * i);
        }
        sdt_phys = r;
    }
    const uint8_t *sdt = acpi_ptr(sdt_phys, 8);
    if (sdt == NULL) {
        serial_write_line("[smp] SDT map failed; single CPU");
        return 1;
    }
    uint32_t sdt_len = 0;
    for (int i = 0; i < 4; i++) {
        sdt_len |= (uint32_t)sdt[4 + i] << (8 * i);
    }
    sdt = acpi_ptr(sdt_phys, sdt_len);
    if (sdt == NULL) {
        serial_write_line("[smp] SDT map failed; single CPU");
        return 1;
    }
    if (sdt_len < 36 || !acpi_checksum(sdt, sdt_len)) {
        serial_write_line("[smp] SDT invalid; single CPU");
        return 1;
    }
    /* MADT ("APIC") を探す。 */
    const uint8_t *madt = NULL;
    size_t entry_size = xsdt ? 8 : 4;
    size_t count = (sdt_len - 36) / entry_size;
    for (size_t i = 0; i < count; i++) {
        uint64_t phys = 0;
        for (size_t b = 0; b < entry_size; b++) {
            phys |= (uint64_t)sdt[36 + i * entry_size + b]
                    << (8 * b);
        }
        const uint8_t *t = acpi_ptr(phys, 8);
        if (t != NULL && memcmp(t, "APIC", 4) == 0) {
            uint32_t tl = 0;
            for (int b = 0; b < 4; b++) {
                tl |= (uint32_t)t[4 + b] << (8 * b);
            }
            madt = acpi_ptr(phys, tl);
            break;
        }
    }
    if (madt == NULL) {
        serial_write_line("[smp] no MADT; single CPU");
        return 1;
    }
    uint32_t madt_len = 0;
    for (int i = 0; i < 4; i++) {
        madt_len |= (uint32_t)madt[4 + i] << (8 * i);
    }
    uint32_t lapic_phys = 0;
    for (int i = 0; i < 4; i++) {
        lapic_phys |= (uint32_t)madt[36 + i] << (8 * i);
    }
    /* エントリ走査: CPU 一覧と LAPIC アドレス上書き。 */
    int found = 0;
    uint32_t off = 44;
    while (off + 2 <= madt_len) {
        uint8_t type = madt[off];
        uint8_t len = madt[off + 1];
        if (len < 2) {
            break;
        }
        if (type == 0 && len >= 8) {
            uint32_t flags = 0;
            for (int i = 0; i < 4; i++) {
                flags |= (uint32_t)madt[off + 4 + i] << (8 * i);
            }
            if ((flags & 1) && found < SMP_MAX_CPUS) {
                cpu_apic[found] = madt[off + 3];
                cpu_bsp[found] = 0;
                found++;
            }
        } else if (type == 5 && len >= 12) {
            uint64_t a = 0;
            for (int i = 0; i < 8; i++) {
                a |= (uint64_t)madt[off + 4 + i] << (8 * i);
            }
            lapic_phys = (uint32_t)a;
        }
        off += len;
    }
    if (found == 0 || lapic_phys == 0) {
        serial_write_line("[smp] MADT has no usable CPU; single CPU");
        return 1;
    }
    if (paging_map_mmio(lapic_phys, 0x1000, hhdm_offset) != 0) {
        serial_write_line("[smp] LAPIC map failed; single CPU");
        return 1;
    }
    lapic = (volatile uint32_t *)(uintptr_t)(lapic_phys +
                                             hhdm_offset);
    lapic_write(LAPIC_SVR, 0x1FF);
    bsp_apic_id = my_apic_id();
    cpu_count = found;
    int bsp_index = -1;
    for (int i = 0; i < found; i++) {
        if (cpu_apic[i] == bsp_apic_id) {
            cpu_bsp[i] = 1;
            bsp_index = i;
        }
    }
    if (bsp_index < 0) {
        serial_write_line("[smp] BSP not in MADT list; single CPU");
        cpu_count = 1;
        return 1;
    }
    if (found == 1) {
        serial_write_line("[smp] 1 CPU (no AP to start)");
        return 1;
    }

    /* トランポリンを 0x8000 に置き、恒等マップする。 */
    if (paging_map_page_identity(TRAMP_PHYS, hhdm_offset) != 0) {
        serial_write_line("[smp] trampoline map failed; single CPU");
        cpu_count = 1;
        return 1;
    }
    uint8_t *tramp = (uint8_t *)(uintptr_t)(TRAMP_PHYS + hhdm_offset);
    size_t tramp_size =
        (size_t)(ap_trampoline_end - ap_trampoline);
    if (tramp_size > 0x1000) {
        serial_write_line("[smp] trampoline too big; single CPU");
        cpu_count = 1;
        return 1;
    }
    memcpy(tramp, ap_trampoline, tramp_size);
    /* スロットはコピー先 (物理ページ) へ書く。オフセットは
     * リンクされたシンボルの差分から求める。 */
    uint64_t *cr3_slot = (uint64_t *)(uintptr_t)(
        TRAMP_PHYS + hhdm_offset +
        (uint64_t)((const uint8_t *)&ap_tramp_cr3_slot -
                   ap_trampoline));
    uint64_t *stack_slot = (uint64_t *)(uintptr_t)(
        TRAMP_PHYS + hhdm_offset +
        (uint64_t)((const uint8_t *)&ap_tramp_stack_slot -
                   ap_trampoline));
    uint64_t *entry_slot = (uint64_t *)(uintptr_t)(
        TRAMP_PHYS + hhdm_offset +
        (uint64_t)((const uint8_t *)&ap_tramp_entry_slot -
                   ap_trampoline));
    *cr3_slot = read_cr3_now();
    *entry_slot = (uint64_t)(uintptr_t)ap_main;

    int online = 1;
    for (int i = 0; i < found; i++) {
        if (cpu_bsp[i]) {
            continue;
        }
        uint64_t stack_phys = pmm_alloc_pages(4);
        if (stack_phys == 0) {
            serial_write_line("[smp] no stack page for AP");
            continue;
        }
        *stack_slot = stack_phys + hhdm_offset + 4 * 4096;
        int before = ap_checkins;
        /* INIT はレベルトリガで assert/deassert する
         * (エッジ指定だと通常割り込みとして届いてしまう)。 */
        send_ipi(cpu_apic[i], 0xC500); /* INIT assert */
        wait_ms_approx(10);
        send_ipi(cpu_apic[i], 0x8500); /* INIT deassert */
        spin_us_approx(200);
        send_ipi(cpu_apic[i], 0x4600 | (TRAMP_PHYS >> 12)); /* SIPI */
        spin_us_approx(200);
        send_ipi(cpu_apic[i], 0x4600 | (TRAMP_PHYS >> 12)); /* SIPI */
        /* チェックインを最大約 1 秒待つ。 */
        int ok = 0;
        for (int w = 0; w < 100; w++) {
            if (ap_checkins > before) {
                ok = 1;
                break;
            }
            wait_ms_approx(10);
        }
        if (ok) {
            online++;
        } else {
            serial_write("[smp] APIC id ");
            {
                char b[4];
                int n = 0;
                uint8_t id = cpu_apic[i];
                if (id >= 100) {
                    b[n++] = (char)('0' + id / 100);
                }
                if (id >= 10) {
                    b[n++] = (char)('0' + (id / 10) % 10);
                }
                b[n++] = (char)('0' + id % 10);
                b[n] = '\0';
                serial_write(b);
            }
            serial_write_line(" did not check in");
        }
    }
    serial_write("[smp] ");
    {
        char b[4];
        int n = 0;
        int v = online;
        if (v >= 10) {
            b[n++] = (char)('0' + v / 10);
        }
        b[n++] = (char)('0' + v % 10);
        b[n] = '\0';
        serial_write(b);
    }
    serial_write_line(" CPU(s) online");
    (void)bsp_index;
    return online;
}
