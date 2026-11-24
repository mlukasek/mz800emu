/*
 * unicard_opencpu.c - OpenCPU of the Unicard mk3b (CAh-CEh, uc3 v0.37 on the MZ-800): ARM code from the MZ.
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 *
 * As the firmware 0.37-0.38 (opencpu.c, NOBOMI; UC-UniMgr docs/api.md 4.8): the MZ writes Thumb code into SRAM3
 * (CAh address, CBh byte, CCh and CEh read) and calls it (CDh). The card runs it in its main loop: until it
 * returns, the data port 51h reads CDh (its bus interrupt), whatever needs the main loop waits (status, commands),
 * the VGA goes on. The code finds the card's copy of the VRAM (4 planes of 8 KB, MZ-800 order) through the address
 * in the firmware's vector table at 0x08008228; SVC 0 puts a pixel (r0 x, r1 y, r2 colour) into it.
 *
 * The emulation runs the code in the Cortex-M4 interpreter (armcm4.c) at once, when CDh comes, and keeps the card
 * busy for the time the card needs: its M4 cycles at an effective 56 MHz and 100 us to start (measured with CPUTIME
 * of UC-UniMgr on the author's card, fw 0.38b1, 2026-11-24: the card takes 2.8-3.1 times the M4 cycles at 168 MHz,
 * ALU 2.91, loads and stores 2.97, FPU 2.80, divides 2.91, compiled code 3.10; it runs the code from SRAM3 over the
 * system bus, the VGA and bus interrupts take their share; in the 320 x 200 mode about 1 % more). The code must keep r4-r11,
 * s16-s31 and SP (AAPCS): the firmware calls it from its main loop, whose registers it would spoil (the card then
 * hangs, e.g. with the MZ in WAIT); the emulation checks them and reports it. Memory of the code: SRAM3 64 KB, a stack of 32 KB under
 * 0x20010000 (on the card the main loop's stack), the CCM with the VRAM copy at 0x10002620 (the address of the
 * firmware 0.38b1) mapped onto the GDG VRAM of the emulator: the emulator shows what the card shows on its VGA
 * (on a real MZ the program's own output only), the rest of the CCM a scratch RAM; the flash reads 0 but the VRAM
 * pointer. Anything else is a bus fault: the card would crash, the emulation reports it and stops the call.
 * A reset of the computer clears SRAM3 (the RAM-disk cache of the card takes it back).
 */

#include <stdio.h>
#include <string.h>

#include "unicard.h"
#include "unicard_opencpu.h"
#include "armcm4.h"
#include "hw-generic/gdg/gdgclk.h"
#include "hw-generic/gdg/gdg.h"

#if MZARCH == 800
#include "mzarch/mz800/memory/mz800_memory.h"
#include "mzarch/mz800/gdg/mz800_framebuffer.h"
#endif

#define OC_SRAM3 0x20020000u
#define OC_SRAM3_SIZE 0x10000u
#define OC_STACK_TOP 0x20010000u
#define OC_STACK_SIZE 0x8000u
#define OC_CCM 0x10000000u
#define OC_CCM_SIZE 0x10000u
#define OC_VRAM 0x10002620u             /* the card's VRAM copy in the CCM (fw 0.38b1, APPbuild800/APP.map) */
#define OC_VRAM_SIZE 0x8000u
#define OC_VRAM_PTR 0x08008228u         /* the vector table word with the VRAM address */
#define OC_FLASH 0x08000000u
#define OC_FLASH_SIZE 0x00200000u
#define OC_CARD_HZ 56000000u            /* M4 cycles a second on the card (168 MHz / 3.0, CPUTIME) */
#define OC_CALL_US 100u                 /* from the call to the code (the main loop takes the event, CPUTIME) */
#define OC_LIMIT 200000000u             /* instructions of one call (over 1 s of the card) */

static uint8_t s_sram3[OC_SRAM3_SIZE];
static uint8_t s_stack[OC_STACK_SIZE];
static uint8_t s_ccm[OC_CCM_SIZE];
static st_ARMCM4 s_cpu;
static int s_cpu_ready;
static uint32_t s_addr = OC_SRAM3;
static uint64_t s_busy_until;
static uint32_t s_abi_errors;           /* the registers the last call did not keep: bit n = rn, 31 = s16-s31 */


int unicard_opencpu_available ( void ) {
#if MZARCH == 800
    return unicard_get_api_rev2 ( );
#else
    return 0;
#endif
}


static int addr_ok ( uint32_t a ) {
    return ( a - OC_SRAM3 ) < OC_SRAM3_SIZE;
}


#if MZARCH == 800
/* a byte of the card's VRAM copy: plane = offset / 8 KB, as the GDG planes of the emulator */
static uint8_t* vram_byte ( uint32_t off, int write ) {
    uint32_t i = off & 0x1FFFu;
    switch ( off >> 13 ) {
        case 0: return &g_memoryVRAM_I[i];
        case 1: return &g_memoryVRAM_II[i];
        case 2: return write ? &g_memoryVRAM_III_WR[i] : &g_memoryVRAM_III[i];
        default: return write ? &g_memoryVRAM_IV_WR[i] : &g_memoryVRAM_IV[i];
    };
}
#endif


static int oc_read ( st_ARMCM4 *c, uint32_t a, int size, uint32_t *value ) {
    uint32_t v = 0;
    (void) c;
    for ( int i = 0; i < size; i++ ) {
        uint32_t x = a + (uint32_t) i, b;
        if ( ( x - OC_VRAM ) < OC_VRAM_SIZE ) {
#if MZARCH == 800
            b = *vram_byte ( x - OC_VRAM, 0 );
#else
            b = 0;
#endif
        } else if ( ( x - OC_CCM ) < OC_CCM_SIZE ) {
            b = s_ccm[x - OC_CCM];
        } else if ( ( x - OC_VRAM_PTR ) < 4 ) {
            b = ( OC_VRAM >> ( 8 * ( x - OC_VRAM_PTR ) ) ) & 0xFFu;
        } else if ( ( x - OC_FLASH ) < OC_FLASH_SIZE ) {
            b = 0;
        } else {
            return -1;
        };
        v |= b << ( 8 * i );
    };
    *value = v;
    return 0;
}


static int oc_write ( st_ARMCM4 *c, uint32_t a, int size, uint32_t value ) {
    (void) c;
    for ( int i = 0; i < size; i++ ) {
        uint32_t x = a + (uint32_t) i;
        uint8_t b = (uint8_t) ( value >> ( 8 * i ) );
        if ( ( x - OC_VRAM ) < OC_VRAM_SIZE ) {
#if MZARCH == 800
            *vram_byte ( x - OC_VRAM, 1 ) = b;
            g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;
#endif
        } else if ( ( x - OC_CCM ) < OC_CCM_SIZE ) {
            s_ccm[x - OC_CCM] = b;
        } else {
            return -1;
        };
    };
    return 0;
}


/* SVC 0: put_pixel(x, y, colour) into the VRAM copy (as the firmware's __SVC_0) */
static void oc_svc ( st_ARMCM4 *c, uint32_t imm ) {
    if ( 0 != imm ) return;
    int32_t x = (int32_t) c->r[0], y = (int32_t) c->r[1];
    uint32_t col = c->r[2], off = (uint32_t) ( y * 40 + x / 8 ) & 0x1FFFu;
    uint8_t mask = (uint8_t) ( 1u << ( x & 7 ) );
    for ( uint32_t p = 0; p < 4; p++ ) {
        uint32_t v = 0;
        oc_read ( c, OC_VRAM + off + p * 0x2000u, 1, &v );
        v = ( v & ~mask ) | ( ( col & ( 1u << p ) ) ? mask : 0 );
        oc_write ( c, OC_VRAM + off + p * 0x2000u, 1, v );
    };
}


static void cpu_setup ( void ) {
    if ( s_cpu_ready ) return;
    armcm4_init ( &s_cpu );
    armcm4_add_region ( &s_cpu, OC_SRAM3, OC_SRAM3_SIZE, s_sram3 );
    armcm4_add_region ( &s_cpu, OC_STACK_TOP - OC_STACK_SIZE, OC_STACK_SIZE, s_stack );
    s_cpu.read = oc_read;
    s_cpu.write = oc_write;
    s_cpu.svc = oc_svc;
    s_cpu_ready = 1;
}


int unicard_opencpu_set_addr ( uint32_t addr ) {
    if ( !unicard_opencpu_available ( ) || !addr_ok ( addr ) ) return 0;
    s_addr = addr;
    return 1;
}


/* the next byte; past the end of SRAM3 its start */
static void next_addr ( void ) {
    if ( ++s_addr >= OC_SRAM3 + OC_SRAM3_SIZE ) s_addr = OC_SRAM3;
}


int unicard_opencpu_write ( uint8_t data ) {
    if ( !unicard_opencpu_available ( ) || !addr_ok ( s_addr ) ) return 0;
    s_sram3[s_addr - OC_SRAM3] = data;
    next_addr ( );
    return 1;
}


uint8_t unicard_opencpu_read ( void ) {
    if ( !unicard_opencpu_available ( ) || !addr_ok ( s_addr ) ) return 0;
    uint8_t data = s_sram3[s_addr - OC_SRAM3];
    next_addr ( );
    return data;
}


int unicard_opencpu_call ( uint32_t addr ) {
    if ( !unicard_opencpu_available ( ) || !addr_ok ( addr ) ) return 0;
    cpu_setup ( );
    uint64_t cycles = s_cpu.cycles;
    memset ( s_cpu.r, 0, sizeof ( s_cpu.r ) );
    for ( uint32_t i = 4; i <= 11; i++ ) s_cpu.r[i] = 0xC0DE0000u | i;     /* the main loop's registers */
    for ( uint32_t i = 16; i < 32; i++ ) s_cpu.s[i] = 0x7F8C0000u | i;
    int res = armcm4_call ( &s_cpu, addr, OC_STACK_TOP, OC_LIMIT );
    s_abi_errors = 0;
    if ( ARMCM4_OK != res ) {
        fprintf ( stderr, "WARNING: Unicard OpenCPU: %s at %08X (instruction %08X, address %08X); the card would"
                  " crash\n", armcm4_fault_text ( res ), s_cpu.fault_pc, s_cpu.fault_insn, s_cpu.fault_addr );
    } else {
        for ( uint32_t i = 4; i <= 11; i++ ) if ( s_cpu.r[i] != ( 0xC0DE0000u | i ) ) s_abi_errors |= 1u << i;
        if ( OC_STACK_TOP != s_cpu.r[13] ) s_abi_errors |= 1u << 13;
        for ( uint32_t i = 16; i < 32; i++ ) if ( s_cpu.s[i] != ( 0x7F8C0000u | i ) ) s_abi_errors |= 1u << 31;
        if ( s_abi_errors ) {
            fprintf ( stderr, "WARNING: Unicard OpenCPU: the code at %08X did not keep the registers %08Xh (bit n = rn,"
                      " 13 SP, 31 s16-s31; AAPCS): on the card the firmware's main loop goes wrong (it hangs)\n",
                      addr & ~1u, s_abi_errors );
        };
    };
    uint64_t ticks = ( s_cpu.cycles - cycles ) * GDGCLK_BASE / OC_CARD_HZ + (uint64_t) OC_CALL_US * GDGCLK_BASE / 1000000u;
    s_busy_until = gdg_get_total_ticks ( ) + ticks;
    return 1;
}


uint64_t unicard_opencpu_busy_ticks ( void ) {
    if ( !s_busy_until ) return 0;
    uint64_t now = gdg_get_total_ticks ( );
    if ( now >= s_busy_until ) {
        s_busy_until = 0;
        return 0;
    };
    return s_busy_until - now;
}


uint32_t unicard_opencpu_busy_cpu ( void ) {
    uint64_t t = unicard_opencpu_busy_ticks ( ) / GDGCLK2CPU_DIVIDER;
    return ( t > 0xFFFFFFFFu ) ? 0xFFFFFFFFu : (uint32_t) t;
}


uint32_t unicard_opencpu_abi_errors ( void ) {
    return s_abi_errors;
}


void unicard_opencpu_finish ( void ) {
    s_busy_until = 0;
}


void unicard_opencpu_reset ( void ) {
    memset ( s_sram3, 0, sizeof ( s_sram3 ) );
    memset ( s_ccm, 0, sizeof ( s_ccm ) );
    s_addr = OC_SRAM3;
    s_busy_until = 0;
    s_cpu_ready = 0;
}
