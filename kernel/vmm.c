#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define PAGE_PRESENT 0x001
#define PAGE_WRITE   0x002
#define PAGE_USER    0x004

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
