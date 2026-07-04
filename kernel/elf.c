#include <pureunix/elf.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/vfs.h>
#include <pureunix/vmm.h>

#define EI_NIDENT 16
#define ET_EXEC 2
#define EM_386 3
#define PT_LOAD 1

/* Ring3 sandbox window: code+data+bss go in the low part, a fixed-size
 * stack (growing down from the top) takes the rest. USER_WINDOW_BASE/END/
 * USER_STACK_SIZE are defined in vmm.h — every process gets its own
 * private mapping of this same virtual range (see vmm_create_user_
 * directory()/vmm_map_page_in()). */
#define ELF_CODE_LIMIT   (USER_WINDOW_END - USER_STACK_SIZE)

typedef struct elf32_ehdr {
    uint8_t e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) elf32_phdr_t;

bool elf_is_valid(const uint8_t *image, size_t size)
{
    if (size < sizeof(elf32_ehdr_t)) {
        return false;
    }
    const elf32_ehdr_t *eh = (const elf32_ehdr_t *)image;
    return eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
           eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F' &&
           eh->e_ident[4] == 1 && eh->e_type == ET_EXEC && eh->e_machine == EM_386;
}

/* Reads, validates, and loads path's PT_LOAD segments plus a fresh stack
 * into pd_phys (a page directory not necessarily currently active in CR3;
 * every write here goes through a freshly allocated frame's identity
 * mapping, never through pd_phys's own mapping — see vmm_map_page_in()).
 * On success, writes the entry point to *out_entry and returns 0; on
 * failure, returns a negative value and leaves pd_phys untouched by the
 * caller's perspective (any pages already mapped into it are the caller's
 * responsibility to free via vmm_free_user_directory()). */
static int elf_load_into(const char *path, uint32_t pd_phys, uint32_t *out_entry)
{
    /* Executing requires X_OK on the file itself — distinct from, and
     * checked in addition to, the R_OK that vfs_read_file() below enforces
     * to actually load the bytes off disk (this kernel has no separate
     * "read the file without going through the normal read gate" path, so
     * running a program still needs both bits set, as every ELF in /bin
     * in the test image does: rwxr-xr-x). */
    vfs_stat_t st;
    if (vfs_stat(path, &st) != 0) {
        printf("%s: not found\n", path);
        return -1;
    }
    if (!vfs_access(&st, current_uid(), current_gid(), X_OK)) {
        printf("%s: permission denied\n", path);
        return -1;
    }

    uint8_t *image = NULL;
    size_t size = 0;
    if (vfs_read_file(path, &image, &size) != 0) {
        printf("%s: not found\n", path);
        return -1;
    }
    if (!elf_is_valid(image, size)) {
        printf("%s: not an i386 ELF executable\n", path);
        kfree(image);
        return -1;
    }

    elf32_ehdr_t *eh = (elf32_ehdr_t *)image;
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        elf32_phdr_t *ph = (elf32_phdr_t *)(image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_offset + ph->p_filesz > size || ph->p_vaddr < USER_WINDOW_BASE ||
            ph->p_vaddr + ph->p_memsz > ELF_CODE_LIMIT) {
            printf("%s: invalid segment\n", path);
            kfree(image);
            return -1;
        }

        uint32_t seg_start = ALIGN_DOWN(ph->p_vaddr, PUREUNIX_PAGE_SIZE);
        uint32_t seg_end = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PUREUNIX_PAGE_SIZE);
        for (uint32_t va = seg_start; va < seg_end; va += PUREUNIX_PAGE_SIZE) {
            phys_addr_t frame = pmm_alloc_frame();
            if (!frame) {
                printf("%s: out of memory\n", path);
                kfree(image);
                return -1;
            }
            memset((void *)frame, 0, PUREUNIX_PAGE_SIZE);

            /* Copy whatever slice of the segment's file-backed bytes
             * [p_vaddr, p_vaddr+p_filesz) falls within this page; anything
             * beyond p_filesz (bss) stays zeroed. */
            uint32_t file_start = ph->p_vaddr;
            uint32_t file_end = ph->p_vaddr + ph->p_filesz;
            uint32_t page_end = va + PUREUNIX_PAGE_SIZE;
            uint32_t copy_start = va > file_start ? va : file_start;
            uint32_t copy_end = page_end < file_end ? page_end : file_end;
            if (copy_start < copy_end) {
                memcpy((uint8_t *)frame + (copy_start - va),
                       image + ph->p_offset + (copy_start - file_start),
                       copy_end - copy_start);
            }

            vmm_map_page_in(pd_phys, va, frame, PAGE_USER | PAGE_WRITE);
        }
    }

    uint32_t entry = eh->e_entry;
    kfree(image);

    for (uint32_t va = USER_WINDOW_END - USER_STACK_SIZE; va < USER_WINDOW_END;
         va += PUREUNIX_PAGE_SIZE) {
        phys_addr_t frame = pmm_alloc_frame();
        if (!frame) {
            printf("%s: out of memory\n", path);
            return -1;
        }
        memset((void *)frame, 0, PUREUNIX_PAGE_SIZE);
        vmm_map_page_in(pd_phys, va, frame, PAGE_USER | PAGE_WRITE);
    }

    *out_entry = entry;
    return 0;
}

int elf_exec(const char *path)
{
    uint32_t pd_phys = vmm_create_user_directory();
    if (!pd_phys) {
        printf("%s: out of memory\n", path);
        return -1;
    }

    uint32_t entry;
    int rc = elf_load_into(path, pd_phys, &entry);
    if (rc != 0) {
        vmm_free_user_directory(pd_phys);
        return rc;
    }

    task_t *child = task_create_user("user", entry, USER_WINDOW_END, pd_phys);
    if (!child) {
        vmm_free_user_directory(pd_phys);
        printf("%s: failed to create process\n", path);
        return -1;
    }
    return task_join(child);
}

int elf_exec_current(interrupt_regs_t *regs, const char *path)
{
    task_t *t = task_current();
    if (!t || !t->is_user) {
        return -1;
    }

    uint32_t new_pd = vmm_create_user_directory();
    if (!new_pd) {
        return -1;
    }

    uint32_t entry;
    int rc = elf_load_into(path, new_pd, &entry);
    if (rc != 0) {
        vmm_free_user_directory(new_pd);
        return rc;
    }

    uint32_t old_pd = t->pd_phys;
    t->pd_phys = new_pd;
    t->user_entry = entry;
    t->user_stack = USER_WINDOW_END;
    vmm_switch_directory(new_pd);
    if (old_pd != vmm_kernel_directory_phys()) {
        vmm_free_user_directory(old_pd);
    }

    /* The generic int $0x80 return path (isr_common_stub) will iret using
     * these fields — redirect it into the freshly loaded program instead
     * of resuming the one just replaced. */
    regs->eip = entry;
    regs->useresp = USER_WINDOW_END;
    return 0;
}
