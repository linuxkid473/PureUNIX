#include <pureunix/arch.h>

/* Real x87/SSE enablement + a captured "freshly reset" FXSAVE template —
 * see include/pureunix/arch.h's own comments and task_t.fpu_state
 * (include/pureunix/task.h) for the full story. Before this file existed,
 * CR4.OSFXSR was never set, so any ring-3 SSE instruction (movaps, etc.)
 * took a #UD (invalid opcode) trap straight to a kernel panic — found via
 * the Qt QPA port's first real on-screen QPainter paint, which uses
 * libQt6Gui.a's SSE2-optimized raster blend routines.
 *
 * Deliberately oversized by 16 bytes and aligned at the point of use
 * (fpu_state_area()) rather than assuming task_t's own allocator aligns
 * it — kernel/heap.c's kmalloc()/kcalloc() only guarantee 8-byte
 * alignment (align8()), but FXSAVE/FXRSTOR's memory operand must be
 * 16-byte aligned or both instructions #GP fault. */
static uint8_t g_default_fpu_state[512 + 16];

void fpu_init(void)
{
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1u << 2); /* EM: 0 = real x87/SSE, not "trap and emulate" */
    cr0 |= (1u << 1);  /* MP: monitor coprocessor (paired with EM=0) */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    uint32_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1u << 9);  /* OSFXSR: FXSAVE/FXRSTOR + legal SSE in ring 3 */
    cr4 |= (1u << 10); /* OSXMMEXCPT: unmasked SIMD FP exceptions -> #XF, not #UD */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile("fninit");
    fpu_save(fpu_state_area(g_default_fpu_state));
}

void fpu_init_task_state(uint8_t *dst)
{
    const uint8_t *src = fpu_state_area(g_default_fpu_state);
    uint8_t *aligned_dst = fpu_state_area(dst);
    for (int i = 0; i < 512; i++) {
        aligned_dst[i] = src[i];
    }
}
