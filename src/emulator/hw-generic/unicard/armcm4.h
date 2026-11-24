/*
 * armcm4.h - an interpreter of the ARM Cortex-M4 (ARMv7E-M, Thumb and Thumb-2, FPv4-SP) for the OpenCPU of the
 * Unicard (unicard_opencpu.c).
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 *
 * The code runs a called routine to its return: armcm4_call() sets SP, LR = a return address no code has, and
 * runs until the PC comes to it (BX LR, POP {PC}). Thread mode, the main stack, no exceptions: SVC goes to a
 * callback, a fault (an unknown instruction, a bus error) stops the run. Memory: fast regions (host buffers, little
 * endian) and callbacks for the rest. The integer instruction set of ARMv7-M with the DSP multiplies used by
 * compilers, the single precision FPU (FPv4-SP: VFP data processing, conversions, compare, loads and stores of
 * single and double registers). Not: the parallel add/subtract (SADD16...) and saturating DSP instructions,
 * exclusive access monitors (LDREX/STREX behave as plain accesses, STREX always succeeds), exceptions and
 * privileged state (MRS/MSR read 0 / ignore). Cycles: an estimate of the Cortex-M4 timing (no wait states).
 */

#ifndef ARMCM4_H
#define ARMCM4_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define ARMCM4_REGIONS 4
#define ARMCM4_RETURN 0xFEFEFEFEu      /* LR of armcm4_call (| 1): the routine returned */

    enum {
        ARMCM4_OK = 0,
        ARMCM4_FAULT_BUS,               /* an access outside the regions that the callback refused */
        ARMCM4_FAULT_UNDEF,             /* an instruction the interpreter does not know (or UDF) */
        ARMCM4_FAULT_STATE,             /* a branch to an even address (ARM state) */
        ARMCM4_FAULT_BKPT,
        ARMCM4_FAULT_LIMIT,             /* too many instructions */
    };

    typedef struct st_ARMCM4 st_ARMCM4;

    /* memory outside the fast regions: size 1, 2 or 4 bytes; 0 = OK, else a bus fault */
    typedef int (*armcm4_read_fn) ( st_ARMCM4 *c, uint32_t addr, int size, uint32_t *value );
    typedef int (*armcm4_write_fn) ( st_ARMCM4 *c, uint32_t addr, int size, uint32_t value );
    typedef void (*armcm4_svc_fn) ( st_ARMCM4 *c, uint32_t imm );

    typedef struct {
        uint32_t base, size;
        uint8_t *mem;
    } st_ARMCM4_REGION;

    struct st_ARMCM4 {
        uint32_t r[16];                 /* r[15] = the address of the instruction being run */
        uint32_t n, z, c, v, q;         /* APSR flags, 0 or 1 */
        uint32_t ge;                    /* APSR.GE (not used by the instructions implemented) */
        uint32_t itstate;
        uint32_t s[32];                 /* FPU single registers (bits) */
        uint32_t fpscr;
        uint64_t cycles, instructions;
        st_ARMCM4_REGION region[ARMCM4_REGIONS];
        int nregions;
        armcm4_read_fn read;
        armcm4_write_fn write;
        armcm4_svc_fn svc;
        void *user;
        int fault;
        uint32_t fault_pc, fault_addr, fault_insn;
        uint32_t npc;                   /* inside: the address of the next instruction */
    };

    /* zero the state, no regions, no callbacks */
    extern void armcm4_init ( st_ARMCM4 *c );
    extern void armcm4_add_region ( st_ARMCM4 *c, uint32_t base, uint32_t size, uint8_t *mem );
    /* calls addr (bit 0 = Thumb) with the stack pointer sp and runs to its return or a fault, at most `limit`
     * instructions; r0-r3 as the caller set them. Returns ARMCM4_OK or ARMCM4_FAULT_*. */
    extern int armcm4_call ( st_ARMCM4 *c, uint32_t addr, uint32_t sp, uint64_t limit );
    extern const char* armcm4_fault_text ( int fault );

#ifdef __cplusplus
}
#endif

#endif /* ARMCM4_H */
