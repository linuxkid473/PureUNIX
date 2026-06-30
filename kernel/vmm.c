#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>

#define PAGE_PRESENT 0x001
#define PAGE_WRITE   0x002
#define PAGE_USER    0x004

static uint32_t page_directory[1024] __attribute__((aligned(4096)));
static uint32_t identity_tables[32][1024] __attribute__((aligned(4096)));

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

void vmm_init(void)
{
    memset(page_directory, 0, sizeof(page_directory));
    memset(identity_tables, 0, sizeof(identity_tables));

    for (uint32_t table = 0; table < ARRAY_SIZE(identity_tables); ++table) {
        for (uint32_t i = 0; i < 1024; ++i) {
            uint32_t addr = (table * 1024 + i) * PUREUNIX_PAGE_SIZE;
            identity_tables[table][i] = addr | PAGE_PRESENT | PAGE_WRITE;
        }
        page_directory[table] = (uint32_t)identity_tables[table] | PAGE_PRESENT | PAGE_WRITE;
    }

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
