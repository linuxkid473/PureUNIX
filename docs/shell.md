# Shell

## Overview

**BusyBox ash (`/bin/sh`/`/bin/ash`) is the default login/interactive shell as of 2026-07.** `kernel/main.c`'s `run_login_shell()` execs the user's configured shell (`/etc/passwd`'s shell field, seeded `/bin/sh`) as a real ring-3 process via `elf_exec_argv()` right after a successful login, and blocks until it exits — exiting the shell (`exit`, Ctrl-D) returns you to a fresh login prompt, real-getty style. If the configured shell can't be exec'd at all, it falls back to `/bin/puresh` (not yet built as a real ELF — see "Login Shell Exec" below), then as an absolute last resort to the legacy in-kernel shell described in the rest of this document, so the system is never unusable.

The kernel-resident shell below (`shell/`) is what BusyBox ash *replaced* as the default — it's still fully built and functional, just no longer what a normal login session runs. It remains reachable only via `kernel_main()`'s last-resort fallback path, not as an ordinary command. It is implemented across four files in `shell/`:

| File | Purpose |
|---|---|
| `shell/sh.c` | Main loop, pipeline execution, external program dispatch, output routing |
| `shell/parser.c` | Tokenizer and pipeline parser |
| `shell/line.c` | Interactive line editor with history and tab completion |
| `shell/builtins.c` | All built-in command implementations, environment variable table |
| `shell/shell_internal.h` | Internal types and function declarations shared across shell files |

The public API (`include/pureunix/shell.h`):

```c
void shell_run(void);
int  shell_execute_line(const char *line);

/* Shared with kernel/users.c's login flow — see docs/users.md */
const char *shell_getenv(const char *key);
int  shell_setenv(const char *key, const char *value);
void shell_set_home_cwd(const char *home);
```

---

## Internal Types

### `shell_command_t`

```c
typedef struct shell_command {
    int  argc;
    char args[SHELL_MAX_ARGS][SHELL_MAX_ARG_LEN];   // 16 args × 96 bytes
    char *argv[SHELL_MAX_ARGS + 1];                  // NULL-terminated
    char input[PUREUNIX_MAX_PATH];                   // input redirection filename
    char output[PUREUNIX_MAX_PATH];                  // output redirection filename
    bool append;                                      // true if '>>'
} shell_command_t;
```

### `shell_pipeline_t`

```c
typedef struct shell_pipeline {
    int count;                                        // 1–4
    shell_command_t commands[SHELL_MAX_COMMANDS];     // SHELL_MAX_COMMANDS = 4
} shell_pipeline_t;
```

### `shell_output_t`

```c
typedef struct shell_output {
    char  *buffer;     // NULL if terminal=true
    size_t capacity;   // SHELL_OUTPUT_CAP = 16384
    size_t length;
    bool   terminal;   // if true, writes go directly to vga_write_len
} shell_output_t;
```

### `shell_context_t`

```c
typedef struct shell_context {
    char cwd[PUREUNIX_MAX_PATH];    // current working directory, starts "/"
} shell_context_t;
```

---

## Main Loop

`shell_run()` displays a prompt (format: `cwd $ `) and calls `shell_readline` to read a line. It then calls `shell_execute_line` and loops forever. As of the BusyBox-ash migration this only runs at all as `kernel_main()`'s absolute-last-resort fallback (see "Login Shell Exec" below) — it never returns either way, matching the fallback's need for *something* to keep the system usable if every real shell is missing.

---

## Login Shell Exec

`kernel/main.c`'s `run_login_shell()`/`try_login_shell()` (not part of `shell/` — this runs before/instead of the in-kernel shell) is the real entry point for an interactive session:

1. Reads `$SHELL` (set by `kernel/users.c`'s `users_login()` from `/etc/passwd`'s shell field, seeded `/bin/sh`).
2. `try_login_shell(path)`: `vfs_stat()`s `path` first — only if it actually exists does it call `elf_exec_argv(path, 1, argv, shell_build_envp())` and report "launched" back to the caller. This existence gate is what distinguishes "couldn't start this shell at all" from "started fine and exited" — `elf_exec_argv()`'s own return value is the *child's raw exit code* (including negative "killed by signal N" codes, see `SYS_KILL`'s doc in `docs/syscalls.md`), which is ambiguous with a launch failure if inspected directly.
3. If the configured shell couldn't be launched, tries `/bin/puresh` next (a real userspace port of the shell below this section — **not yet built**; the try silently fails today, exactly like any other missing file, and falls through to step 4). Porting the in-kernel shell's parser/builtins/line-editor to a real syscall-based ELF (rather than the direct kernel-internal `vfs_*`/`task_*` calls it uses today) is tracked as follow-up work — see project memory.
4. If that also fails, prints a warning and calls the legacy `shell_run()` (this file's "Main Loop" above) as an absolute last resort.
5. Either way, once the launched shell process exits, `kernel_main()`'s own loop calls `users_login()` again — exiting your shell logs you out back to a login prompt, matching real getty behavior. (The one exception is the `shell_run()` fallback branch, which never returns at all, matching its own pre-existing infinite-loop design.)

`shell_build_envp()` (`shell/builtins.c`, declared in the public `include/pureunix/shell.h`) supplies the new process's `envp` — the same "KEY=VALUE" dump of the kernel's env table used by any other program the in-kernel shell launches, so the login shell starts with the just-logged-in user's `USER`/`HOME`/`SHELL`/`PATH`.

---

## Parser

`shell_parse(line, pipeline, err)` tokenizes `line` into a `shell_pipeline_t`.

### Tokenizer

`next_token` reads the next token from `*cursor`:
- Whitespace is skipped.
- Metacharacters (`|`, `<`, `>`) are single-character tokens; `>>` is a two-character token.
- Quoted strings (`'...'` or `"..."`) produce a token with the quotes stripped. No escape processing.
- Other characters accumulate into a word until whitespace or a metacharacter.

### Pipeline Assembly

- `|` advances to the next command slot (up to `SHELL_MAX_COMMANDS` = 4).
- `<` sets `cmd->input` from the following token.
- `>` sets `cmd->output` from the following token.
- `>>` sets `cmd->output` and `cmd->append = true`.
- All other tokens are appended to `cmd->argv` (up to `SHELL_MAX_ARGS` = 16).

---

## Pipeline Execution

`shell_execute_line(line)`:

1. Parses the line into a `shell_pipeline_t`.
2. Allocates two 16 KiB static buffers (`stage_a`, `stage_b`) for inter-stage data.
3. For each command in the pipeline:
   - If input redirection (`cmd->input` is set), reads the file via VFS into a heap buffer.
   - Runs the command with `run_command(ctx, cmd, input, &out)`, where `out` is a `shell_output_t` backed by the current stage buffer.
   - The output buffer from this stage becomes the `input` for the next.
4. For the final command, **only if it was a builtin** (`shell_find_builtin()` still finds it — checked again in this same loop, since `run_command()`'s own check isn't visible here):
   - If output redirection is set, writes `out.buffer` to the VFS file (append or truncate).
   - Otherwise, writes `out.buffer` to the VGA console via `vga_write_len`.

An **external** program's output never goes through `out.buffer` at all — a real ELF program's own `write(1, ...)` calls go straight through its own file descriptor table, which `shell_output_t` knows nothing about (`out.buffer` stays empty for it, always). Its normal, unredirected output already reaches the console directly (routed by `SYS_WRITE` — see `docs/syscalls.md`), which is why this never looked broken for the ordinary case. Output *redirection* for an external program instead happens inside `exec_external()` itself (see "Command Resolution" below), by giving the launched program's fd 1 a real redirected binding *before* it starts — this only became possible once `SYS_DUP2`/shared file descriptions existed (see `docs/syscalls.md`'s "File descriptors are now shared, refcounted open file descriptions"); before that, `cmd->output` on an external command silently produced an empty file while the real output leaked to the console.

The two stage buffers alternate (`stage_a`, `stage_b`, `stage_a`, ...) so that no extra allocation is needed for up to 4 pipe stages. This buffer-relay mechanism only actually carries data between **builtin** pipeline stages — an external program's real output was never captured into it to relay to a next stage either, so `cmd1 | cmd2` where either side is an external ELF program doesn't work yet (needs real `SYS_PIPE`-based stage wiring, not just the redirect fix above).

---

## Command Resolution

`run_command(ctx, cmd, input, out)`:

1. Searches the built-in table with `shell_find_builtin(cmd->argv[0])`.
2. If found, calls the builtin function.
3. Otherwise, calls `exec_external`.

`exec_external`:
- If `cmd->output` is set, calls `shell_redirect_stdout()` first — resolves the target path, creates it if needed, and installs a real `open_file_t` onto the shell's own fd 1 (there's no separate task for the interactive shell; it runs as `task_current()`, the same task `kernel_main()` calls into). `task_create_user()` (`kernel/task.c`) now shares fds with its creator exactly like `task_fork()` does, so the launched program inherits that redirected fd 1 the same way a real shell's `fork()`+`dup2()`+`exec()` sequence would give it one. `shell_restore_stdout()` puts fd 1 back to the console binding afterward — which is also what flushes the write, since the shell's own reference was the last one once the launched program's own copy closed at its exit (`kernel/task.c`'s `close_all_fds()`, run when a task is reaped).
- Path resolution itself (`exec_external_inner`) is unchanged: if `argv[0]` contains `/`, resolves it against `cwd` via `vfs_normalize` and calls `elf_exec_argv(path, cmd->argc, cmd->argv, envp)`.
- Special case: `calculator` maps to `/bin/calc.elf`.
- Otherwise, tries `/bin/NAME.elf`, then `/bin/NAME`.
- If none found, prints "command not found".

Every case passes the shell's own already-parsed `cmd->argc`/`cmd->argv` straight through, so the program's `main(int argc, char *argv[])` sees the real command line (see `docs/userland.md`'s "Passing argv").

---

## Line Editor

`shell_readline(prompt, out, max)` provides an interactive line editor:

### Features

| Key | Action |
|---|---|
| Enter | Submit line, add to history |
| Ctrl-C | Clear line, return empty string |
| Backspace | Delete character before cursor |
| Delete | Delete character at cursor |
| Left/Right | Move cursor one character |
| Home | Move cursor to start of line |
| End | Move cursor to end of line |
| Up/Down | Navigate history |
| Tab | Attempt tab completion |
| Printable chars | Insert at cursor position |

Cursor movement and line editing use in-place redraw: `\r\033[K` clears the line, then the prompt and line contents are reprinted, and backspaces are used to reposition the terminal cursor.

### History

Up to 64 lines are stored in a static `history[64][256]` array. Consecutive duplicate lines are suppressed. History wraps (oldest entry is dropped) when the buffer is full. Up/Down arrows navigate the history buffer; Down at the end of history restores an empty line.

### Tab Completion

Completion triggers when Tab is pressed at the end of the line. The current token (from last space to cursor) is used as a prefix:

1. All built-in names are checked for the prefix match.
2. If no builtin matches, all entries in `/` (root directory) are checked.
3. If exactly one match is found, the remaining characters are appended.
4. Multiple matches: no action (no menu display).

**Limitation**: only the root directory is searched for file completions. Path-qualified tokens and PATH-based search are not implemented.

---

## Built-in Commands

All 29 builtins share the signature:
```c
typedef int (*builtin_fn_t)(shell_context_t *ctx, shell_command_t *cmd,
                            const char *input, shell_output_t *out);
```

All output is written via `shell_out_printf`/`shell_out_puts`, which routes to the `shell_output_t` buffer or directly to VGA.

| Command | Description |
|---|---|
| `ls [PATH]` | List directory contents (type, name, size) |
| `cd [DIR]` | Change current working directory |
| `pwd` | Print current working directory |
| `mkdir DIR` | Create directory |
| `rmdir DIR` | Remove empty directory |
| `rm FILE` | Remove file |
| `cp SRC DST` | Copy file (read then write) |
| `mv SRC DST` | Move/rename (tries `vfs_rename`; falls back to copy+delete) |
| `cat [FILE ...]` | Concatenate files or pass stdin through |
| `echo [ARG ...]` | Print arguments; `$VAR` expands environment variables |
| `touch FILE` | Create empty file (or no-op if it exists) |
| `clear` | Clear the VGA screen |
| `history` | Print command history |
| `date` | Read and display CMOS RTC time (UTC assumed) |
| `uname` | Print `PureUnix pureunix 0.1.0 i686` |
| `whoami` | Print value of `USER` environment variable |
| `mount` | Show mounted filesystem info |
| `df` | Show filesystem space usage |
| `free` | Show physical memory and heap usage |
| `ps` | List tasks (PID, state, name) |
| `kill PID` | Mark task as zombie |
| `reboot` | Reboot via keyboard controller |
| `shutdown` | Power off via ACPI/QEMU ports |
| `help` | List all built-ins with descriptions |
| `env` | Print all environment variables |
| `export KEY=VALUE` | Set or create environment variable |
| `vim FILE` / `vi FILE` | Open file in PureUNIX's own small in-kernel modal editor (`editor/editor.c`) |
| `adduser NAME` | Create a new account (root only); see docs/users.md |
| `passwd [NAME]` | Change a password (root can change any user's, others only their own) |

Not a builtin — an ordinary `/bin/neatvi.elf` external program, found the same way any other program is (see "Command Resolution" below): `neatvi [FILE]`, a vendored port of [Neatvi](https://github.com/aligrudi/neatvi) (see `docs/userland.md`'s "user/vi/ (neatvi)"). Deliberately a separate command from `vi`/`vim` above rather than replacing them, so both editors stay available.

### `date` Implementation

`date` reads the CMOS RTC registers directly:

| Register | Field |
|---|---|
| `0x00` | Seconds |
| `0x02` | Minutes |
| `0x04` | Hours |
| `0x07` | Day of month |
| `0x08` | Month |
| `0x09` | Year (2-digit) |
| `0x0B` | Status B (bit 2: binary mode vs BCD) |

Values are BCD-decoded unless status register B bit 2 is set. Year is displayed as `20XX`.

### `echo` Variable Expansion

`echo_expanded` checks if an argument starts with `$`. If so, it looks up the remainder in the environment table via `shell_getenv`. Only top-level arguments beginning with `$` are expanded; no nested or inline expansion.

---

## Environment Variables

A static array of 32 `env_var_t` entries (key up to 31 chars, value up to 95 chars). Pre-populated defaults:

| Key | Default Value |
|---|---|
| `USER` | `root` |
| `HOME` | `/` |
| `PATH` | `/bin` |
| `SHELL` | `sh` |

`shell_getenv(key)` returns the value string or `""` if not found.  
`shell_setenv(key, value)` updates an existing entry or creates a new one (up to 32 total).

`export` lets the user set variables directly; `kernel/users.c`'s login flow overwrites `USER`/`HOME`/`SHELL` with the values from `/etc/passwd` once a login succeeds (see docs/users.md). There is no inheritance between shells.
