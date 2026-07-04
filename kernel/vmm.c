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

/* Identity-maps the 4MiB-aligned PD entries spanning [base, base+size), using
 * one of the spare fb_tables. Used for MMIO regions (e.g. the linear
 * framebuffer) that live far outside the low RAM identity map above. */
static void map_extra_region(phys_addr_t base, uint32_t size)
{
    if (size == 0) {
        return;
    }
    uint32_t region_base = ALIGN_DOWN(base, 0x400000U);
    uint32_t region_end = ALIGN_UP(base + size, 0x400000U);
    uint32_t next_table = 0;

    for (uint32_t addr = region_base; addr < region_end && next_table < FB_EXTRA_TABLES;
         addr += 0x400000U, ++next_table) {
        uint32_t pd_i = addr >> 22;
        if (page_directory[pd_i] & PAGE_PRESENT) {
            continue;
        }
        for (uint32_t i = 0; i < 1024; ++i) {
            fb_tables[next_table][i] = (addr + i * PUREUNIX_PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITE;
        }
        page_directory[pd_i] = (uint32_t)fb_tables[next_table] | PAGE_PRESENT | PAGE_WRITE;
    }
}

void vmm_init(phys_addr_t identity_extra_base, uint32_t identity_extra_size)
{
    memset(page_directory, 0, sizeof(page_directory));
    memset(identity_tables, 0, sizeof(identity_tables));
    memset(fb_tables, 0, sizeof(fb_tables));

    for (uint32_t table = 0; table < ARRAY_SIZE(identity_tables); ++table) {
        for (uint32_t i = 0; i < 1024; ++i) {
            uint32_t addr = (table * 1024 + i) * PUREUNIX_PAGE_SIZE;
            identity_tables[table][i] = addr | PAGE_PRESENT | PAGE_WRITE;
        }
        page_directory[table] = (uint32_t)identity_tables[table] | PAGE_PRESENT | PAGE_WRITE;
    }

    map_extra_region(identity_extra_base, identity_extra_size);

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
    uint32_t pd_i = USER_WINDOW_BASE >> 22;
    if (pd[pd_i] & PAGE_PRESENT) {
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
    uint32_t pd_i = USER_WINDOW_BASE >> 22;
    uint32_t *src_pd = (uint32_t *)src_pd_phys;
    if (!(src_pd[pd_i] & PAGE_PRESENT)) {
        return true;
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
         * which directory is currently loaded in CR3, since every physical
         * frame pmm_alloc_frame() hands out is < 128 MiB and thus covered
         * by the shared kernel PDEs present in every directory. */
        memcpy((void *)dst_frame, (void *)src_frame, PUREUNIX_PAGE_SIZE);
        virt_addr_t virt = (pd_i << 22) | (i << 12);
        vmm_map_page_in(dst_pd_phys, virt, dst_frame, (src_table[i] & 0xFFF) | PAGE_PRESENT);
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
