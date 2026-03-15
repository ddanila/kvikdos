/*
 * cpu8086.c: software 8086 CPU interpreter for kvikdos (non-KVM platforms).
 *
 * This is a thin adapter that wires XTulator's CPU core into kvikdos.
 * It provides memory/IO callbacks and register sync between XTulator's
 * CPU_t and kvikdos's kvm_regs/kvm_sregs structs.
 *
 * CPU core: XTulator by Mike Chambers (GPLv2).
 * Original source: XTulator/XTulator/cpu/cpu.c
 *
 * This file is part of kvikdos. Licensed under GNU GPL >=2.0.
 */

#include "cpu8086.h"

#include <stdio.h>
#include <string.h>

/* Include our adapted XTulator CPU types (CPU_t, macros, register indices). */
#include "cpu8086_xt.h"

/* -------------------------------------------------------------------- */
/* kvikdos execution context (file-scope, single-threaded).             */
/* Memory/IO callbacks below read this to access guest memory and to    */
/* signal exit conditions back to cpu8086_run().                        */
/* -------------------------------------------------------------------- */
static struct {
	unsigned char *mem;
	unsigned mem_size;
	struct kvm_run *run;
	int exit_pending;
} g_ctx;

/* -------------------------------------------------------------------- */
/* Memory access callbacks for XTulator CPU core.                       */
/* These replace XTulator's memory.c — we use kvikdos's flat buffer.    */
/* -------------------------------------------------------------------- */

uint8_t cpu_read(CPU_t* cpu, uint32_t addr) {
	(void)cpu;
	addr &= 0xFFFFF; /* 1MB wrap */
	if (addr < g_ctx.mem_size) {
		return g_ctx.mem[addr];
	}
	/* MMIO: address beyond guest RAM. */
	g_ctx.run->exit_reason = KVM_EXIT_MMIO;
	g_ctx.run->mmio.phys_addr = addr;
	g_ctx.run->mmio.len = 1;
	g_ctx.run->mmio.is_write = 0;
	g_ctx.exit_pending = 1;
	return 0xFF;
}

void cpu_write(CPU_t* cpu, uint32_t addr, uint8_t value) {
	(void)cpu;
	addr &= 0xFFFFF;
	if (addr < g_ctx.mem_size) {
		g_ctx.mem[addr] = value;
		return;
	}
	/* MMIO write. */
	g_ctx.run->exit_reason = KVM_EXIT_MMIO;
	g_ctx.run->mmio.phys_addr = addr;
	g_ctx.run->mmio.len = 1;
	g_ctx.run->mmio.is_write = 1;
	g_ctx.run->mmio.data[0] = value;
	g_ctx.exit_pending = 1;
}

/* cpu_readw/cpu_writew are defined in XTulator's cpu.c (FUNC_INLINE).
 * They call cpu_read/cpu_write, so they get our implementations. */

/* -------------------------------------------------------------------- */
/* Port I/O callbacks for XTulator CPU core.                            */
/* These replace XTulator's ports.c — we signal KVM_EXIT_IO and return  */
/* to kvikdos, which handles port I/O the same as the KVM backend.      */
/* -------------------------------------------------------------------- */

uint8_t port_read(CPU_t* cpu, uint16_t portnum) {
	(void)cpu;
	g_ctx.run->exit_reason = KVM_EXIT_IO;
	g_ctx.run->io.direction = KVM_EXIT_IO_IN;
	g_ctx.run->io.size = 1;
	g_ctx.run->io.port = portnum;
	g_ctx.run->io.count = 1;
	g_ctx.run->io.data_offset = 0;
	g_ctx.exit_pending = 1;
	return 0xFF; /* dummy; kvikdos will write the real value to regs */
}

void port_write(CPU_t* cpu, uint16_t portnum, uint8_t value) {
	(void)cpu;
	g_ctx.run->exit_reason = KVM_EXIT_IO;
	g_ctx.run->io.direction = KVM_EXIT_IO_OUT;
	g_ctx.run->io.size = 1;
	g_ctx.run->io.port = portnum;
	g_ctx.run->io.count = 1;
	g_ctx.run->io.data_offset = 0;
	/* Store the output value where kvikdos expects it. */
	((uint8_t *)g_ctx.run)[g_ctx.run->io.data_offset] = value;
	g_ctx.exit_pending = 1;
}

uint16_t port_readw(CPU_t* cpu, uint16_t portnum) {
	(void)cpu;
	g_ctx.run->exit_reason = KVM_EXIT_IO;
	g_ctx.run->io.direction = KVM_EXIT_IO_IN;
	g_ctx.run->io.size = 2;
	g_ctx.run->io.port = portnum;
	g_ctx.run->io.count = 1;
	g_ctx.run->io.data_offset = 0;
	g_ctx.exit_pending = 1;
	return 0xFFFF;
}

void port_writew(CPU_t* cpu, uint16_t portnum, uint16_t value) {
	(void)cpu;
	g_ctx.run->exit_reason = KVM_EXIT_IO;
	g_ctx.run->io.direction = KVM_EXIT_IO_OUT;
	g_ctx.run->io.size = 2;
	g_ctx.run->io.port = portnum;
	g_ctx.run->io.count = 1;
	g_ctx.run->io.data_offset = 0;
	*(uint16_t *)((uint8_t *)g_ctx.run + g_ctx.run->io.data_offset) = value;
	g_ctx.exit_pending = 1;
}

/* -------------------------------------------------------------------- */
/* Include XTulator's cpu.c with preprocessor guard tricks.             */
/*                                                                      */
/* cpu.c includes: "cpu.h", "../config.h", "../debuglog.h"              */
/* We define their include guards so they are skipped — we already      */
/* provided everything they define via cpu8086_xt.h above.              */
/* -------------------------------------------------------------------- */

/* Prevent cpu.h from being re-included (we have cpu8086_xt.h). */
#define _CPU_H_

/* Prevent config.h (we already defined FUNC_INLINE in cpu8086_xt.h). */
#define _CONFIG_H_

/* Prevent debuglog.h — stub debug_log as a no-op. */
#define _DEBUGLOG_H_
#define DEBUG_NONE   0
#define DEBUG_ERROR  1
#define DEBUG_INFO   2
#define DEBUG_DETAIL 3
static void debug_log(uint8_t level, char* format, ...) __attribute__((unused));
static void debug_log(uint8_t level, char* format, ...) {
	(void)level; (void)format;
}

/* Prevent i8259.h (already stubbed in cpu8086_xt.h). */
#define _I8259_H_

/* Stub i8259_nextintr (called from cpu_interruptCheck, which kvikdos doesn't use). */
static uint8_t i8259_nextintr(I8259_t *i8259) {
	(void)i8259;
	return 0;
}

/* Forward declarations for XTulator functions that are used before defined. */
static void cpu_intcall(CPU_t* cpu, uint8_t intnum);
static void cpu_exec(CPU_t* cpu, uint32_t execloops);
void cpu_reset(CPU_t* cpu);
void cpu_interruptCheck(CPU_t* cpu, I8259_t* i8259);
void cpu_registerIntCallback(CPU_t* cpu, uint8_t interrupt, void (*cb)(CPU_t*, uint8_t));

/* Now include the XTulator CPU core.  All its nested #includes will be
 * skipped due to the guards above.  It gets our cpu_read/cpu_write/
 * port_read/port_write implementations, and our CPU_t/macro definitions
 * from cpu8086_xt.h. */
/* Suppress -Wincompatible-function-pointer-types for cpu_registerIntCallback
 * (pre-existing XTulator issue: int_callback uses void* but the setter takes CPU_t*).
 * We don't use this function. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-function-pointer-types"
#include "XTulator/XTulator/cpu/cpu.c"
#pragma GCC diagnostic pop

/* -------------------------------------------------------------------- */
/* Post-include patches.                                                */
/*                                                                      */
/* XTulator's cpu_exec() is a void function that runs N iterations.     */
/* We need it to stop on HLT, I/O, or MMIO.  Rather than modifying the */
/* included source, we wrap it: run 1 instruction at a time and check   */
/* exit_pending.  This is correct but slower than patching the loop.    */
/*                                                                      */
/* TODO: For better performance, fork XTulator's cpu.c and add an       */
/* exit_pending check inside the main loop (one line).                  */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Register sync between XTulator's CPU_t and kvikdos's kvm structs.   */
/*                                                                      */
/* CRITICAL: kvm_regs field order is rax,rbx,rcx,rdx,rsi,rdi,rsp,rbp   */
/* but x86 encoding order is AX=0,CX=1,DX=2,BX=3,SP=4,BP=5,SI=6,DI=7 */
/* These do NOT match — explicit mapping is required.                   */
/* -------------------------------------------------------------------- */

static void regs_kvm_to_cpu(CPU_t *cpu, struct kvm_regs *regs,
                            struct kvm_sregs *sregs) {
	/* General-purpose registers. */
	cpu->regs.wordregs[regax] = (uint16_t)regs->rax;
	cpu->regs.wordregs[regbx] = (uint16_t)regs->rbx;
	cpu->regs.wordregs[regcx] = (uint16_t)regs->rcx;
	cpu->regs.wordregs[regdx] = (uint16_t)regs->rdx;
	cpu->regs.wordregs[regsi] = (uint16_t)regs->rsi;
	cpu->regs.wordregs[regdi] = (uint16_t)regs->rdi;
	cpu->regs.wordregs[regsp] = (uint16_t)regs->rsp;
	cpu->regs.wordregs[regbp] = (uint16_t)regs->rbp;

	/* Instruction pointer. */
	cpu->ip = (uint16_t)regs->rip;

	/* Flags: unpack from rflags to individual bytes. */
	decodeflagsword(cpu, (uint16_t)(regs->rflags & 0xFFFF));

	/* Segment registers. */
	cpu->segregs[reges] = (uint16_t)sregs->es.selector;
	cpu->segregs[regcs] = (uint16_t)sregs->cs.selector;
	cpu->segregs[regss] = (uint16_t)sregs->ss.selector;
	cpu->segregs[regds] = (uint16_t)sregs->ds.selector;

	/* Clear halt state — kvikdos handles HLT externally. */
	cpu->hltstate = 0;
}

static void regs_cpu_to_kvm(CPU_t *cpu, struct kvm_regs *regs,
                            struct kvm_sregs *sregs) {
	/* General-purpose registers: write low 16 bits, preserve upper. */
	regs->rax = (regs->rax & ~(uint64_t)0xFFFF) | cpu->regs.wordregs[regax];
	regs->rbx = (regs->rbx & ~(uint64_t)0xFFFF) | cpu->regs.wordregs[regbx];
	regs->rcx = (regs->rcx & ~(uint64_t)0xFFFF) | cpu->regs.wordregs[regcx];
	regs->rdx = (regs->rdx & ~(uint64_t)0xFFFF) | cpu->regs.wordregs[regdx];
	regs->rsi = (regs->rsi & ~(uint64_t)0xFFFF) | cpu->regs.wordregs[regsi];
	regs->rdi = (regs->rdi & ~(uint64_t)0xFFFF) | cpu->regs.wordregs[regdi];
	regs->rsp = (regs->rsp & ~(uint64_t)0xFFFF) | cpu->regs.wordregs[regsp];
	regs->rbp = (regs->rbp & ~(uint64_t)0xFFFF) | cpu->regs.wordregs[regbp];

	/* Instruction pointer. */
	regs->rip = (regs->rip & ~(uint64_t)0xFFFF) | cpu->ip;

	/* Flags: pack individual bytes into rflags. */
	regs->rflags = (regs->rflags & ~(uint64_t)0xFFFF) |
	               (uint16_t)(makeflagsword(cpu) | 0xF002);

	/* Segment registers. */
	sregs->es.selector = cpu->segregs[reges];
	sregs->es.base = (uint64_t)cpu->segregs[reges] << 4;
	sregs->cs.selector = cpu->segregs[regcs];
	sregs->cs.base = (uint64_t)cpu->segregs[regcs] << 4;
	sregs->ss.selector = cpu->segregs[regss];
	sregs->ss.base = (uint64_t)cpu->segregs[regss] << 4;
	sregs->ds.selector = cpu->segregs[regds];
	sregs->ds.base = (uint64_t)cpu->segregs[regds] << 4;
}

/* -------------------------------------------------------------------- */
/* Public interface: cpu8086_run()                                      */
/* -------------------------------------------------------------------- */

int cpu8086_run(struct kvm_regs *regs, struct kvm_sregs *sregs,
                struct kvm_run *run, void *mem, unsigned mem_size) {
	static CPU_t cpu;
	static int initialized = 0;

	if (!initialized) {
		memset(&cpu, 0, sizeof(cpu));
		initialized = 1;
	}

	/* Set up context for memory/IO callbacks. */
	g_ctx.mem = (unsigned char *)mem;
	g_ctx.mem_size = mem_size;
	g_ctx.run = run;
	g_ctx.exit_pending = 0;

	/* Sync kvikdos register state into XTulator CPU. */
	regs_kvm_to_cpu(&cpu, regs, sregs);

	/* Execute instructions one at a time until an exit condition.
	 * Each cpu_exec(cpu, 1) call executes one instruction.
	 * On HLT, the XTulator code sets cpu->hltstate = 1 and returns.
	 * On I/O or MMIO, our callbacks set g_ctx.exit_pending.
	 *
	 * TODO: For better performance, patch the XTulator cpu_exec() loop
	 * to check g_ctx.exit_pending after each instruction, so we can
	 * call cpu_exec(cpu, 0xFFFFFFFF) instead of stepping one-by-one.
	 */
	while (!g_ctx.exit_pending) {
		cpu_exec(&cpu, 1);

		/* HLT sets hltstate=1; translate to KVM_EXIT_HLT. */
		if (cpu.hltstate) {
			run->exit_reason = KVM_EXIT_HLT;
			cpu.hltstate = 0;
			break;
		}
	}

	/* Sync XTulator CPU state back to kvikdos registers. */
	regs_cpu_to_kvm(&cpu, regs, sregs);

	return 0;
}
