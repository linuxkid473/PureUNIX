#ifndef PUREUNIX_BOOTSPLASH_H
#define PUREUNIX_BOOTSPLASH_H

/* Draws the PureUNIX boot logo directly to VGA text memory, holds it for
 * 5 seconds, then clears the screen. Call right after vga_init(); it has
 * no dependency on the PIT/IDT/scheduler. */
void bootsplash_show(void);

#endif
