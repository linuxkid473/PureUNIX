# Syscall Documentation

PureUnix uses software interrupt `int 0x80`.

Registers:

- `eax`: syscall number
- `ebx`, `ecx`, `edx`: arguments
- return value: `eax`

## Calls

| Number | Name | Arguments | Description |
| --- | --- | --- | --- |
| 1 | `SYS_EXIT` | `ebx = status` | Returns status to the current loader path. |
| 2 | `SYS_WRITE` | `ebx = fd`, `ecx = buffer`, `edx = length` | Writes to fd 1 or 2. |
| 3 | `SYS_READ` | `ebx = fd`, `ecx = buffer`, `edx = length` | Reads from fd 0. |
| 4 | `SYS_GETPID` | none | Returns current task id. |
| 5 | `SYS_YIELD` | none | Cooperative yield. |

## Current Model

ELF programs are loaded into a fixed low virtual address range and run in kernel privilege for the initial milestone. The syscall ABI is already separated so later work can move programs to ring 3 with per-process address spaces.

