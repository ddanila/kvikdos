/*
 * cpu8086.h: software 8086 CPU interpreter for kvikdos (non-KVM platforms).
 *
 * Executes 8086 real-mode instructions from a memory buffer, using the same
 * kvm_regs/kvm_sregs/kvm_run structs as the KVM backend. Returns on HLT
 * (interrupt dispatch), I/O port access, or MMIO (high memory access).
 *
 * This file is part of kvikdos. Licensed under GNU GPL >=2.0.
 * 8086 instruction set based on 8086tiny by Adrian Cable (MIT License).
 */

#ifndef CPU8086_H
#define CPU8086_H

#include "mini_kvm.h"

/*
 * Execute 8086 instructions until an exit condition occurs.
 *
 * regs/sregs: CPU register state (read/write, modified in place).
 * run:        Exit info (exit_reason + union filled on return).
 * mem:        Guest memory buffer (640KB, directly addressable).
 * mem_size:   Size of mem in bytes (typically 0xa0000).
 *
 * Exit reasons set in run->exit_reason:
 *   KVM_EXIT_HLT  - HLT instruction (used for INT dispatch in kvikdos)
 *   KVM_EXIT_IO   - IN/OUT instruction (run->io filled)
 *   KVM_EXIT_MMIO - Memory access beyond mem_size (run->mmio filled)
 *   KVM_EXIT_SHUTDOWN - Fatal: unimplemented opcode or CPU error
 *
 * For KVM_EXIT_IO with direction==KVM_EXIT_IO_IN, the caller should write
 * the result byte(s) into run->io.data_offset area, then call cpu8086_run()
 * again. The software CPU will pick it up from regs (the IN result is
 * already placed in AL/AX by the caller via the normal kvikdos IO handler).
 *
 * Returns 0 on normal exit, -1 on fatal internal error.
 */
int cpu8086_run(struct kvm_regs *regs, struct kvm_sregs *sregs,
                struct kvm_run *run, void *mem, unsigned mem_size,
                void *video_mem, unsigned video_mem_size, unsigned video_base);

/* Instruction coverage: tracks which (CS<<4)+IP addresses were executed. */
void cpu8086_coverage_enable(void);
void cpu8086_coverage_disable(void);
void cpu8086_coverage_reset(void);
unsigned cpu8086_coverage_count(unsigned start, unsigned size);
const unsigned char *cpu8086_coverage_bitmap(void);

#endif /* CPU8086_H */
