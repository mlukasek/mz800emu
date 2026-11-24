/*
 * unicard_opencpu.h - OpenCPU of the Unicard mk3b (CAh-CEh, uc3 v0.37 on the MZ-800): ARM code from the MZ.
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 *
 * As the firmware 0.37-0.38 (opencpu.c, NOBOMI): the MZ loads Thumb code into SRAM3 (0x20020000-0x2002FFFF) through
 * the repository and calls it; the code runs in the interpreter armcm4.c. See unicard_opencpu.c.
 */

#ifndef UNICARD_OPENCPU_H
#define UNICARD_OPENCPU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

    extern int unicard_opencpu_available ( void );         /* uc3 v0.37 on the MZ-800 (GETCONFIG STATUS OPENCPU) */
    extern int unicard_opencpu_set_addr ( uint32_t addr ); /* CAh; 1 = OK, 0 = outside SRAM3 */
    extern int unicard_opencpu_write ( uint8_t data );     /* CBh: the address goes on */
    extern uint8_t unicard_opencpu_read ( void );          /* CCh, CEh: the address goes on (0 outside SRAM3) */
    extern int unicard_opencpu_call ( uint32_t addr );     /* CDh: runs the code (Thumb, bit 0 set) */
    /* GDG ticks the called code still runs on the card (0 = done): port 51h reads CDh meanwhile, the accesses the
     * firmware's main loop serves wait */
    extern uint64_t unicard_opencpu_busy_ticks ( void );
    extern uint32_t unicard_opencpu_busy_cpu ( void );     /* the same in CPU T states */
    extern void unicard_opencpu_reset ( void );            /* a reset of the computer: SRAM3 is the cache's again */
    extern void unicard_opencpu_finish ( void );           /* tests: the called code is done now */
    /* the registers the last call did not keep (AAPCS: the card would hang): bit n = rn, 13 SP, 31 s16-s31 */
    extern uint32_t unicard_opencpu_abi_errors ( void );

#ifdef __cplusplus
}
#endif

#endif /* UNICARD_OPENCPU_H */
