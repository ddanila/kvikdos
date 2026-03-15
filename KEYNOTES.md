# KEYNOTES — kvikdos macOS port (software 8086 CPU backend)

Non-trivial findings and gotchas discovered during the macOS port work.
See MACOS-PORT.md for the overall plan and architecture.

## kvm_regs register order vs x86 encoding order

**Critical**: `struct kvm_regs` (from Linux KVM ABI) has fields in order:
```
rax, rbx, rcx, rdx, rsi, rdi, rsp, rbp
```
So `(&regs->rax)[0]` = AX, `[1]` = BX, `[2]` = CX, `[3]` = DX.

But x86 instruction encoding uses a different order:
```
AX=0, CX=1, DX=2, BX=3, SP=4, BP=5, SI=6, DI=7
```

This means you **cannot** index into `kvm_regs` fields directly using x86 register
encoding indices. A translation table is required. XTulator's `CPU_t.regs.wordregs[8]`
uses the x86 encoding order, so register sync between `CPU_t` and `kvm_regs` must
remap indices, not just memcpy.

The `REG16(idx)` macro in the original cpu8086.c skeleton was buggy for this reason —
it assumed x86 encoding order matched kvm_regs field order.

## XTulator CPU core external dependencies

XTulator's cpu.c has only 6 external function dependencies:
- `cpu_read(cpu, addr32)` / `cpu_write(cpu, addr32, value)` — memory access
- `cpu_readw(cpu, addr32)` / `cpu_writew(cpu, addr32, value)` — 16-bit memory (inline wrappers over cpu_read/write)
- `port_read(cpu, port)` / `port_write(cpu, port, value)` — I/O ports
- `port_readw(cpu, port)` / `port_writew(cpu, port, value)` — 16-bit I/O ports
- `i8259_nextintr()` — only in `cpu_interruptCheck()`, not in `cpu_exec()` itself

The `cpu` parameter in `cpu_read`/`cpu_write` is **unused** by XTulator's own
implementation (memory.c uses global arrays). But the parameter exists in the
signature, which is convenient — we can use it to pass context (e.g. a pointer to
kvikdos's flat memory buffer).

## XTulator cpu_exec() is void, runs N iterations

`cpu_exec(CPU_t* cpu, uint32_t execloops)` is `void` and runs a fixed number of
instruction loops. For kvikdos, we need it to **return** on HLT, I/O, or MMIO.
The modification approach: change HLT to return immediately (instead of setting
`hltstate`), and have port_read/port_write/cpu_read/cpu_write signal "exit needed"
(e.g. via a field in CPU_t or a wrapper struct) so the main loop can break.

## XTulator flags: individual bytes, not packed word

XTulator stores each flag as a separate `uint8_t` in `CPU_t` (cf, pf, af, zf, sf,
tf, ifl, df, of). kvikdos's `kvm_regs.rflags` is a packed 64-bit word.
Conversion macros `makeflagsword(cpu)` and `decodeflagsword(cpu, val)` already exist
in cpu.h. Must be called at cpu8086_run() entry/exit boundaries.

## FUNC_INLINE in XTulator

XTulator's config.h defines:
```c
#ifdef _WIN32
#define FUNC_INLINE __forceinline
#else
#define FUNC_INLINE __attribute__((always_inline))
#endif
```
For kvikdos (compiled with `-ansi -pedantic`), `__attribute__` may produce warnings.
May need to redefine as `static inline` or just `static` for portability.

## mini_kvm.h needed include guard

The original mini_kvm.h had no `#ifndef` include guard. When both kvikdos.c and
cpu8086.c include it (directly and via cpu8086.h), this caused redefinition errors
for all structs. Fixed by adding `#ifndef MINI_KVM_H` / `#define MINI_KVM_H` /
`#endif`.

## INT dispatch: IVT -> HLT -> userspace

kvikdos sets up IVT so each INT vector points to segment `INT_HLT_PARA` (0x54)
where HLT instructions live. Both KVM and software CPU follow the same path:
INT -> push flags/CS/IP -> load CS:IP from IVT -> execute HLT -> exit to kvikdos.
The software CPU must implement INT microcode (push/load from IVT) but does NOT
need to handle any INT semantics — kvikdos does that in userspace after the HLT exit.

## macOS portability notes

- `MAP_ANONYMOUS` works on macOS (synonym for `MAP_ANON`)
- `MAP_NORESERVE` does not exist on macOS — harmless to omit (macOS always overcommits)
- `madvise(MADV_DONTNEED)` works on macOS
- `memmem()` is not in glibc on all platforms — kvikdos already has `my_memmem()` fallback
- `poll.h` works on macOS
- `_IO`/`_IOR`/`_IOW` macros exist on macOS via `sys/ioccom.h`
