#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/vmm.h>

#define FB_EXTRA_TABLES 4

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t identity_tables[32][1024] __attribute__((aligned(4096)));
static uint32_t fb_tables[FB_EXTRA_TABLES][1024] __attribute__((aligned(4096)));

static void load_page_directory(uint32_t *pd)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd));
}

static void enable_paging(void)
{
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000U;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

static bool cpu_has_pat(void)
{
    uint32_t edx;
    __asm__ volatile("cpuid" : "=d"(edx) : "a"(1) : "ebx", "ecx");
    return (edx & (1U << 16)) != 0;
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* On real hardware the GPU's linear-framebuffer BAR is MMIO: firmware leaves
 * its MTRR type at the reset default, Uncacheable, and every store to it is
 * therefore a separate, strongly-ordered, blocking round trip over the PCI
 * bus (tens to hundreds of cycles each, with zero write buffering). This
 * driver writes one pixel — sometimes one *byte* — at a time (see
 * fb_put_pixel()/draw_cell_ex() in drivers/{framebuffer,vga}.c), so a single
 * redrawn line can be tens of thousands of individually-serialized bus
 * transactions: that's what makes real hardware visibly paint scrollback
 * line by line. QEMU's virtual display is backed by an ordinary host RAM
 * region, not a real PCI bus, so it never reproduces this penalty — which is
 * exactly why the slowdown was invisible in every QEMU test run.
 *
 * The fix is Write-Combining (WC): reprogram PAT entry 4 (index
 * PAT=1,PCD=0,PWT=0 — left otherwise untouched so entries 0-3 keep their
 * PCD/PWT-only reset semantics for anything else that relies on them) from
 * its default Write-Back to WC, then tag the framebuffer's own PTEs with the
 * PAT bit (see PAGE_PAT, applied in vmm_map_framebuffer_wc() below) to
 * select that entry. WC lets the CPU buffer consecutive stores into full
 * cache-line bursts instead of draining each one individually — the same
 * technique every mainstream OS's framebuffer console (Linux vesafb/fbcon,
 * Windows, etc.) uses for exactly this reason. No-op (leaves the default,
 * safe write-back-but-actually-UC-via-MTRR behavior) on CPUs without PAT. */
static bool pat_wc_available;

static void enable_pat_write_combining(void)
{
    if (!cpu_has_pat()) {
        pat_wc_available = false;
        return;
    }
    uint64_t pat = rdmsr(0x277);
    pat = (pat & ~(0xFFULL << 32)) | (0x01ULL << 32);
    wrmsr(0x277, pat);
    pat_wc_available = true;
}

/* Identity-maps [base, base+size) 1:1, page by page, as Write-Combining,
 * using the spare fb_tables reserved above — the only place in this kernel
 * that ever sets PAGE_PAT. See vmm_map_framebuffer_wc()'s declaration in
 * include/pureunix/memory.h for the full contract and vmm_map_mmio_uc()
 * below for the counterpart PCI MMIO devices must use instead.
 *
 * This used to blanket-fill every page across the whole 4MiB-aligned PD
 * chunk(s) covering [base, base+size) with PRESENT+WC, not just the pages
 * that were actually framebuffer bytes — harmless when the chunk padding
 * really was unused address space, but real hardware routinely packs PCI
 * BARs back-to-back in the same MMIO aperture, so on the HP Pavilion the
 * xHCI controller's BAR landed inside that same 4MiB-aligned padding region
 * right after the framebuffer. That pre-marked every xHCI register page
 * PRESENT+WC before xhci_init() ever ran. vmm_map_page() (the old map_bar()
 * path) does fully overwrite a PTE it's given, so xHCI's own explicitly-
 * mapped pages ended up back at PRESENT+WRITE — but with no PAGE_PCD/PWT
 * either, that left them at PAT entry 0 (Write-Back by the PAT reset
 * default), relying entirely on the BIOS's MTRR for that range to still
 * force UC. On QEMU's software-emulated MMIO that ambiguity is invisible
 * (every access "just works" regardless of memory type); on real silicon it
 * was enough to break the strict, immediately-visible, in-order writes the
 * doorbell register and ERDP (event ring dequeue pointer) need — see
 * xhci_barrier() in drivers/xhci.c, which only issues a *compiler* barrier
 * because it assumes true hardware UC, not whatever a race with this
 * function's blanket fill happened to leave behind.
 *
 * Now only pages inside [base, base+size) are ever marked PRESENT here, so
 * a neighboring BAR in the same 4MiB chunk is left completely unmapped
 * until its own driver calls vmm_map_mmio_uc() for it, with no window where
 * it could inherit WC (or anything else) from this function. */
void vmm_map_framebuffer_wc(phys_addr_t base, uint32_t size)
{
    if (size == 0) {
        return;
    }
    uint32_t region_base = ALIGN_DOWN(base, 0x400000U);
    uint32_t region_end = ALIGN_UP(base + size, 0x400000U);
    uint32_t next_table = 0;
    uint32_t extra_flags = pat_wc_available ? PAGE_PAT : 0;

    for (uint32_t chunk = region_base; chunk < region_end && next_table < FB_EXTRA_TABLES;
         chunk += 0x400000U, ++next_table) {
        uint32_t pd_i = chunk >> 22;
        if (page_directory[pd_i] & PAGE_PRESENT) {
            continue;
        }
        for (uint32_t i = 0; i < 1024; ++i) {
            uint32_t page_addr = chunk + i * PUREUNIX_PAGE_SIZE;
            if (page_addr >= base && page_addr < base + size) {
                fb_tables[next_table][i] = page_addr | PAGE_PRESENT | PAGE_WRITE | extra_flags;
            }
        }
        page_directory[pd_i] = (uint32_t)fb_tables[next_table] | PAGE_PRESENT | PAGE_WRITE;
    }
}

/* Counterpart to vmm_map_framebuffer_wc() above, for PCI device MMIO
 * registers (xHCI, e1000, ...): forces PAGE_PCD|PAGE_PWT (PAGE_PAT left
 * clear), selecting PAT entry 3 — UC, and per the Intel SDM's PAT/MTRR
 * combining rules that specific selection is UC regardless of what the
 * BIOS's MTRRs say for that physical range, unlike relying on MTRRs alone.
 * Every PCI BAR mapping in this kernel (drivers/xhci.c's map_bar(),
 * drivers/e1000.c's equivalent) must go through this, never a bare
 * vmm_map_page(), specifically so none of them can ever end up sharing a
 * memory type with vmm_map_framebuffer_wc()'s WC mapping by accident again. */
void vmm_map_mmio_uc(virt_addr_t virt, phys_addr_t phys, uint32_t flags)
{
    uint32_t safe_flags = (flags & (PAGE_WRITE | PAGE_USER)) | PAGE_PCD | PAGE_PWT;
    vmm_map_page(virt, phys, safe_flags);
}

void vmm_init(phys_addr_t identity_extra_base, uint32_t identity_extra_size)
{
    memset(page_directory, 0, sizeof(page_directory));
    memset(identity_tables, 0, sizeof(identity_tables));
    memset(fb_tables, 0, sizeof(fb_tables));

    enable_pat_write_combining();

    for (uint32_t table = 0; table < ARRAY_SIZE(identity_tables); ++table) {
        for (uint32_t i = 0; i < 1024; ++i) {
            uint32_t addr = (table * 1024 + i) * PUREUNIX_PAGE_SIZE;
            identity_tables[table][i] = addr | PAGE_PRESENT | PAGE_WRITE;
        }
        page_directory[table] = (uint32_t)identity_tables[table] | PAGE_PRESENT | PAGE_WRITE;
    }

    vmm_map_framebuffer_wc(identity_extra_base, identity_extra_size);

    load_page_directory(page_directory);
    enable_paging();
    printf("VMM: identity mapped %u MiB and enabled paging\n", (uint32_t)(ARRAY_SIZE(identity_tables) * 4));
}

void vmm_map_page(virt_addr_t virt, phys_addr_t phys, uint32_t flags)
{
    uint32_t pd_i = virt >> 22;
    uint32_t pt_i = (virt >> 12) & 0x3FF;
    uint32_t *table;
    if (!(page_directory[pd_i] & PAGE_PRESENT)) {
        phys_addr_t frame = pmm_alloc_frame();
        table = (uint32_t *)frame;
        memset(table, 0, PUREUNIX_PAGE_SIZE);
        page_directory[pd_i] = frame | PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);
    } else {
        table = (uint32_t *)(page_directory[pd_i] & ~0xFFF);
        /* Widening an already-present PDE (e.g. the identity map) to
         * PAGE_USER only relaxes the directory-level gate; each page's own
         * PTE.user bit below still decides whether that specific page is
         * actually reachable from ring 3. */
        page_directory[pd_i] |= (flags & PAGE_USER);
    }
    table[pt_i] = (phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

phys_addr_t vmm_get_mapping(virt_addr_t virt)
{
    uint32_t pd_i = virt >> 22;
    uint32_t pt_i = (virt >> 12) & 0x3FF;
    if (!(page_directory[pd_i] & PAGE_PRESENT)) {
        return 0;
    }
    uint32_t *table = (uint32_t *)(page_directory[pd_i] & ~0xFFF);
    if (!(table[pt_i] & PAGE_PRESENT)) {
        return 0;
    }
    return (table[pt_i] & ~0xFFF) | (virt & 0xFFF);
}

uint32_t vmm_create_user_directory(void)
{
    phys_addr_t pd_frame = pmm_alloc_frame();
    if (!pd_frame) {
        return 0;
    }
    /* Share every kernel PDE (identity-mapped low RAM, kernel image, heap,
     * framebuffer) by reference — only pd[USER_WINDOW_BASE>>22] differs per
     * process, and it starts absent here, populated lazily by
     * vmm_map_page_in(). */
    memcpy((void *)pd_frame, page_directory, sizeof(page_directory));
    return pd_frame;
}

void vmm_free_user_directory(uint32_t pd_phys)
{
    if (!pd_phys) {
        return;
    }
    uint32_t *pd = (uint32_t *)pd_phys;
    /* The user window now spans multiple PDEs (see vmm.h's "Virtual address
     * space layout" comment) -- every one of them is private per-process
     * (unlike pd[0..31], shared by reference with the kernel's own
     * page_directory), so every present one here needs its own table and
     * frames freed, not just a single hardcoded index. */
    uint32_t first_pd_i = USER_WINDOW_BASE >> 22;
    uint32_t last_pd_i  = (USER_WINDOW_END - 1) >> 22;
    for (uint32_t pd_i = first_pd_i; pd_i <= last_pd_i; ++pd_i) {
        if (!(pd[pd_i] & PAGE_PRESENT)) {
            continue;
        }
        uint32_t *table = (uint32_t *)(pd[pd_i] & ~0xFFF);
        for (uint32_t i = 0; i < 1024; ++i) {
            if (table[i] & PAGE_PRESENT) {
                pmm_free_frame(table[i] & ~0xFFF);
            }
        }
        pmm_free_frame((phys_addr_t)table);
    }
    pmm_free_frame(pd_phys);
}

void vmm_map_page_in(uint32_t pd_phys, virt_addr_t virt, phys_addr_t phys, uint32_t flags)
{
    uint32_t *pd = (uint32_t *)pd_phys;
    uint32_t pd_i = virt >> 22;
    uint32_t pt_i = (virt >> 12) & 0x3FF;
    uint32_t *table;
    if (!(pd[pd_i] & PAGE_PRESENT)) {
        phys_addr_t frame = pmm_alloc_frame();
        table = (uint32_t *)frame;
        memset(table, 0, PUREUNIX_PAGE_SIZE);
        pd[pd_i] = frame | PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);
    } else {
        table = (uint32_t *)(pd[pd_i] & ~0xFFF);
        pd[pd_i] |= (flags & PAGE_USER);
    }
    table[pt_i] = (phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;

    /* Only flush if pd_phys is the directory currently loaded in CR3 — a
     * directory being built for a not-yet-scheduled process shouldn't pay
     * for (or rely on) a TLB flush of the currently active address space. */
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 == pd_phys) {
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
}

bool vmm_fork_address_space(uint32_t dst_pd_phys, uint32_t src_pd_phys)
{
    uint32_t *src_pd = (uint32_t *)src_pd_phys;
    /* See vmm_free_user_directory()'s comment: the user window now spans
     * multiple PDEs, so every one of them needs its own deep-copy pass. */
    uint32_t first_pd_i = USER_WINDOW_BASE >> 22;
    uint32_t last_pd_i  = (USER_WINDOW_END - 1) >> 22;
    for (uint32_t pd_i = first_pd_i; pd_i <= last_pd_i; ++pd_i) {
        if (!(src_pd[pd_i] & PAGE_PRESENT)) {
            continue;
        }
        uint32_t *src_table = (uint32_t *)(src_pd[pd_i] & ~0xFFF);
        for (uint32_t i = 0; i < 1024; ++i) {
            if (!(src_table[i] & PAGE_PRESENT)) {
                continue;
            }
            phys_addr_t src_frame = src_table[i] & ~0xFFF;
            phys_addr_t dst_frame = pmm_alloc_frame();
            if (!dst_frame) {
                return false;
            }
            /* Both frames are always identity-accessible here regardless of
             * which directory is currently loaded in CR3, since every
             * physical frame pmm_alloc_frame() hands out is < 128 MiB and
             * thus covered by the shared kernel PDEs present in every
             * directory. */
            memcpy((void *)dst_frame, (void *)src_frame, PUREUNIX_PAGE_SIZE);
            virt_addr_t virt = (pd_i << 22) | (i << 12);
            vmm_map_page_in(dst_pd_phys, virt, dst_frame, (src_table[i] & 0xFFF) | PAGE_PRESENT);
        }
    }
    return true;
}

void vmm_switch_directory(uint32_t pd_phys)
{
    load_page_directory((uint32_t *)pd_phys);
}

void vmm_switch_directory_kernel(void)
{
    load_page_directory(page_directory);
}

uint32_t vmm_kernel_directory_phys(void)
{
    return (uint32_t)page_directory;
}
