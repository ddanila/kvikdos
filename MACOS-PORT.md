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

### Step 6: Integrate XTulator CPU core

Replace the hand-written opcode switch in `cpu8086.c` with XTulator's CPU core.

**Substeps:**
1. Copy `XTulator/XTulator/cpu/cpu.c`, `cpu.h`, `cpuconf.h` into kvikdos (or reference
   from submodule path).
2. Remove `#include "i8259.h"` from `cpu.h`. Stub/remove `cpu_interruptCheck()` — kvikdos
   handles interrupts via IVT → HLT → userspace dispatch, not hardware PIC.
3. Implement `cpu_read`/`cpu_write`/`cpu_readw`/`cpu_writew` as direct access to kvikdos's
   flat memory buffer (with `KVM_EXIT_MMIO` for addresses >= `mem_size`).
4. Implement `port_read`/`port_write`/`port_readw`/`port_writew` to trigger `KVM_EXIT_IO`
   return (stop execution, fill `kvm_run->io`).
5. Modify `cpu_exec()` to return on HLT/IO/MMIO instead of running a fixed loop count.
   Change from `void cpu_exec(CPU_t*, uint32_t)` to returning an exit reason.
6. Write register sync: `kvm_regs`/`kvm_sregs` → `CPU_t` at entry, `CPU_t` → `kvm_regs`/
   `kvm_sregs` at exit. Use `makeflagsword`/`decodeflagsword` for flags conversion.
7. Set `cpuconf.h` to `CPU_8086` mode (strip 80186+ if not needed).

**Test**: `guest.com` runs completely (INT 21h dispatch, string output, exit).

### Step 7: Fix up and test basic programs

With the full XTulator instruction set now wired in, run the existing test programs:
- `guest.com` — string output, program args, exit
- `cat.com` — stdin/stdout I/O
- `malloct.com` / `mallocs.com` — memory allocation
- `waitkey.com` — keyboard input
- `printenv.com` — environment access

Fix any integration bugs (memory access edge cases, I/O port handling, flag sync issues).

**Test**: All .com test programs produce identical output to Linux/KVM.

### Step 8: Integration test with real build tools

Run the actual MS-DOS 4.0 build toolchain under the software CPU:
MASM → CL → LINK → LIB. Fix missing or buggy instructions iteratively.
Compare output binaries to KVM-produced ones (should be byte-identical).

**Test**: `make` in the parent project produces identical binaries on macOS.

### Step 9: Build system and CI

- Makefile already detects platform via `uname -s` (done in step 5)
- Ensure XTulator CPU files are compiled on non-Linux
- Handle any remaining portability issues (`MAP_ANONYMOUS`/`MAP_ANON`, `madvise`)
- Add macOS to CI if desired
- `make test` works on both platforms

**Test**: `make && make run` works on macOS.

## XTulator as CPU core

We use the CPU core from [XTulator](https://github.com/mikechambers84/XTulator) (GPLv2,
forked to [ddanila/XTulator](https://github.com/ddanila/XTulator)) as the foundation for
the software CPU backend. XTulator is a portable 80186 PC emulator by Mike Chambers
(same author as fake86). Its CPU core is ~3,300 lines in 2 files (`cpu.c` + `cpu.h`) with
very clean separation from peripherals.

### Why XTulator over alternatives
- **Cleanest extraction**: CPU core is 2 self-contained files with only 6 external
  function dependencies (`cpu_read`, `cpu_write`, `port_read`, `port_write`, `cpu_readw`,
  `cpu_writew` + `i8259_nextintr` for interrupt checks).
- **No global state**: All functions take `CPU_t*` as first argument.
- **Full 8086 coverage**: All 256 primary opcodes + 80186 extensions (PUSHA, POPA, ENTER,
  LEAVE, PUSH imm, IMUL imm, BOUND, INS, OUTS).
- **Battle-tested**: Same CPU core lineage as fake86.
- **Small**: ~3,300 lines total vs. libx86emu's ~13,100 lines.
- **GPLv2**: Compatible with kvikdos's GPL >=2.0.

### What we use from XTulator
- `cpu_exec()` main decode/execute loop (the big 256-case switch)
- `CPU_t` struct with register/flag state
- ModR/M decoder with all 16-bit addressing modes
- All arithmetic/logic with correct flag computation (individual flag bytes +
  `makeflagsword`/`decodeflagsword` macros)
- String operations with REP/REPNE prefixes
- MUL/DIV, shifts/rotates, BCD instructions
- `cpu_intcall()` INT microcode

### What we strip/adapt
- **Memory access**: Replace `cpu_read`/`cpu_write` with direct access to kvikdos's
  flat memory buffer. For addresses >= `mem_size`, return `KVM_EXIT_MMIO`.
- **Port I/O**: Replace `port_read`/`port_write` with `KVM_EXIT_IO` returns, letting
  kvikdos handle port I/O the same way as the KVM backend.
- **Interrupt controller**: Remove `i8259_nextintr()` dependency. kvikdos doesn't use
  hardware interrupt injection; all INTs go through IVT → HLT → userspace dispatch.
- **Register sync**: Keep `CPU_t` internally; copy between `CPU_t` and
  `kvm_regs`/`kvm_sregs` at each `cpu8086_run()` entry/exit boundary.
  Individual flag bytes ↔ packed `rflags` via existing `makeflagsword`/`decodeflagsword`.
- **Main loop exit**: Modify `cpu_exec()` to return on HLT (for INT dispatch),
  I/O port access, or MMIO, instead of running a fixed loop count.
- **Remove includes**: Strip `i8259.h`, `config.h`, `debuglog.h` dependencies.
- **CPU variant**: Set `cpuconf.h` to `CPU_8086` or `CPU_80186` mode.

### Alternatives considered
- **8086tiny** (MIT): Complete 8086 in ~760 lines, but tightly coupled to BIOS binary
  lookup tables and memory-mapped register model. Harder to extract cleanly.
- **libx86emu** (BSD): Very mature, embeddable API, but 5x larger (~13K lines) and its
  page-table memory model adds overhead we don't need.
- **fake86** (GPLv2): Same author as XTulator, but uses global state (no CPU_t* param)
  and has hardcoded VGA memory ranges in cpu.c. XTulator is the cleaner evolution.
- **i8086emu** (TheFox): MIT, but written in PHP — not usable in a C project.
- **Blink** (ISC): x86-64 focused, massive codebase (~7K stars), overkill for 8086.

## Risk areas

- **XTulator exec loop modification** (step 6): changing `cpu_exec()` from a fixed-loop
  void function to one that returns on HLT/IO/MMIO. The main risk is missing an exit
  point or breaking the prefix handling (segment overrides, REP).
- **Register sync correctness** (step 6): mapping between `CPU_t` individual flag bytes
  and `kvm_regs.rflags` packed word. The `makeflagsword`/`decodeflagsword` macros already
  exist in XTulator, but edge cases around reserved flag bits (bit 1 always set) need care.
- **Memory access boundary**: XTulator's `cpu_read`/`cpu_write` must return MMIO exit for
  addresses >= 640KB. Need to handle this mid-instruction (e.g. during a `MOV` that
  crosses the boundary).
- **Segment wrapping**: real 8086 wraps at 1MB. XTulator handles this already via
  `& 0xFFFFF` masking in `segbase()`.
- **Self-modifying code**: works naturally (both backends read from same `mem` buffer).

## References

- Intel 8086/8088 User's Manual (instruction encoding tables)
- [XTulator](https://github.com/mikechambers84/XTulator) — portable 80186 emulator (GPLv2)
- [fake86](https://github.com/mikechambers84/fake86) — predecessor by same author
- [libx86emu](https://github.com/wfeldt/libx86emu) — embeddable x86 emulator (BSD)
- kvikdos `mini_kvm.h` — portable struct definitions
