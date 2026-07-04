#ifndef PUREUNIX_ELF_H
#define PUREUNIX_ELF_H

#include <pureunix/arch.h>
#include <pureunix/types.h>

/* Loads path into a brand new address space and runs it as a new child
 * task, blocking (via task_join) until it exits. Used by the shell. */
int elf_exec(const char *path);
/* Replaces the calling task's own address space with path, in place — the
 * userspace-visible equivalent of POSIX execve(). Only returns (with a
 * negative error code) on failure; regs is the caller's own int $0x80 trap
 * frame, redirected to the new program's entry point on success. */
int elf_exec_current(interrupt_regs_t *regs, const char *path);
bool elf_is_valid(const uint8_t *image, size_t size);

#endif
