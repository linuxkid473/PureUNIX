#include <pureunix/elf.h>
#include <pureunix/memory.h>
#include <pureunix/stdio.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/vfs.h>

#define EI_NIDENT 16
#define ET_EXEC 2
#define EM_386 3
#define PT_LOAD 1

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

int elf_exec(const char *path)
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
        if (ph->p_offset + ph->p_filesz > size || ph->p_vaddr < 0x400000 || ph->p_vaddr + ph->p_memsz > 0x700000) {
            printf("%s: invalid segment\n", path);
            kfree(image);
            return -1;
        }
        memcpy((void *)ph->p_vaddr, image + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz) {
            memset((void *)(ph->p_vaddr + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
        }
    }

    int (*entry)(void) = (int (*)(void))eh->e_entry;
    int rc = entry();
    kfree(image);
    return rc;
}
