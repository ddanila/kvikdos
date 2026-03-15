# macOS Port — Software 8086 CPU Backend

## Goal

Add macOS support to kvikdos by implementing a software 8086 CPU interpreter
as an alternative to KVM. Linux remains the first-class platform (KVM, near-native
speed). macOS gets a portable software CPU that is correct enough to run the
MS-DOS 4.0 build tools (MASM, CL, LINK, LIB, etc.).

## Architecture

```
kvikdos.c (DOS emulation: INT handlers, file I/O, path translation, EXE/COM loader)
    |
    +-- #ifdef __linux__  -->  KVM backend (existing, unchanged)
    |                           /dev/kvm, ioctl KVM_RUN, hardware x86 execution
    |
    +-- #else             -->  cpu8086 backend (new)
                                cpu8086.c: software 8086 interpreter
                                Same kvm_regs/kvm_sregs/kvm_run structs
                                Same exit_reason protocol (KVM_EXIT_HLT, etc.)
```

### Why reuse `kvm_regs`/`kvm_sregs` structs

The DOS emulation code has 500+ references to `regs.rax`, `sregs.cs.selector`, etc.
Introducing new struct types would require rewriting all of them. Instead, the software
CPU reads/writes the same KVM structs directly. On macOS these are just plain C structs
defined in `mini_kvm.h` (no kernel dependency).

### How the interrupt mechanism works

kvikdos sets up an interrupt vector table at address 0x000 where each INT vector points
to segment `INT_HLT_PARA` (0x54), offset = `int_number`. At those addresses, there is
a HLT instruction. When DOS code executes `INT 21h`, the CPU:

1. Pushes FLAGS, CS, IP onto the stack
2. Loads CS:IP from IVT[0x21] = `0x0054:0x0021`
3. Executes HLT at that address

KVM reports this as `KVM_EXIT_HLT`. The software CPU does the same: executes the INT
microcode (push flags/CS/IP, load from IVT), then when it hits HLT, returns with
`exit_reason = KVM_EXIT_HLT`. The rest of kvikdos handles it identically.

## Implementation Steps

Each step is a separate commit. No step breaks the existing Linux/KVM functionality.

### Step 1: Make `mini_kvm.h` portable

Guard KVM ioctl constants (`KVM_RUN`, `KVM_CREATE_VM`, etc.) with `#ifdef __linux__`.
Keep struct definitions (`kvm_regs`, `kvm_sregs`, `kvm_run`, `kvm_segment`, etc.)
unconditional — they are plain C and needed by both backends.

**Test**: Header compiles on macOS with clang.

### Step 2: Guard KVM code with `#ifdef __linux__`

Wrap in `#ifdef __linux__`:
- `#include <linux/kvm.h>` (line 45)
- `open("/dev/kvm", ...)` and all ioctl setup in `reset_emu()` (lines 1852-1925)
- `KVM_SET_SREGS`/`KVM_SET_REGS` before the main loop (lines 2269-2274)
- `KVM_RUN` + `KVM_GET_REGS`/`KVM_GET_SREGS` in the loop (lines 2280-2292)

Add `#else` stubs that `fprintf(stderr, "KVM not available\n"); exit(252);`.

**Test**: Linux unchanged. macOS compiles, prints error at runtime.

### Step 3: macOS memory setup (no KVM regions)

In the `#else` branch of `reset_emu()`:
- `mmap()` for `mem` (handle `MAP_ANONYMOUS` vs `MAP_ANON`)
- Skip `KVM_SET_USER_MEMORY_REGION` calls
- Allocate a static `struct kvm_run` (not mmap'd from vCPU)
- Set `kvm_fds` to all -1 (the `get_linux_fd` security check naturally works)
- Handle `madvise(MADV_DONTNEED)` portability

**Test**: Initializes memory on macOS, prints success, exits.

### Step 4: Add `cpu8086_run()` skeleton

New files: `cpu8086.h` + `cpu8086.c`.

```c
/* Execute instructions until HLT, I/O, MMIO, or error.
 * Returns: sets run->exit_reason and returns 0, or -1 on fatal error. */
int cpu8086_run(struct kvm_regs *regs, struct kvm_sregs *sregs,
                struct kvm_run *run, void *mem, unsigned mem_size);
```

Implements only: `HLT` (0xF4), `NOP` (0x90), `INT n` (0xCD nn).
INT microcode: push FLAGS, push CS, push IP, load CS:IP from IVT, continue executing
(which immediately hits HLT in the interrupt table).

**Test**: Minimal .com that does `INT 21h/AH=4Ch` — the INT dispatches to HLT,
kvikdos handles exit, program terminates.

### Step 5: Wire `cpu8086_run()` into main loop

In the `#else` branch, replace KVM_RUN with `cpu8086_run()`.
The exit_reason switch stays identical. The `set_sregs_regs_and_continue` goto
needs no ioctl on the software path (regs/sregs are already the CPU state).

**Test**: A trivial .com program exits cleanly on macOS.

### Step 6: Core data movement instructions

- `MOV` r/m ↔ r/m, imm (0x88-0x8B, 0xA0-0xA3, 0xB0-0xBF, 0xC6-0xC7)
- `PUSH`/`POP` registers and segments (0x50-0x5F, 0x06/0E/16/1E/07/17/1F, 0xFF/6, 0x8F/0)
- `XCHG` (0x86-0x87, 0x90-0x97)
- `LEA` (0x8D), `LDS`/`LES` (0xC5/0xC4)
- Segment override prefixes (0x26/2E/36/3E)
- `CBW`/`CWD` (0x98/0x99)
- Segment register MOVs (0x8C/0x8E)

**This is the biggest single step** because it requires implementing the ModR/M byte
decoder with all 16-bit addressing modes: [BX+SI], [BX+DI], [BP+SI], [BP+DI],
[SI], [DI], [disp16], [BX], plus 8-bit and 16-bit displacements.

**Test**: `MOV AH, 4Ch` / `INT 21h` works. `guest.com` gets further.

### Step 7: Arithmetic and flags

- `ADD`/`ADC`/`SUB`/`SBB`/`CMP` (0x00-0x3D, 0x80-0x83 groups)
- `AND`/`OR`/`XOR`/`TEST`/`NOT`/`NEG` (various opcodes)
- `INC`/`DEC` (0x40-0x4F, 0xFE-0xFF groups)
- Flag manipulation: `STC`/`CLC`/`CMC`/`STI`/`CLI`/`CLD`/`STD`/`LAHF`/`SAHF`/`PUSHF`/`POPF`

Eager flag computation: compute CF, ZF, SF, OF, AF, PF after each operation,
store in `regs.rflags`. Simplest and most correct approach.

**Test**: Arithmetic + comparisons produce correct flag state.

### Step 8: Control flow

- `JMP` near/short/far (0xE9/0xEB/0xEA)
- `Jcc` all 16 conditional jumps (0x70-0x7F)
- `CALL` near/far (0xE8/0x9A, 0xFF/2, 0xFF/3)
- `RET`/`RETF` (0xC3/0xCB/0xC2/0xCA)
- `LOOP`/`LOOPE`/`LOOPNE`/`JCXZ` (0xE0-0xE3)
- `IRET` (0xCF)

**Test**: `guest.com` runs completely (uses LODSB, CMP, JE, JMP, RET, INT).

### Step 9: String operations

- `MOVSB`/`MOVSW` (0xA4-0xA5)
- `STOSB`/`STOSW` (0xAA-0xAB)
- `LODSB`/`LODSW` (0xAC-0xAD)
- `CMPSB`/`CMPSW` (0xA6-0xA7)
- `SCASB`/`SCASW` (0xAE-0xAF)
- `REP`/`REPE`/`REPNE` prefixes (0xF2/0xF3)

Heavily used by C runtime (memcpy, memset, strlen) and by MASM/LINK.

**Test**: `cat.com` works (reads stdin, writes stdout).

### Step 10: Multiply, divide, shifts, remaining

- `MUL`/`IMUL`/`DIV`/`IDIV` (0xF6-0xF7 groups)
- `SHL`/`SHR`/`SAR`/`ROL`/`ROR`/`RCL`/`RCR` (0xD0-0xD3, 0xC0-0xC1)
- `AAA`/`AAS`/`AAM`/`AAD`/`DAA`/`DAS` (BCD)
- `XLAT` (0xD7)
- `IN`/`OUT` (0xE4-0xE7, 0xEC-0xEF)
- `INT 3` (0xCC), `INTO` (0xCE)

**Test**: `malloct.com` / `mallocs.com` run correctly.

### Step 11: Integration test with real build tools

Run the actual MS-DOS 4.0 build toolchain under the software CPU:
MASM → CL → LINK → LIB. Fix missing or buggy instructions iteratively.
Compare output binaries to KVM-produced ones (should be byte-identical).

**Test**: `make` in the parent project produces identical binaries on macOS.

### Step 12: Makefile and build system

- Detect platform via `uname -s`
- Linux: build as before (KVM, single-file compile)
- macOS: compile `cpu8086.c` + `kvikdos.c`, link with `clang`
- Handle `MAP_ANONYMOUS`/`MAP_ANON`, `madvise` portability
- `make test` works on both platforms

**Test**: `make && make run` works on macOS.

## Risk areas

- **ModR/M decoder correctness** (step 6): all 256 addressing mode combinations.
  Use Intel 8086 manual or reference implementation (fake86, 8086tiny) to verify.
- **Flag computation** (step 7): AF, OF for SUB vs CMP edge cases.
  Test with known flag-dependent code paths in the build tools.
- **Segment wrapping**: real 8086 wraps at 1MB. Unlikely to matter for conventional
  memory programs, but needs handling for correctness.
- **Self-modifying code**: works naturally (both backends read from same `mem` buffer).

## References

- Intel 8086/8088 User's Manual (instruction encoding tables)
- [8086tiny](https://github.com/adriancable/8086tiny) — ~500 lines of core 8086 logic
- [fake86](https://github.com/rubbermallet/fake86) — more complete, ~2500 lines
- kvikdos `mini_kvm.h` — portable struct definitions
