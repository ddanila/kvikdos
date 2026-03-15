/*
 * cpu8086.c: software 8086 CPU interpreter for kvikdos (non-KVM platforms).
 *
 * This file is part of kvikdos. Licensed under GNU GPL >=2.0.
 * 8086 instruction set based on 8086tiny by Adrian Cable (MIT License).
 */

#include "cpu8086.h"

#include <stdio.h>
#include <string.h>

/* --- IP/SP/flags update helper --- */

/* Set the low 16 bits of a 64-bit register, preserving upper bits.
 * kvm_regs uses __u64 for all registers; we only modify the 16-bit part. */
#define SET16(reg, val) ((reg) = ((reg) & ~(__u64)0xFFFF) | (unsigned short)(val))

/* Advance IP by n bytes. */
#define ADVANCE_IP(n) SET16(regs->rip, (unsigned short)regs->rip + (n))

/* --- Helper macros for memory access --- */

/* Linear address from segment:offset (wraps at 1MB). */
#define LINEAR(seg, ofs) (((unsigned)(seg) * 16 + (unsigned)(ofs)) & 0xFFFFF)

/* Read/write 8-bit and 16-bit values from guest memory. */
#define MEM8(addr) (((unsigned char *)(mem))[(addr)])
#define MEM16(addr) (*(unsigned short *)(((unsigned char *)(mem)) + (addr)))

/* --- Register accessors --- */

/* 16-bit register array (AX=0, CX=1, DX=2, BX=3, SP=4, BP=5, SI=6, DI=7). */
#define REG16(idx) (*(unsigned short *)(&(&regs->rax)[(idx)]))

/* 8-bit register decode: 0=AL,1=CL,2=DL,3=BL,4=AH,5=CH,6=DH,7=BH.
 * Low regs (0-3): low byte of AX,CX,DX,BX.
 * High regs (4-7): high byte of AX,CX,DX,BX. */
static unsigned char *reg8_ptr(struct kvm_regs *regs, int idx) {
  unsigned short *base = &REG16(idx & 3);
  return (unsigned char *)base + (idx >> 2);  /* +0 for low, +1 for high */
}
#define REG8(idx) (*reg8_ptr(regs, (idx)))

/* Segment register by index (0=ES, 1=CS, 2=SS, 3=DS). */
static unsigned short get_seg(struct kvm_sregs *sregs, int idx) {
  switch (idx) {
    case 0: return (unsigned short)sregs->es.selector;
    case 1: return (unsigned short)sregs->cs.selector;
    case 2: return (unsigned short)sregs->ss.selector;
    case 3: return (unsigned short)sregs->ds.selector;
  }
  return 0;
}

static void set_seg(struct kvm_sregs *sregs, int idx, unsigned short val) {
  struct kvm_segment *seg;
  switch (idx) {
    case 0: seg = &sregs->es; break;
    case 1: seg = &sregs->cs; break;
    case 2: seg = &sregs->ss; break;
    case 3: seg = &sregs->ds; break;
    default: return;
  }
  seg->selector = val;
  seg->base = (unsigned)val << 4;
}

/* Flags bits. */
#define FLAG_CF  0x0001
#define FLAG_PF  0x0004
#define FLAG_AF  0x0010
#define FLAG_ZF  0x0040
#define FLAG_SF  0x0080
#define FLAG_TF  0x0100
#define FLAG_IF  0x0200
#define FLAG_DF  0x0400
#define FLAG_OF  0x0800

/* Current CS:IP as linear address. */
#define CS_IP_LINEAR() LINEAR(sregs->cs.selector, (unsigned short)regs->rip)

/* Push a 16-bit value onto SS:SP. */
static void push16(struct kvm_regs *regs, struct kvm_sregs *sregs, void *mem, unsigned short val) {
  unsigned short sp = (unsigned short)regs->rsp - 2;
  SET16(regs->rsp, sp);
  MEM16(LINEAR(sregs->ss.selector, sp)) = val;
}

/* Pop a 16-bit value from SS:SP. */
static unsigned short pop16(struct kvm_regs *regs, struct kvm_sregs *sregs, void *mem) {
  unsigned short sp = (unsigned short)regs->rsp;
  unsigned short val = MEM16(LINEAR(sregs->ss.selector, sp));
  SET16(regs->rsp, sp + 2);
  return val;
}

/* Execute INT: push flags/CS/IP, clear IF/TF, jump to IVT entry. */
static void do_int(struct kvm_regs *regs, struct kvm_sregs *sregs,
                   void *mem, unsigned char int_num, unsigned short ip_after) {
  unsigned short flags16 = (unsigned short)(regs->rflags & 0xFFFF) | 0xF002;
  push16(regs, sregs, mem, flags16);
  push16(regs, sregs, mem, (unsigned short)sregs->cs.selector);
  push16(regs, sregs, mem, ip_after);
  regs->rflags &= ~(FLAG_IF | FLAG_TF);
  set_seg(sregs, 1, MEM16(int_num * 4 + 2));
  SET16(regs->rip, MEM16(int_num * 4));
}

int cpu8086_run(struct kvm_regs *regs, struct kvm_sregs *sregs,
                struct kvm_run *run, void *mem, unsigned mem_size) {
  (void)mem_size;  /* TODO: bounds checking for MMIO exits. */

  for (;;) {
    unsigned linear = CS_IP_LINEAR();
    unsigned char opcode = MEM8(linear);

    switch (opcode) {

    /* NOP (0x90) */
    case 0x90:
      ADVANCE_IP(1);
      break;

    /* HLT (0xF4) - exit to kvikdos for INT dispatch. */
    case 0xF4:
      ADVANCE_IP(1);
      run->exit_reason = KVM_EXIT_HLT;
      return 0;

    /* INT 3 (0xCC) - single-byte breakpoint interrupt. */
    case 0xCC:
      do_int(regs, sregs, mem, 3, (unsigned short)regs->rip + 1);
      break;

    /* INT imm8 (0xCD nn). */
    case 0xCD:
      do_int(regs, sregs, mem, MEM8(linear + 1), (unsigned short)regs->rip + 2);
      break;

    /* INTO (0xCE) - interrupt on overflow. */
    case 0xCE:
      if (regs->rflags & FLAG_OF) {
        do_int(regs, sregs, mem, 4, (unsigned short)regs->rip + 1);
      } else {
        ADVANCE_IP(1);
      }
      break;

    /* IRET (0xCF). */
    case 0xCF: {
      unsigned short new_ip = pop16(regs, sregs, mem);
      unsigned short new_cs = pop16(regs, sregs, mem);
      unsigned short new_flags = pop16(regs, sregs, mem);
      SET16(regs->rip, new_ip);
      set_seg(sregs, 1, new_cs);
      SET16(regs->rflags, new_flags | 0xF002);
      break;
    }

    /* MOV r8, imm8 (0xB0-0xB7). */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
      REG8(opcode - 0xB0) = MEM8(linear + 1);
      ADVANCE_IP(2);
      break;

    /* MOV r16, imm16 (0xB8-0xBF). */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
      REG16(opcode - 0xB8) = MEM16(linear + 1);
      ADVANCE_IP(3);
      break;

    /* Unimplemented opcode - exit with SHUTDOWN. */
    default:
      fprintf(stderr, "cpu8086: unimplemented opcode 0x%02X at %04X:%04X (linear 0x%05X)\n",
              opcode, (unsigned)sregs->cs.selector, (unsigned short)regs->rip, linear);
      run->exit_reason = KVM_EXIT_SHUTDOWN;
      return 0;
    }
  }
}
