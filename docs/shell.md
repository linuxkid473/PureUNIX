# Shell

## Overview

The PureUnix shell runs entirely inside the kernel. It is implemented across four files in `shell/`:

| File | Purpose |
|---|---|
| `shell/sh.c` | Main loop, pipeline execution, external program dispatch, output routing |
| `shell/parser.c` | Tokenizer and pipeline parser |
| `shell/line.c` | Interactive line editor with history and tab completion |
| `shell/builtins.c` | All built-in command implementations, environment variable table |
| `shell/shell_internal.h` | Internal types and function declarations shared across shell files |

The public API (`include/pureunix/shell.h`) exposes only two functions:

```c
void shell_run(void);
int  shell_execute_line(const char *line);
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

`shell_run()` displays a prompt (format: `cwd $ `) and calls `shell_readline` to read a line. It then calls `shell_execute_line` and loops forever.

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
4. For the final command:
   - If output redirection is set, writes `out.buffer` to the VFS file (append or truncate).
   - Otherwise, writes `out.buffer` to the VGA console via `vga_write_len`.

The two stage buffers alternate (`stage_a`, `stage_b`, `stage_a`, ...) so that no extra allocation is needed for up to 4 pipe stages.

---

## Command Resolution

`run_command(ctx, cmd, input, out)`:

1. Searches the built-in table with `shell_find_builtin(cmd->argv[0])`.
2. If found, calls the builtin function.
3. Otherwise, calls `exec_external`.

`exec_external`:
- If `argv[0]` contains `/`, resolves it against `cwd` via `vfs_normalize` and calls `elf_exec`.
- Special case: `calculator` maps to `/bin/calc.elf`.
- Otherwise, tries `/bin/NAME.elf`, then `/bin/NAME`.
- If none found, prints "command not found".

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

All 27 builtins share the signature:
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
| `vim FILE` / `vi FILE` | Open file in the vim-like editor |

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

`export` is the only way for the user to set variables. There is no inheritance between shells.
