#ifndef PUREUNIX_E1000_H
#define PUREUNIX_E1000_H

#include <pureunix/types.h>

/* Looks for an Intel e1000-family NIC on the PCI bus (see drivers/pci.c)
 * and, if found, enables bus mastering, maps its MMIO BAR, resets it,
 * reads its MAC address, and programs RX/TX descriptor rings. Safe to call
 * even when no such device is present; e1000_present() reports the
 * outcome. No-op (but safe to call again) if already initialized. */
void e1000_init(void);

bool e1000_present(void);

/* Copies the 6-byte MAC address into `mac`. Leaves `mac` untouched if
 * !e1000_present(). */
void e1000_get_mac(uint8_t mac[6]);

/* Queues `len` bytes (a full Ethernet frame, header included -- this driver
 * does not build headers) for transmission. Busy-polls the chosen
 * descriptor's own completion bit first, so back-to-back sends never
 * overwrite a frame still in flight. Returns 0 on success, -1 if the device
 * isn't present, the frame is larger than a single descriptor's buffer, or
 * the wait for a free descriptor timed out. */
int e1000_send(const void *data, uint16_t len);

/* Non-blocking: checks the next RX descriptor for a completed frame. If one
 * is ready, copies up to `buf_len` bytes into `buf`, recycles the
 * descriptor back to the NIC, and returns the frame's real length.  Returns
 * 0 if no frame is currently available, -1 if the device isn't present or
 * the frame didn't fit in `buf_len` (the descriptor is still recycled in
 * that case, dropping the frame). Call this in a loop to poll, or from
 * inside/after the RX interrupt once it has fired. */
int e1000_receive(void *buf, uint16_t buf_len);

/* Registers a callback invoked from inside the RX interrupt handler
 * whenever the cause includes RXT0 (frames are ready) -- the callback is
 * expected to drain the RX ring itself via e1000_receive() in a loop (see
 * net/eth.c's eth_init()/eth_dispatch()). Only one handler at a time;
 * registering a new one replaces the previous. */
void e1000_set_rx_handler(void (*handler)(void));

/* Diagnostic: sends a broadcast ARP probe and polls (up to ~1s of real
 * time, via pit_sleep()) for a reply, exercising the full TX and RX
 * datapath on real (emulated) hardware. No-op if !e1000_present(). Must be
 * called after arch_enable_interrupts() -- see kernel/main.c. */
void e1000_selftest(void);

/* Diagnostic: prints running TX/RX/error/interrupt counters plus a live
 * register snapshot (ring head/tail, ICR, IMS, STATUS). No-op (just a
 * message) if !e1000_present(). Reading ICR here clears its pending cause
 * bits, same as e1000_irq() -- diagnostic-only, safe to call anytime but
 * not free of side effects. */
void e1000_dump_stats(void);

#endif
