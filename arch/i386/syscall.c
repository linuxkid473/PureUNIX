#include <pureunix/arch.h>
#include <pureunix/keyboard.h>
#include <pureunix/stdio.h>
#include <pureunix/syscall.h>
#include <pureunix/task.h>

void syscall_init(void)
{
}

uint32_t syscall_dispatch(interrupt_regs_t *regs)
{
    switch (regs->eax) {
    case SYS_EXIT:
        return regs->ebx;
    case SYS_WRITE: {
        int fd = (int)regs->ebx;
        const char *buf = (const char *)regs->ecx;
        size_t len = regs->edx;
        if (fd != 1 && fd != 2) {
            return (uint32_t)-1;
        }
        for (size_t i = 0; i < len; ++i) {
            putchar(buf[i]);
        }
        return len;
    }
    case SYS_READ: {
        int fd = (int)regs->ebx;
        char *buf = (char *)regs->ecx;
        size_t len = regs->edx;
        if (fd != 0) {
            return (uint32_t)-1;
        }
        size_t i = 0;
        while (i < len) {
            int key = keyboard_getkey();
            if (key == KEY_ENTER) {
                buf[i++] = '\n';
                break;
            }
            if (key > 0 && key < 128) {
                buf[i++] = (char)key;
            }
        }
        return i;
    }
    case SYS_GETPID:
        return task_current() ? task_current()->id : 0;
    case SYS_YIELD:
        task_yield();
        return 0;
    default:
        return (uint32_t)-1;
    }
}
