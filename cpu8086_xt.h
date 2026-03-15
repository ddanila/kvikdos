/*
 * cpu8086_xt.h: XTulator CPU types and macros adapted for kvikdos.
 *
 * Derived from XTulator cpu.h by Mike Chambers (GPLv2).
 * Original: XTulator/XTulator/cpu/cpu.h
 *
 * Changes from original:
 *   - Removed #include "cpuconf.h" and "../chipset/i8259.h"
 *   - CPU variant set to 8086 mode inline
 *   - I8259_t stubbed as minimal struct
 *   - Added FUNC_INLINE definition (from config.h)
 */

#ifndef CPU8086_XT_H
#define CPU8086_XT_H

#include <stdint.h>

/* --- CPU variant: 8086 mode (from cpuconf.h) --- */
#define CPU_8086
#define CPU_CLEAR_ZF_ON_MUL
#define CPU_ALLOW_POP_CS
#define CPU_SET_HIGH_FLAGS

/* --- FUNC_INLINE: portable inline hint --- */
#define FUNC_INLINE static

/* --- Stub I8259 PIC (kvikdos handles interrupts via IVT->HLT) --- */
typedef struct {
	uint8_t imr;
	uint8_t irr;
} I8259_t;

/* --- XTulator CPU register and type definitions (from cpu.h) --- */

union _bytewordregs_ {
	uint16_t wordregs[8];
	uint8_t byteregs[8];
};

typedef struct {
	union _bytewordregs_ regs;
	uint8_t	opcode, segoverride, reptype, hltstate;
	uint16_t segregs[4], savecs, saveip, ip, useseg, oldsp;
	uint8_t	tempcf, oldcf, cf, pf, af, zf, sf, tf, ifl, df, of, mode, reg, rm;
	uint16_t oper1, oper2, res16, disp16, temp16, dummy, stacksize, frametemp;
	uint8_t	oper1b, oper2b, res8, disp8, temp8, nestlev, addrbyte;
	uint32_t temp1, temp2, temp3, temp4, temp5, temp32, tempaddr32, ea;
	int32_t	result;
	uint16_t trap_toggle;
	uint64_t totalexec;
	void (*int_callback[256])(void*, uint8_t);
} CPU_t;

#define regax 0
#define regcx 1
#define regdx 2
#define regbx 3
#define regsp 4
#define regbp 5
#define regsi 6
#define regdi 7
#define reges 0
#define regcs 1
#define regss 2
#define regds 3

#ifdef __BIG_ENDIAN__
#define regal 1
#define regah 0
#define regcl 3
#define regch 2
#define regdl 5
#define regdh 4
#define regbl 7
#define regbh 6
#else
#define regal 0
#define regah 1
#define regcl 2
#define regch 3
#define regdl 4
#define regdh 5
#define regbl 6
#define regbh 7
#endif

#define StepIP(mycpu, x)	mycpu->ip += x
#define getmem8(mycpu, x, y)	cpu_read(mycpu, segbase(x) + y)
#define getmem16(mycpu, x, y)	cpu_readw(mycpu, segbase(x) + y)
#define putmem8(mycpu, x, y, z)	cpu_write(mycpu, segbase(x) + y, z)
#define putmem16(mycpu, x, y, z)	cpu_writew(mycpu, segbase(x) + y, z)
#define signext(value)	(int16_t)(int8_t)(value)
#define signext32(value)	(int32_t)(int16_t)(value)
#define getreg16(mycpu, regid)	mycpu->regs.wordregs[regid]
#define getreg8(mycpu, regid)	mycpu->regs.byteregs[byteregtable[regid]]
#define putreg16(mycpu, regid, writeval)	mycpu->regs.wordregs[regid] = writeval
#define putreg8(mycpu, regid, writeval)	mycpu->regs.byteregs[byteregtable[regid]] = writeval
#define getsegreg(mycpu, regid)	mycpu->segregs[regid]
#define putsegreg(mycpu, regid, writeval)	mycpu->segregs[regid] = writeval
#define segbase(x)	((uint32_t) x << 4)

#define makeflagsword(x) \
	( \
	2 | (uint16_t) x->cf | ((uint16_t) x->pf << 2) | ((uint16_t) x->af << 4) | ((uint16_t) x->zf << 6) | ((uint16_t) x->sf << 7) | \
	((uint16_t) x->tf << 8) | ((uint16_t) x->ifl << 9) | ((uint16_t) x->df << 10) | ((uint16_t) x->of << 11) \
	)

#define decodeflagsword(x,y) { \
	uint16_t tmp; \
	tmp = y; \
	x->cf = tmp & 1; \
	x->pf = (tmp >> 2) & 1; \
	x->af = (tmp >> 4) & 1; \
	x->zf = (tmp >> 6) & 1; \
	x->sf = (tmp >> 7) & 1; \
	x->tf = (tmp >> 8) & 1; \
	x->ifl = (tmp >> 9) & 1; \
	x->df = (tmp >> 10) & 1; \
	x->of = (tmp >> 11) & 1; \
}

#define modregrm(x) { \
	x->addrbyte = getmem8(x, x->segregs[regcs], x->ip); \
	StepIP(x, 1); \
	x->mode = x->addrbyte >> 6; \
	x->reg = (x->addrbyte >> 3) & 7; \
	x->rm = x->addrbyte & 7; \
	switch(x->mode) \
	{ \
	case 0: \
	if(x->rm == 6) { \
	x->disp16 = getmem16(x, x->segregs[regcs], x->ip); \
	StepIP(x, 2); \
	} \
	if(((x->rm == 2) || (x->rm == 3)) && !x->segoverride) { \
	x->useseg = x->segregs[regss]; \
	} \
	break; \
 \
	case 1: \
	x->disp16 = signext(getmem8(x, x->segregs[regcs], x->ip)); \
	StepIP(x, 1); \
	if(((x->rm == 2) || (x->rm == 3) || (x->rm == 6)) && !x->segoverride) { \
	x->useseg = x->segregs[regss]; \
	} \
	break; \
 \
	case 2: \
	x->disp16 = getmem16(x, x->segregs[regcs], x->ip); \
	StepIP(x, 2); \
	if(((x->rm == 2) || (x->rm == 3) || (x->rm == 6)) && !x->segoverride) { \
	x->useseg = x->segregs[regss]; \
	} \
	break; \
 \
	default: \
	x->disp8 = 0; \
	x->disp16 = 0; \
	} \
}

/* Function declarations for functions implemented in cpu8086.c (our callbacks).
 * Functions defined as FUNC_INLINE in XTulator's cpu.c (cpu_readw, cpu_writew,
 * cpu_intcall, etc.) are NOT declared here — they come from the #include. */
uint8_t cpu_read(CPU_t* cpu, uint32_t addr);
void cpu_write(CPU_t* cpu, uint32_t addr32, uint8_t value);
void port_write(CPU_t* cpu, uint16_t portnum, uint8_t value);
void port_writew(CPU_t* cpu, uint16_t portnum, uint16_t value);
uint8_t port_read(CPU_t* cpu, uint16_t portnum);
uint16_t port_readw(CPU_t* cpu, uint16_t portnum);

#endif /* CPU8086_XT_H */
