# Virtual Terminals

PureUnix supports 6 virtual terminals (VT1–VT6), Linux-console style: independent
screen buffers, cursors, scrollback, and shell sessions, switchable with
Alt+F1..Alt+F6 or the `tty` command, all running concurrently under the
scheduler.

**Sources**:
- `kernel/vt.c` / `include/pureunix/vt.h` — the VT subsystem itself (switching,
  per-VT termios, per-VT keyboard queues)
- `drivers/vga.c` / `include/pureunix/vga.h` — the underlying multi-console
  rendering driver `kernel/vt.c` is built on
- `drivers/tty.c` / `include/pureunix/tty.h` — per-VT termios-aware `read()`
- `user/tty.c` — the `tty` / `tty N` shell command

---

## Architecture

### Layering

```
kernel/main.c        boot: vt_init(), spawn one session task per VT
        |
kernel/vt.c           policy: which VT is active, per-VT termios,
        |              per-VT keyboard queues, vt_switch()
        v
drivers/vga.c         mechanism: N independent console_t buffers
                       (cell grid, cursor, ANSI parser, scrollback),
                       exactly one bound to real hardware at a time
```

`drivers/vga.c` has no notion of "VT1" through "VT6" — it just owns a small
pool of `console_t` objects (`VGA_MAX_CONSOLES`, currently 8, of which
`kernel/vt.c` claims `NUM_VTS` = 6) and knows how to render whichever one is
currently bound to real hardware (`vga_bind_active()`), while every other one
keeps its own buffer up to date without touching a single pixel
(`vga_putc_vt()`/`vga_write_len_vt()` — see `force_draw()`'s `cs != g_active`
gate). `kernel/vt.c` is the layer that actually decides what a VT *is*:
console + termios + keyboard queue + (via `task_t.vt_id`) a set of tasks.

### `console_t`: one independent screen

Each `console_t` (defined privately in `drivers/vga.c`) holds:

- `cell_char[MAX_ROWS][MAX_COLS]` / `cell_attr[MAX_ROWS][MAX_COLS]` — the text
  grid and its per-cell SGR attribute byte
- `row`, `col`, `color` — cursor position and current SGR color
- `esc_state`, `esc_buf`, `esc_len` — ANSI escape-sequence parser state (a
  background VT can be mid-escape-sequence when it's switched away from, so
  this has to be saved per console, not global)
- `scroll_top`, `scroll_bottom` — DECSTBM scroll region
- `cur_row`, `cur_col`, `cursor_valid` — hardware cursor overlay bookkeeping,
  meaningful only while this console is active
- `sb_char[SCROLLBACK_LINES][MAX_COLS]`, `sb_count`, `sb_next`, `sb_view` —
  scrollback ring (see below)

Only `use_fb`, `vga_cols`, `vga_rows`, `origin_x`, `origin_y` stay as
driver-wide globals in `drivers/vga.c` — they're properties of the one
physical display, not of any particular console.

**No N framebuffer copies**: every `console_t` is plain RAM (a few hundred
KiB total for all 6 VTs); the framebuffer itself only ever shows one of them.
Switching VTs is: rebind which `console_t` owns hardware
(`vga_bind_active()`), then one full repaint pass
(`vga_console_repaint()`) that blits *that* console's own `cell_char`/
`cell_attr` to the screen. A backgrounded console's writes
(`force_draw()`/`set_cell()`) still update its own `cell_char`/`cell_attr`
so that repaint always has correct, current content to draw from — they just
skip every pixel-level and hardware-cursor operation.

### Scrollback

`scroll()` (`drivers/vga.c`) pushes the line about to scroll off the top of
the *whole screen* (`scroll_top == 0 && scroll_bottom == vga_rows - 1` — a
custom DECSTBM region, e.g. Neatvi's status-bar-exclusion, is editor-internal
redraw noise, not real scrollback) into a per-console ring buffer,
`SCROLLBACK_LINES` (150) lines deep, text-only (no per-cell attributes, to
keep the per-VT footprint down). Shift+PageUp/PageDown
(`drivers/keyboard.c`/`drivers/hid.c` → `vt_scroll_view()` →
`vga_console_scroll_view()`) shifts the *displayed* rows into that history and
repaints; the live `cell_char` buffer is never touched by viewing. Any new
output on a scrolled-back console snaps the view back to live before drawing
(`vga_putc_vt()`'s `sb_view != 0` check) — same as a real terminal.

---

## VT Switching

`vt_switch(n)` (`kernel/vt.c`, 0-based: VT1 == 0) is the **one** kernel VT-switching
path:

```c
void vt_switch(int n)
{
    if (!valid_vt(n) || n == active_vt) return;
    active_vt = n;
    vga_bind_active(vts[n].console);
}
```

Two callers, no duplicated logic:

- **Alt+F1..Alt+F6**: `drivers/keyboard.c`'s `keyboard_irq()` recognizes Set-1
  scancodes `0x3B`–`0x40` (F1–F6, never `0xE0`-prefixed) with `alt_down` set,
  and calls `vt_switch(sc - 0x3B)` directly instead of producing a key event.
  `drivers/hid.c` does the same for USB HID Boot Protocol keyboards using
  usage IDs `0x3A`–`0x3F` and the modifier byte's Alt bits. A firmware that
  maps a laptop's bare F-keys to Fn+F<n> and requires Fn+Alt+F<n> for the
  "real" F-key needs nothing extra here: Fn is handled below the OS by the
  keyboard controller, so Fn+Alt+F<n> and Alt+F<n> arrive as the identical
  scancode/usage-ID pair either way.
- **`tty N`** (`user/tty.c`): `ioctl(fd, VT_ACTIVATE, &n)` → `SYS_IOCTL`
  (`arch/i386/syscall.c`) → `vt_switch(n - 1)`. Exactly the same function call
  Alt+F<n> makes.

Switching is immediate: `vga_bind_active()` runs a full repaint
synchronously before `vt_switch()` returns, so by the time the keystroke's
IRQ handler (or the `tty N` syscall) returns, the new VT is already fully
drawn.

---

## Keyboard Routing

`vt_input_push(key)` always targets `vts[active_vt]`'s own ring buffer — a
keystroke can only ever reach whichever VT is actually on screen. Each VT's
buffer is otherwise completely independent (`vt_input_try_getkey(vt_id)`/
`vt_input_getkey(vt_id)`), so a backgrounded VT's blocking read just blocks —
its queue is never fed until it becomes active again, exactly like a real
Linux VT. `drivers/keyboard.c`/`drivers/hid.c` are the only two producers
(replacing the old single-queue `drivers/input.c`, which this feature
retired in favor of `kernel/vt.c` owning `NUM_VTS` independent queues
directly).

---

## Task ↔ VT Binding

`task_t.vt_id` (`include/pureunix/task.h`) — 0-based, `-1` for a task that
predates `vt_init()` — names which VT a task's fd 0/1/2 default console
binding and controlling-terminal ioctls target. `task_alloc()`
(`kernel/task.c`) copies it from the creator at creation time, exactly like
uid/gid/cwd — so a shell's `vt_id`, set once by its session task (see below),
propagates automatically to every child it `fork()`s/`exec()`s: `ping`,
`seq`, `make` and everything `make` spawns all inherit the VT they were
launched from with no extra plumbing.

`arch/i386/syscall.c`'s `SYS_WRITE`/`SYS_READ` route fd 0/1/2's default
console binding through `vt_write(t->vt_id, ...)`/`tty_read(t->vt_id, ...)`
instead of a single global console — this is what makes a backgrounded VT's
process (e.g. `ping` left running on VT2 while VT1 is on screen) update only
its *own* saved console buffer, never bleed output onto whatever VT the user
actually has in front of them.

---

## `drivers/tty.c`: per-VT termios

Each VT has its own `struct termios` (`kernel/vt.c`'s `vt_t.termios`,
`vt_get_termios()`/`vt_set_termios()`) — `drivers/tty.c` resolves the calling
task's own VT (`task_t.vt_id`) on every `SYS_TCGETATTR`/`SYS_TCSETATTR`/
`tty_read()` call instead of touching one kernel-wide `struct termios` the
way it did before this feature (when PureUnix had exactly one terminal).
`tty_read(vt_id, buf, len)` takes an explicit `vt_id` — normally the
caller's own, but a `/dev/ttyN` descriptor opened for a *different* VT (see
below) passes that VT's id instead, so reading someone else's tty device
node blocks until that VT becomes active, same as real Linux.

---

## `/dev/tty1`–`/dev/tty6`, `/dev/tty`

`tools/mkext2.py`'s `add_dev()` stages six character-device inodes
(`S_IFCHR | 0666`) plus `/dev/tty` in the EXT2 image so the paths are real,
listable, `stat()`-able filesystem entries. PureUnix's VFS has no general
device-node/rdev dispatch mechanism yet, so these are intercepted **by path**
in `SYS_OPEN` (`arch/i386/syscall.c`'s `dev_tty_path_vt()`) before any of the
normal VFS-backed open logic runs — there's no on-disk content to read, just
`kernel/vt.c`'s own state. The resulting fd is `FD_KIND_TTY`
(`include/pureunix/task.h`), a third `open_file_t` kind alongside
`FD_KIND_FILE`/`FD_KIND_PIPE`, carrying just the target VT id
(`tty_vt_id`); `SYS_READ`/`SYS_WRITE`/`SYS_IOCTL`/`SYS_TCGETATTR`/
`SYS_TCSETATTR` all have an `FD_KIND_TTY` branch that routes straight to
`kernel/vt.c` instead of a file buffer. `open_file_unref()`
(`kernel/task.c`) does nothing extra for it on close — there's no buffer to
flush, matching real UNIX `close()` on a tty device node.

`/dev/ttyN` opens VT `N-1` directly; `/dev/tty` resolves to the *opening*
task's own `vt_id` at open time (POSIX's "controlling terminal" alias).

---

## `tty` / `tty N` (`user/tty.c`)

A small standalone program (not a BusyBox applet — `CONFIG_TTY` is off in
`third_party/busybox/pureunix.config`, avoiding any collision with this
custom, PureUnix-specific switching syntax real BusyBox `tty` doesn't have),
staged at `/bin/tty.elf` with a plain `/bin/tty` symlink
(`tools/mkext2.py`) so BusyBox ash's PATH lookup finds it as an ordinary
command.

- **`tty`** (no args): `ioctl(0, VT_GETACTIVE, &n)` and prints `/dev/ttyN`.
  This reports **which VT this process's own terminal is** (POSIX
  `ttyname()` semantics) — for an explicit `/dev/ttyN` fd it's the VT that
  fd was opened against; for the fd 0/1/2 default binding it's the calling
  task's own `vt_id`. Deliberately *not* "whichever VT is currently on
  screen" — those two questions have different answers once a process can
  be left running on a backgrounded VT, and a backgrounded VT's own `tty`
  must not report someone else's VT number.
- **`tty N`** (`N` in 1..6): `ioctl(0, VT_ACTIVATE, &n)` → `vt_switch(n-1)` —
  see "VT Switching" above. Same kernel path as Alt+F`N`, not a
  reimplementation.

`VT_GETACTIVE`/`VT_ACTIVATE` (`include/pureunix/ioctl.h`, values 3/4) are
regular `SYS_IOCTL` requests, gated by `tty_fd_check()` the same way
`TIOCGWINSZ`/`TIOCSFONT` are (extended to also accept an `FD_KIND_TTY` fd,
not just the fd 0/1/2 default binding).

---

## Boot Sequence

`kernel_main()` (`kernel/main.c`):

1. Normal hardware/filesystem bring-up, unchanged.
2. `vt_init()` right after `keyboard_init()`/`tty_init()`: VT1 (index 0)
   claims `drivers/vga.c`'s existing boot console directly (it already holds
   every boot message printed so far — see `docs/vt.md`'s `console_t`
   section on why this isn't a freshly reset one); VT2..VT6 get fresh
   `vga_console_reset()` ones. `task_current()->vt_id = 0` makes `main_task`
   itself VT1's session from this point on.
3. One interactive login (`users_login()`) on VT1 — the only VT actually
   visible at boot. This establishes the credentials
   (`task_set_creds()`, `kernel/users.c`) and env (`USER`/`HOME`/`SHELL`,
   `shell/builtins.c`) every VT's session shares. PureUnix has no
   multi-user, login-per-tty model yet, so VT2..VT6 start
   **pre-authenticated** as that same identity rather than each prompting
   its own login — six concurrent password prompts fighting over one
   physical keyboard before any of them are even visible would be worse,
   not more correct.
4. `task_create()` spawns one kernel-mode session task per VT2..VT6
   (`vt_session_main()`, entry point in `kernel/main.c`): the very first
   thing each does, once actually scheduled, is set its own `vt_id`
   (overriding the `0` it inherited from `main_task` at creation time — see
   "Task ↔ VT Binding" above), then loop calling `run_login_shell()`
   (launches `$SHELL`/`/bin/sh`/`/bin/puresh`, falling back to the
   in-kernel recovery shell as an absolute last resort — unchanged from
   before this feature).
5. `main_task` becomes VT1's own session: `for (;;) { run_login_shell();
   users_login(); }` — exiting VT1's shell re-prompts for login,
   real-getty style; VT2..VT6 just start another pre-authenticated session
   on exit, matching how they started.

---

## Concurrency: the scheduler fix this feature required

PureUnix's scheduler (`docs/scheduler.md`) is cooperative: nothing preempts
a running task, and before this feature, the two primitives a VT session
would spend nearly all its idle time in — `drivers/input.c`'s (now retired)
`input_getkey()` blocking-keyboard-wait loop, and `arch/i386/pit.c`'s
`pit_sleep()` — only ever called `arch_halt()`, never `task_yield()`. A task
blocked in either would hold the CPU indefinitely: every *other* task,
including a different VT's shell, would simply never run again until that
task itself yielded some other way. That made "a long-lived process on VT2
keeps running while VT1 is in use" structurally impossible, not just
untested.

Both are fixed to `task_yield()` before every `arch_halt()`
(`kernel/vt.c`'s `vt_input_getkey()`, which replaced `input_getkey()`, and
`pit_sleep()` itself) — the natural extension of this kernel's own
documented "cooperative round-robin, yield on blocking I/O" model, not a
move to preemptive scheduling. `SYS_NANOSLEEP` (`arch/i386/syscall.c`) also
gained the `arch_enable_interrupts()` call before `pit_sleep()` that
`SYS_PING` already had — without it, a process sleeping between iterations
(`ping`'s own loop) would deadlock outright (the PIT tick interrupt driving
`pit_sleep()`'s wait can never fire with interrupts still masked from
`int $0x80` entry), not just fail to yield.

A compute-bound task that never blocks on I/O, sleeps, or waits on a child
still monopolizes the CPU — that limitation is unchanged and is inherent to
cooperative scheduling, not specific to VTs. See `docs/scheduler.md`.

---

## Known Limitations

- **Single shared identity across all VTs**: no per-VT login; see "Boot
  Sequence" above.
- **`font` (`TIOCSFONT`) resizes the shared hardware grid, not each VT's
  saved buffer**: changing font scale recomputes `vga_cols`/`vga_rows` and
  repaints only the active console; a backgrounded VT's `cell_char` stays at
  its old dimensions until it's next written to or switched to, which can
  show stale content briefly at the new geometry.
- **The legacy in-kernel recovery shell** (`shell/sh.c`'s `shell_run()`,
  reached only if `elf_exec_argv()` can launch neither the configured shell
  nor `/bin/puresh` — normally never exercised) still renders through
  `drivers/vga.c`'s legacy no-arg API (`vga_write()` et al., always
  targeting whichever console is currently active) rather than being
  VT-routed; two concurrent fallback shells on different VTs would visually
  collide. `shell/sh.c`'s two real output call sites were moved to
  `vt_write(task_current()->vt_id, ...)`, so only genuinely nested
  legacy-shell-on-a-backgrounded-VT scenarios are affected.
- **Scrollback has no attributes**: history lines are plain text, rendered
  in the console's *current* color, not whatever color they were originally
  written in.
- **`VGA_MAX_CONSOLES` (8) vs. `NUM_VTS` (6)**: two spare `console_t` slots
  exist in the pool but nothing claims them yet — headroom for a future PTY
  or a second physical display, not currently reachable from anywhere.
