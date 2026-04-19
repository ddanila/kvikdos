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

#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

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
	unsigned char *vga_mem;
	unsigned vga_mem_size;  /* Typically 4000 (80x25x2). */
	unsigned video_base;  /* Linear base address: 0xB8000 (color) or 0xB0000 (mono). */
	struct kvm_run *run;
	int exit_pending;
} g_ctx;

/* Instruction coverage bitmap: 1 bit per byte in 1MB address space = 128KB.
 * Set bit at (CS<<4)+IP before each instruction. */
static unsigned char g_coverage_bitmap[1 << 17];  /* 128KB = 131072 bytes. */
static int g_coverage_enabled;

/* Abort flag: checked in the execution loop to force-exit the emulator. */
volatile int g_cpu8086_abort;

/* -------------------------------------------------------------------- */
/* Memory access callbacks for XTulator CPU core.                       */
/* These replace XTulator's memory.c — we use kvikdos's flat buffer.    */
/* -------------------------------------------------------------------- */

/* INVARS (DOS List of Lists) region. Reads at 0xFFF7E..0xFFFB3 must
 * return the synthetic MCB/DOS-internals data that kvikdos's KVM path
 * serves via the MMIO handler (kvikdos.c:5193). The software CPU
 * routes memory reads through cpu_read() directly instead of trapping
 * to the MMIO handler before the instruction completes, so we have
 * to answer inline here. Without this, reads of INVARS return the
 * raw BIOS-ROM 0xFF bytes and programs like MEM.EXE and VC's
 * Memory Info see an empty/bogus MCB chain.
 *
 * Layout matches kvikdos.c's invars_data[]:
 *   -0x02: first MCB segment (PROGRAM_MCB_PARA, little-endian)
 *   +0x00..+0x21: DOS internals (all zero for our purposes)
 *   +0x22: NUL device header (18 bytes, ends device-driver chain)
 */
#define SOFT_INVARS_BASE   0xFFF7EU
#define SOFT_INVARS_END    0xFFFB4U   /* exclusive */
#define SOFT_PROGRAM_MCB   0xFFU      /* matches PSP_PARA-1 in kvikdos.c */

static uint8_t soft_invars_byte(uint32_t addr) {
	uint32_t ofs = addr - SOFT_INVARS_BASE;
	/* -0x02..-0x01: first-MCB-segment little-endian */
	if (ofs == 0) return (uint8_t)(SOFT_PROGRAM_MCB & 0xFF);
	if (ofs == 1) return (uint8_t)((SOFT_PROGRAM_MCB >> 8) & 0xFF);
	/* +0x22..+0x25: NUL header "next device = FFFF:FFFF" */
	if (ofs >= 0x24 && ofs <= 0x27) return 0xFF;
	/* +0x26..+0x27: attribute = 0x8004 (character device + NUL) */
	if (ofs == 0x28) return 0x04;
	if (ofs == 0x29) return 0x80;
	/* +0x2E..+0x35: device name "NUL     " */
	if (ofs == 0x2C) return 'N';
	if (ofs == 0x2D) return 'U';
	if (ofs == 0x2E) return 'L';
	if (ofs >= 0x2F && ofs <= 0x33) return ' ';
	return 0;  /* all other INVARS bytes */
}

/* Read a single byte at paragraph mcb_seg. The MCB at `mcb_seg` lives
 * in guest RAM (below 640 KB), so it's already served by the g_ctx.mem
 * branch in cpu_read. Nothing to do here for the MCB itself — the
 * fields are laid out by DOS/kvikdos at allocation time. */

uint8_t cpu_read(CPU_t* cpu, uint32_t addr) {
	(void)cpu;
	addr &= 0xFFFFF; /* 1MB wrap */
	if (addr < g_ctx.mem_size) {
		return g_ctx.mem[addr];
	}
	if (addr - g_ctx.video_base < g_ctx.vga_mem_size) {
		return g_ctx.vga_mem[addr - g_ctx.video_base];
	}
	if (addr >= SOFT_INVARS_BASE && addr < SOFT_INVARS_END) {
		return soft_invars_byte(addr);
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
	if (addr - g_ctx.video_base < g_ctx.vga_mem_size) {
		g_ctx.vga_mem[addr - g_ctx.video_base] = value;
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
                struct kvm_run *run, void *mem, unsigned mem_size,
                void *video_mem, unsigned video_mem_size, unsigned video_base) {
	static CPU_t cpu;
	static int initialized = 0;

	if (!initialized) {
		memset(&cpu, 0, sizeof(cpu));
		initialized = 1;
	}

	/* Set up context for memory/IO callbacks. */
	g_ctx.mem = (unsigned char *)mem;
	g_ctx.mem_size = mem_size;
	g_ctx.vga_mem = (unsigned char *)video_mem;
	g_ctx.vga_mem_size = video_mem_size;
	g_ctx.video_base = video_base;
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
	{ unsigned insn_count = 0;
	while (!g_ctx.exit_pending) {
		unsigned cov_ip_before = 0, cov_seg_base = 0;
		if (g_coverage_enabled) {
			cov_seg_base = (unsigned)cpu.segregs[regcs] << 4;
			cov_ip_before = cpu.ip;
			{ unsigned addr = (cov_seg_base + cov_ip_before) & 0xFFFFF;
			  g_coverage_bitmap[addr >> 3] |= (1 << (addr & 7));
			}
		}
		if (g_cpu8086_abort) {
			run->exit_reason = KVM_EXIT_SHUTDOWN;
			g_ctx.exit_pending = 1;
			break;
		}
		/* Update BDA timer periodically so programs that busy-wait on
		 * 0040:006Ch (e.g. VC.COM's sync_to_timer for dialog animation)
		 * don't hang.  Also yield to other threads. */
		if ((++insn_count & 0xFFF) == 0) {
			struct timeval tv;
			gettimeofday(&tv, NULL);
			{ const unsigned s = (unsigned)(tv.tv_sec % 86400);
			  unsigned tick = s * 18U + s * 13318U / 65536U + (unsigned)tv.tv_usec * 18U / 1000000U;
			  ((unsigned char *)mem)[0x46c] = (unsigned char)tick;
			  ((unsigned char *)mem)[0x46d] = (unsigned char)(tick >> 8);
			  ((unsigned char *)mem)[0x46e] = (unsigned char)(tick >> 16);
			  ((unsigned char *)mem)[0x46f] = (unsigned char)(tick >> 24);
			}
			if (g_coverage_enabled) sched_yield();
		}
		cpu_exec(&cpu, 1);
		/* Mark ALL bytes of the executed instruction in the coverage bitmap.
		 * After cpu_exec, cpu.ip points to the next instruction. For
		 * sequential execution (no jump), ip_after - ip_before = insn length.
		 * For jumps/calls the delta is large or negative; skip in that case. */
		if (g_coverage_enabled) {
			unsigned delta = cpu.ip - cov_ip_before;
			if (delta >= 2 && delta <= 15) {
				unsigned i;
				for (i = 1; i < delta; ++i) {
					unsigned addr = (cov_seg_base + cov_ip_before + i) & 0xFFFFF;
					g_coverage_bitmap[addr >> 3] |= (1 << (addr & 7));
				}
			}
		}

		/* HLT sets hltstate=1; translate to KVM_EXIT_HLT. */
		if (cpu.hltstate) {
			run->exit_reason = KVM_EXIT_HLT;
			cpu.hltstate = 0;
			break;
		}
	} /* while !exit_pending */
	} /* insn_count scope */

	/* Sync XTulator CPU state back to kvikdos registers. */
	regs_cpu_to_kvm(&cpu, regs, sregs);

	return 0;
}

void cpu8086_coverage_enable(void) {
	g_coverage_enabled = 1;
}

void cpu8086_coverage_disable(void) {
	g_coverage_enabled = 0;
}

void cpu8086_coverage_reset(void) {
	memset(g_coverage_bitmap, 0, sizeof(g_coverage_bitmap));
}

/* Count set bits in coverage bitmap within [start, start+size). */
unsigned cpu8086_coverage_count(unsigned start, unsigned size) {
	unsigned count = 0;
	unsigned i;
	unsigned end = start + size;
	if (end > 0x100000) end = 0x100000;
	for (i = start; i < end; ++i) {
		if (g_coverage_bitmap[i >> 3] & (1 << (i & 7))) ++count;
	}
	return count;
}

/* Get raw bitmap pointer for external analysis. */
const unsigned char *cpu8086_coverage_bitmap(void) {
	return g_coverage_bitmap;
}
