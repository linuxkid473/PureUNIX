#include <pureunix/elf.h>
#include <pureunix/errno.h>
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

/* Writes one NULL-terminated pointer-array block (argv- or envp-shaped) into
 * the page: the strings themselves, then the pointer array, working
 * downward from *off. Shared by build_argv_stack() for both the argv and
 * envp blocks below. count is capped at max_count; strings and the pointer
 * array (count+1 entries, NULL-terminated) are placed below the previous
 * value of *off. Returns the array's base virtual address via *out_va, or
 * -1 if it doesn't fit in the page. */
static int write_ptr_block(uint8_t *page, uint32_t top_page_va, uint32_t *off,
                            int count, int max_count, char *const items[], uint32_t *out_va)
{
    uint32_t str_va[ELF_MAX_ARGS > ELF_MAX_ENVP ? ELF_MAX_ARGS : ELF_MAX_ENVP];

    if (count < 0) {
        count = 0;
    }
    if (count > max_count) {
        count = max_count;
    }

    for (int i = count - 1; i >= 0; i--) {
        size_t len = strlen(items[i]) + 1;
        if (*off < len) {
            return -1;
        }
        *off -= (uint32_t)len;
        memcpy(page + *off, items[i], len);
        str_va[i] = top_page_va + *off;
    }

    *off &= ~(uint32_t)3; /* align the pointer array */

    uint32_t ptr_bytes = (uint32_t)(count + 1) * 4;
    if (*off < ptr_bytes) {
        return -1;
    }
    *off -= ptr_bytes;
    uint32_t base_va = top_page_va + *off;
    uint32_t *ptr = (uint32_t *)(page + *off);
    for (int i = 0; i < count; i++) {
        ptr[i] = str_va[i];
    }
    ptr[count] = 0;

    *out_va = base_va;
    return 0;
}

/* Counts a NULL-terminated pointer array's entries, capped at max_count
 * (envp has no explicit count the way argv/argc does — every caller passes
 * a NULL-terminated array instead). items may be NULL, meaning zero. */
static int count_ptr_array(char *const items[], int max_count)
{
    if (!items) {
        return 0;
    }
    int n = 0;
    while (n < max_count && items[n]) {
        n++;
    }
    return n;
}

/* Writes a POSIX-shaped argv+envp frame into the physical page backing the
 * top of the new stack (top_frame, identity-mapped so it's directly
 * writable from the kernel, and mapped into the child at virtual address
 * top_page_va): the argv strings+pointer array, then the envp
 * strings+pointer array, then a 3-word header (argc, argv_va, envp_va) —
 * mirroring what "push envp; push argv; push argc; call main" would leave
 * behind, so crt0's _start (see user/crt0.S, user/newlib_crt0.S) can hand
 * them straight to main(argc, argv) (and, for the newlib path, environ).
 * Returns the resulting initial ESP (somewhere within [top_page_va,
 * top_page_va + PUREUNIX_PAGE_SIZE)) via *out_esp, or -1 if argv/envp don't
 * fit in one page. */
static int build_argv_stack(phys_addr_t top_frame, uint32_t top_page_va, int argc,
                             char *const argv[], char *const envp[], uint32_t *out_esp)
{
    uint8_t *page = (uint8_t *)top_frame;
    uint32_t off = PUREUNIX_PAGE_SIZE;
    uint32_t argv_va, envp_va;

    if (argc < 0) {
        argc = 0;
    }
    if (argc > ELF_MAX_ARGS) {
        argc = ELF_MAX_ARGS;
    }

    if (write_ptr_block(page, top_page_va, &off, argc, ELF_MAX_ARGS, argv, &argv_va) != 0) {
        return -1;
    }

    int envc = count_ptr_array(envp, ELF_MAX_ENVP);
    if (write_ptr_block(page, top_page_va, &off, envc, ELF_MAX_ENVP, envp, &envp_va) != 0) {
        return -1;
    }

    if (off < 12) {
        return -1;
    }
    off -= 12;
    uint32_t *frame_hdr = (uint32_t *)(page + off);
    frame_hdr[0] = (uint32_t)argc;
    frame_hdr[1] = argv_va;
    frame_hdr[2] = envp_va;

    *out_esp = top_page_va + off;
    return 0;
}

/* Reads, validates, and loads path's PT_LOAD segments plus a fresh stack
 * into pd_phys (a page directory not necessarily currently active in CR3;
 * every write here goes through a freshly allocated frame's identity
 * mapping, never through pd_phys's own mapping — see vmm_map_page_in()).
 * On success, writes the entry point to *out_entry, the initial ESP (argc/
 * argv already in place at the top of the stack — see build_argv_stack())
 * to *out_stack, and returns 0; on failure, returns a negative value and
 * leaves pd_phys untouched by the caller's perspective (any pages already
 * mapped into it are the caller's responsibility to free via
 * vmm_free_user_directory()). */
static int elf_load_into(const char *raw_path, uint32_t pd_phys, int argc, char *const argv[],
                          char *const envp[], uint32_t *out_entry, uint32_t *out_stack)
{
    /* raw_path may be relative (a real user process's execve("./foo", ...)
     * or a bare/relative name from a fork()+exec() shell — the in-kernel
     * shell's own callers (shell/sh.c) already pass an absolute path, so
     * this is a no-op for them; resolving here too (rather than only in
     * arch/i386/syscall.c's SYS_EXEC case) covers both elf_exec_argv() and
     * elf_exec_current() callers in one place). */
    char path_buf[PUREUNIX_MAX_PATH];
    vfs_normalize(path_buf, task_current_cwd(), raw_path);
    const char *path = path_buf;

    /* Executing requires X_OK on the file itself — distinct from, and
     * checked in addition to, the R_OK that vfs_read_file() below enforces
     * to actually load the bytes off disk (this kernel has no separate
     * "read the file without going through the normal read gate" path, so
     * running a program still needs both bits set, as every ELF in /bin
     * in the test image does: rwxr-xr-x). */
    vfs_stat_t st;
    int src = vfs_stat(path, &st);
    if (src != 0) {
        printf("%s: not found\n", path);
        return src;
    }
    if (!vfs_access(&st, current_uid(), current_gid(), X_OK)) {
        printf("%s: permission denied\n", path);
        return -EACCES;
    }

    uint8_t *image = NULL;
    size_t size = 0;
    int rrc = vfs_read_file(path, &image, &size);
    if (rrc != 0) {
        printf("%s: not found\n", path);
        return rrc;
    }
    if (!elf_is_valid(image, size)) {
        printf("%s: not an i386 ELF executable\n", path);
        kfree(image);
        return -ENOEXEC;
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
            return -ENOEXEC;
        }

        uint32_t seg_start = ALIGN_DOWN(ph->p_vaddr, PUREUNIX_PAGE_SIZE);
        uint32_t seg_end = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PUREUNIX_PAGE_SIZE);
        for (uint32_t va = seg_start; va < seg_end; va += PUREUNIX_PAGE_SIZE) {
            phys_addr_t frame = pmm_alloc_frame();
            if (!frame) {
                printf("%s: out of memory\n", path);
                kfree(image);
                return -ENOMEM;
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

    uint32_t top_page_va = USER_WINDOW_END - PUREUNIX_PAGE_SIZE;
    phys_addr_t top_frame = 0;
    for (uint32_t va = USER_WINDOW_END - USER_STACK_SIZE; va < USER_WINDOW_END;
         va += PUREUNIX_PAGE_SIZE) {
        phys_addr_t frame = pmm_alloc_frame();
        if (!frame) {
            printf("%s: out of memory\n", path);
            return -ENOMEM;
        }
        memset((void *)frame, 0, PUREUNIX_PAGE_SIZE);
        vmm_map_page_in(pd_phys, va, frame, PAGE_USER | PAGE_WRITE);
        if (va == top_page_va) {
            top_frame = frame;
        }
    }

    uint32_t stack_top = USER_WINDOW_END;
    if (build_argv_stack(top_frame, top_page_va, argc, argv, envp, &stack_top) != 0) {
        printf("%s: argument list too long\n", path);
        return -E2BIG;
    }

    *out_entry = entry;
    *out_stack = stack_top;
    return 0;
}

int elf_exec_argv(const char *path, int argc, char *const argv[], char *const envp[])
{
    uint32_t pd_phys = vmm_create_user_directory();
    if (!pd_phys) {
        printf("%s: out of memory\n", path);
        return -ENOMEM;
    }

    uint32_t entry, stack_top;
    int rc = elf_load_into(path, pd_phys, argc, argv, envp, &entry, &stack_top);
    if (rc != 0) {
        vmm_free_user_directory(pd_phys);
        return rc;
    }

    task_t *child = task_create_user("user", entry, stack_top, pd_phys);
    if (!child) {
        vmm_free_user_directory(pd_phys);
        printf("%s: failed to create process\n", path);
        return -ENOMEM;
    }
    return task_join(child);
}

int elf_exec(const char *path)
{
    char *const argv[] = { (char *)path, NULL };
    return elf_exec_argv(path, 1, argv, NULL);
}

int elf_exec_current(interrupt_regs_t *regs, const char *path, int argc, char *const argv[],
                      char *const envp[])
{
    task_t *t = task_current();
    if (!t || !t->is_user) {
        return -EINVAL;
    }

    uint32_t new_pd = vmm_create_user_directory();
    if (!new_pd) {
        return -ENOMEM;
    }

    uint32_t entry, stack_top;
    int rc = elf_load_into(path, new_pd, argc, argv, envp, &entry, &stack_top);
    if (rc != 0) {
        vmm_free_user_directory(new_pd);
        return rc;
    }

    uint32_t old_pd = t->pd_phys;
    t->pd_phys = new_pd;
    t->user_entry = entry;
    t->user_stack = stack_top;
    vmm_switch_directory(new_pd);
    if (old_pd != vmm_kernel_directory_phys()) {
        vmm_free_user_directory(old_pd);
    }

    /* The generic int $0x80 return path (isr_common_stub) will iret using
     * these fields — redirect it into the freshly loaded program instead
     * of resuming the one just replaced. */
    regs->eip = entry;
    regs->useresp = stack_top;
    return 0;
}
