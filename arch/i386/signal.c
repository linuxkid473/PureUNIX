/* arch/i386/signal.c — the register/memory mechanics of invoking a real
 * ring-3 signal handler: the shared trampoline page, redirecting a
 * process's own trap frame into it, and restoring that frame afterward
 * (sigreturn). Policy (default actions, SIG_IGN, deciding *whether* a
 * handler needs to run) lives in kernel/signal.c — this file only ever
 * deals with mechanism. See docs/process-management.md. */
#include <pureunix/arch.h>
#include <pureunix/memory.h>
#include <pureunix/signal.h>
#include <pureunix/string.h>
#include <pureunix/task.h>
#include <pureunix/vmm.h>

/* pop %eax          ; eax = handler address
 * call *%eax        ; handler(sig) -- sig is already the next value on
 *                    ; the stack, exactly where cdecl expects a callee's
 *                    ; first argument once `call` pushes its return addr
 * add $4, %esp       ; pop the sig argument (cdecl: caller cleans up)
 * int $0x82          ; sigreturn -- restores the pre-signal trap frame
 *                    ; wholesale (kernel/task.c's saved_regs_before_
 *                    ; signal, not anything on this stack) and never
 *                    ; returns here
 * jmp .              ; unreachable safety net */
static const uint8_t trampoline_code[] = {
    0x58,
    0xFF, 0xD0,
    0x83, 0xC4, 0x04,
    0xCD, 0x82,
    0xEB, 0xFE,
};

void signal_map_trampoline(uint32_t pd_phys)
{
    phys_addr_t frame = pmm_alloc_frame();
    if (!frame) {
        /* Out of memory this early in exec() is already fatal to the
         * caller (elf_load_into() checks for this same failure mode on
         * every other allocation) — leaving signals entirely unavailable
         * for this one process (pending_signals will just never be able
         * to redirect into a trampoline that isn't mapped) is a safer
         * degradation than panicking the whole kernel over it. */
        return;
    }
    memset((void *)frame, 0, PUREUNIX_PAGE_SIZE);
    memcpy((void *)frame, trampoline_code, sizeof(trampoline_code));
    /* Read + exec, deliberately never PAGE_WRITE — this is code a ring-3
     * task runs, never data it should be able to modify. */
    vmm_map_page_in(pd_phys, SIGNAL_TRAMPOLINE_VA, frame, PAGE_USER);
}

/* Lowest set bit in (pending & ~blocked), or 0 if none. Simple, fully
 * deterministic delivery order — POSIX doesn't mandate any particular
 * order among simultaneously-pending signals. */
static int lowest_deliverable_signal(task_t *t)
{
    uint32_t deliverable = t->pending_signals & ~t->blocked_signals;
    if (!deliverable) {
        return 0;
    }
    for (int sig = 1; sig < 32; ++sig) {
        if (deliverable & (1u << sig)) {
            return sig;
        }
    }
    return 0;
}

void signal_deliver_pending(interrupt_regs_t *regs)
{
    task_t *t = task_current();
    if (!t || t->active_signal != 0) {
        /* Already one signal frame deep — deliberately not reentrant
         * (see include/pureunix/task.h's active_signal comment). The
         * still-pending signal stays queued and is picked up on this
         * same task's next ring3-return boundary after its current
         * handler sigreturns. */
        return;
    }
    int sig = lowest_deliverable_signal(t);
    if (sig == 0) {
        return;
    }
    uint32_t handler = t->sig_handlers[sig];
    if (handler == PU_SIG_DFL || handler == PU_SIG_IGN) {
        /* kernel/signal.c's signal_send() only ever sets pending_signals
         * for a real handler — this should be unreachable, but clear the
         * bit defensively rather than loop forever re-checking it. */
        t->pending_signals &= ~(1u << sig);
        return;
    }

    t->pending_signals &= ~(1u << sig);
    t->active_signal = sig;
    t->saved_regs_before_signal = *regs;
    t->saved_blocked_before_signal = t->blocked_signals;
    /* POSIX: a signal is blocked against itself while its own handler
     * runs (absent SA_NODEFER, which this kernel doesn't support — see
     * docs/process-management.md's scope notes). */
    t->blocked_signals |= (1u << sig);

    uint32_t new_esp = regs->useresp;
    new_esp -= 4;
    *(uint32_t *)new_esp = (uint32_t)sig;
    new_esp -= 4;
    *(uint32_t *)new_esp = handler;

    regs->useresp = new_esp;
    regs->eip = SIGNAL_TRAMPOLINE_VA;
}

void signal_handle_sigreturn(interrupt_regs_t *regs)
{
    task_t *t = task_current();
    if (!t || t->active_signal == 0) {
        /* int $0x82 reached with no signal in flight -- not part of the
         * public syscall ABI, so there's no legitimate way for this to
         * happen short of a bug; ignore rather than trust attacker-
         * controlled state. */
        return;
    }
    *regs = t->saved_regs_before_signal;
    t->blocked_signals = t->saved_blocked_before_signal;
    t->active_signal = 0;
}
