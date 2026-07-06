#ifndef PUREUNIX_ELF_H
#define PUREUNIX_ELF_H

#include <pureunix/arch.h>
#include <pureunix/types.h>

/* Max argv entries (including argv[0]) elf_exec_argv() will place on a new
 * process's initial stack; matches the shell's own SHELL_MAX_ARGS. */
#define ELF_MAX_ARGS 16

/* Loads path into a brand new address space and runs it as a new child
 * task, blocking (via task_join) until it exits. Used by the shell.
 * Equivalent to elf_exec_argv(path, 1, (char *const[]){ (char *)path }). */
int elf_exec(const char *path);
/* Same as elf_exec(), but argc/argv (argv[0..argc-1], POSIX-style — argv[0]
 * is conventionally the program name) are copied onto the new process's
 * initial stack so its main(int argc, char *argv[]) sees real values
 * instead of uninitialized registers. See kernel/elf.c's build_argv_stack(). */
int elf_exec_argv(const char *path, int argc, char *const argv[]);
/* Replaces the calling task's own address space with path, in place — the
 * userspace-visible equivalent of POSIX execve(). Only returns (with a
 * negative error code) on failure; regs is the caller's own int $0x80 trap
 * frame, redirected to the new program's entry point on success. */
int elf_exec_current(interrupt_regs_t *regs, const char *path);
bool elf_is_valid(const uint8_t *image, size_t size);

#endif
