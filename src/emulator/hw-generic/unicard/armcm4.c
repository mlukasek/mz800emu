/*
 * armcm4.c - an interpreter of the ARM Cortex-M4 (ARMv7E-M, Thumb and Thumb-2, FPv4-SP), see armcm4.h.
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 *
 * The decoding follows the ARMv7-M Architecture Reference Manual: A5.2 (16-bit), A5.3 (32-bit), A6 (floating
 * point). The PC of an instruction reads as its address + 4. Floating point on the host's single precision
 * (IEEE 754, round to nearest): the multiply-accumulates without fusion round twice as on the M4 (separate
 * statements through volatile), the fused ones use fmaf().
 */

#include <math.h>
#include <string.h>

#include "armcm4.h"

#define BIT(x, n) ( ( (uint32_t) (x) >> ( n ) ) & 1u )
#define BITS(x, hi, lo) ( ( (uint32_t) (x) >> ( lo ) ) & ( 0xFFFFFFFFu >> ( 31 - ( hi ) + ( lo ) ) ) )

enum { SR_LSL, SR_LSR, SR_ASR, SR_ROR, SR_RRX };


static uint32_t sext ( uint32_t v, int bits ) {
    uint32_t m = 1u << ( bits - 1 );
    v &= ( m << 1 ) - 1u;
    return ( v ^ m ) - m;
}


static float f32 ( uint32_t b ) {
    float f;
    memcpy ( &f, &b, 4 );
    return f;
}


static uint32_t b32 ( float f ) {
    uint32_t b;
    memcpy ( &b, &f, 4 );
    return b;
}


/* ---------------- memory ---------------- */

static uint8_t* fast ( st_ARMCM4 *c, uint32_t a, uint32_t size ) {
    for ( int i = 0; i < c->nregions; i++ ) {
        uint32_t off = a - c->region[i].base;
        if ( ( off < c->region[i].size ) && ( c->region[i].size - off >= size ) ) return c->region[i].mem + off;
    };
    return NULL;
}


static void bus_fault ( st_ARMCM4 *c, uint32_t a ) {
    if ( !c->fault ) {
        c->fault = ARMCM4_FAULT_BUS;
        c->fault_addr = a;
    };
}


static uint32_t rd ( st_ARMCM4 *c, uint32_t a, int size ) {
    uint8_t *p = fast ( c, a, (uint32_t) size );
    if ( p ) {
        if ( 4 == size ) return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (uint32_t) p[3] << 24 );
        if ( 2 == size ) return p[0] | ( p[1] << 8 );
        return p[0];
    };
    uint32_t v = 0;
    if ( !c->read || c->read ( c, a, size, &v ) ) {
        bus_fault ( c, a );
        return 0;
    };
    return v;
}


static void wr ( st_ARMCM4 *c, uint32_t a, int size, uint32_t v ) {
    uint8_t *p = fast ( c, a, (uint32_t) size );
    if ( p ) {
        p[0] = (uint8_t) v;
        if ( size >= 2 ) p[1] = (uint8_t) ( v >> 8 );
        if ( 4 == size ) {
            p[2] = (uint8_t) ( v >> 16 );
            p[3] = (uint8_t) ( v >> 24 );
        };
        return;
    };
    if ( !c->write || c->write ( c, a, size, v ) ) bus_fault ( c, a );
}


/* ---------------- helpers ---------------- */

static uint32_t R ( st_ARMCM4 *c, uint32_t n ) {
    return ( 15 == n ) ? c->r[15] + 4 : c->r[n];
}


static uint32_t pc_align ( st_ARMCM4 *c ) {
    return ( c->r[15] + 4 ) & ~3u;
}


static void undef ( st_ARMCM4 *c ) {
    if ( !c->fault ) c->fault = ARMCM4_FAULT_UNDEF;
}


/* BranchWritePC */
static void branch ( st_ARMCM4 *c, uint32_t a ) {
    c->npc = a & ~1u;
    c->cycles += 2;
}


/* BXWritePC / LoadWritePC: bit 0 must be 1 (Thumb) */
static void bx ( st_ARMCM4 *c, uint32_t a ) {
    if ( !( a & 1u ) ) {
        if ( !c->fault ) {
            c->fault = ARMCM4_FAULT_STATE;
            c->fault_addr = a;
        };
        return;
    };
    c->npc = a & ~1u;
    c->cycles += 2;
}


/* a result into Rd (ALUWritePC for the PC) */
static void wreg ( st_ARMCM4 *c, uint32_t d, uint32_t v ) {
    if ( 15 == d ) branch ( c, v );
    else c->r[d] = v;
}


static uint32_t add_c ( st_ARMCM4 *c, uint32_t x, uint32_t y, uint32_t cin, int setflags ) {
    uint64_t u = (uint64_t) x + y + cin;
    uint32_t r = (uint32_t) u;
    if ( setflags ) {
        c->n = r >> 31;
        c->z = ( 0 == r );
        c->c = (uint32_t) ( u >> 32 ) & 1u;
        c->v = ( ( ~( x ^ y ) & ( x ^ r ) ) >> 31 ) & 1u;
    };
    return r;
}


static void nz ( st_ARMCM4 *c, uint32_t r ) {
    c->n = r >> 31;
    c->z = ( 0 == r );
}


static uint32_t shift_c ( uint32_t v, int type, uint32_t n, uint32_t cin, uint32_t *cout ) {
    if ( ( 0 == n ) && ( SR_RRX != type ) ) {
        *cout = cin;
        return v;
    };
    switch ( type ) {
        case SR_LSL:
            if ( n < 32 ) {
                *cout = ( v >> ( 32 - n ) ) & 1u;
                return v << n;
            };
            *cout = ( 32 == n ) ? ( v & 1u ) : 0;
            return 0;
        case SR_LSR:
            if ( n < 32 ) {
                *cout = ( v >> ( n - 1 ) ) & 1u;
                return v >> n;
            };
            *cout = ( 32 == n ) ? ( v >> 31 ) : 0;
            return 0;
        case SR_ASR:
            if ( n < 32 ) {
                *cout = ( v >> ( n - 1 ) ) & 1u;
                return (uint32_t) ( (int32_t) v >> n );
            };
            *cout = v >> 31;
            return ( v >> 31 ) ? 0xFFFFFFFFu : 0;
        case SR_ROR:
        {
            uint32_t s = n & 31u;
            uint32_t r = s ? ( ( v >> s ) | ( v << ( 32 - s ) ) ) : v;
            *cout = r >> 31;
            return r;
        }
        default: /* RRX */
            *cout = v & 1u;
            return ( v >> 1 ) | ( cin << 31 );
    };
}


/* DecodeImmShift: the type and the amount of a shift by an immediate */
static void imm_shift ( uint32_t type, uint32_t imm5, int *t, uint32_t *n ) {
    switch ( type ) {
        case 0: *t = SR_LSL;
            *n = imm5;
            break;
        case 1: *t = SR_LSR;
            *n = imm5 ? imm5 : 32;
            break;
        case 2: *t = SR_ASR;
            *n = imm5 ? imm5 : 32;
            break;
        default:
            *t = imm5 ? SR_ROR : SR_RRX;
            *n = imm5 ? imm5 : 1;
            break;
    };
}


/* ThumbExpandImm_C */
static uint32_t expand_imm ( uint32_t imm12, uint32_t cin, uint32_t *cout ) {
    if ( 0 == ( imm12 >> 10 ) ) {
        uint32_t b = imm12 & 0xFFu;
        *cout = cin;
        switch ( ( imm12 >> 8 ) & 3u ) {
            case 0: return b;
            case 1: return ( b << 16 ) | b;
            case 2: return ( b << 24 ) | ( b << 8 );
            default: return b * 0x01010101u;
        };
    };
    uint32_t unrot = 0x80u | ( imm12 & 0x7Fu ), rot = imm12 >> 7;
    uint32_t r = ( unrot >> rot ) | ( unrot << ( 32 - rot ) );
    *cout = r >> 31;
    return r;
}


static int cond_pass ( st_ARMCM4 *c, uint32_t cond ) {
    int r;
    switch ( cond >> 1 ) {
        case 0: r = c->z;
            break;
        case 1: r = c->c;
            break;
        case 2: r = c->n;
            break;
        case 3: r = c->v;
            break;
        case 4: r = c->c && !c->z;
            break;
        case 5: r = ( c->n == c->v );
            break;
        case 6: r = !c->z && ( c->n == c->v );
            break;
        default: return 1;
    };
    return ( cond & 1u ) ? !r : r;
}


static uint32_t apsr ( st_ARMCM4 *c ) {
    return ( c->n << 31 ) | ( c->z << 30 ) | ( c->c << 29 ) | ( c->v << 28 ) | ( c->q << 27 ) | ( c->ge << 16 );
}


static uint32_t ssat ( st_ARMCM4 *c, int64_t v, uint32_t bits ) {
    int64_t max = ( (int64_t) 1 << ( bits - 1 ) ) - 1, min = -( (int64_t) 1 << ( bits - 1 ) );
    if ( v > max ) {
        c->q = 1;
        return (uint32_t) max;
    };
    if ( v < min ) {
        c->q = 1;
        return (uint32_t) min;
    };
    return (uint32_t) v;
}


static uint32_t usat ( st_ARMCM4 *c, int64_t v, uint32_t bits ) {
    int64_t max = ( (int64_t) 1 << bits ) - 1;
    if ( v > max ) {
        c->q = 1;
        return (uint32_t) max;
    };
    if ( v < 0 ) {
        c->q = 1;
        return 0;
    };
    return (uint32_t) v;
}


static uint32_t rev ( uint32_t v ) {
    return ( v >> 24 ) | ( ( v >> 8 ) & 0xFF00u ) | ( ( v << 8 ) & 0xFF0000u ) | ( v << 24 );
}


/* ---------------- loads and stores ---------------- */

/* one load into Rt: size 1, 2, 4; sgn = sign extended; Rt = PC: an interworking branch (a word) */
static void load ( st_ARMCM4 *c, uint32_t t, uint32_t a, int size, int sgn ) {
    uint32_t v = rd ( c, a, size );                         /* (no extra cycle: see CPUTIME in unicard_opencpu.c) */
    if ( c->fault ) return;
    if ( sgn ) v = sext ( v, size * 8 );
    if ( 15 == t ) bx ( c, v );
    else c->r[t] = v;
}


/* LDM/STM (also PUSH, POP): the registers in the list from the lowest at a, upwards */
static void ldm ( st_ARMCM4 *c, uint32_t a, uint32_t list ) {
    for ( uint32_t i = 0; i < 16; i++ ) {
        if ( !( list & ( 1u << i ) ) ) continue;
        uint32_t v = rd ( c, a, 4 );
        if ( c->fault ) return;
        if ( 15 == i ) bx ( c, v );
        else c->r[i] = v;
        a += 4;
        c->cycles += 1;
    };
}


static void stm ( st_ARMCM4 *c, uint32_t a, uint32_t list ) {
    for ( uint32_t i = 0; i < 16; i++ ) {
        if ( !( list & ( 1u << i ) ) ) continue;
        wr ( c, a, 4, c->r[i] );
        a += 4;
        c->cycles += 1;
    };
}


static uint32_t count ( uint32_t list ) {
    uint32_t n = 0;
    for ( ; list; list &= list - 1 ) n++;
    return n;
}


/* ---------------- 16-bit instructions ---------------- */

static void dp16 ( st_ARMCM4 *c, uint32_t i, int sf ) {
    uint32_t *r = c->r, m = BITS ( i, 5, 3 ), d = i & 7u, res, cout;
    switch ( BITS ( i, 9, 6 ) ) {
        case 0x0: res = r[d] & r[m];
            r[d] = res;
            if ( sf ) nz ( c, res );
            return;
        case 0x1: res = r[d] ^ r[m];
            r[d] = res;
            if ( sf ) nz ( c, res );
            return;
        case 0x2: case 0x3: case 0x4: case 0x7:
        {
            int type = ( 0x2 == BITS ( i, 9, 6 ) ) ? SR_LSL : ( 0x3 == BITS ( i, 9, 6 ) ) ? SR_LSR
                       : ( 0x4 == BITS ( i, 9, 6 ) ) ? SR_ASR : SR_ROR;
            res = shift_c ( r[d], type, r[m] & 0xFFu, c->c, &cout );
            r[d] = res;
            if ( sf ) {
                nz ( c, res );
                c->c = cout;
            };
            return;
        }
        case 0x5: r[d] = add_c ( c, r[d], r[m], c->c, sf );
            return;
        case 0x6: r[d] = add_c ( c, r[d], ~r[m], c->c, sf );
            return;
        case 0x8: nz ( c, r[d] & r[m] );
            return;
        case 0x9: r[d] = add_c ( c, ~r[m], 0, 1, sf ); /* RSB Rd, Rn, #0 */
            return;
        case 0xA: add_c ( c, r[d], ~r[m], 1, 1 );
            return;
        case 0xB: add_c ( c, r[d], r[m], 0, 1 );
            return;
        case 0xC: res = r[d] | r[m];
            r[d] = res;
            if ( sf ) nz ( c, res );
            return;
        case 0xD: res = r[d] * r[m];
            r[d] = res;
            if ( sf ) nz ( c, res );
            return;
        case 0xE: res = r[d] & ~r[m];
            r[d] = res;
            if ( sf ) nz ( c, res );
            return;
        default: res = ~r[m];
            r[d] = res;
            if ( sf ) nz ( c, res );
            return;
    };
}


static void special16 ( st_ARMCM4 *c, uint32_t i ) {
    uint32_t dn = ( BIT ( i, 7 ) << 3 ) | ( i & 7u ), m = BITS ( i, 6, 3 ), op = BITS ( i, 9, 6 );
    if ( op < 4 ) {                                         /* ADD (high registers), no flags */
        wreg ( c, dn, R ( c, dn ) + R ( c, m ) );
    } else if ( op < 8 ) {                                  /* CMP (high registers) */
        add_c ( c, R ( c, dn ), ~R ( c, m ), 1, 1 );
    } else if ( op < 12 ) {                                 /* MOV (high registers) */
        wreg ( c, dn, R ( c, m ) );
    } else if ( op < 14 ) {                                 /* BX */
        bx ( c, R ( c, m ) );
    } else {                                                /* BLX */
        uint32_t t = R ( c, m );
        c->r[14] = ( c->r[15] + 2 ) | 1u;
        bx ( c, t );
    };
}


static void misc16 ( st_ARMCM4 *c, uint32_t i ) {
    uint32_t *r = c->r, op = BITS ( i, 11, 5 );
    if ( 0x0100 == ( i & 0x0500 ) ) {                       /* CBZ, CBNZ */
        uint32_t n = i & 7u, off = ( BIT ( i, 9 ) << 6 ) | ( BITS ( i, 7, 3 ) << 1 );
        if ( ( 0 == r[n] ) != BIT ( i, 11 ) ) branch ( c, r[15] + 4 + off );
        return;
    };
    if ( 0x00 == ( op & 0x7C ) ) {
        r[13] += ( i & 0x7Fu ) << 2;
    } else if ( 0x04 == ( op & 0x7C ) ) {
        r[13] -= ( i & 0x7Fu ) << 2;
    } else if ( 0x10 == ( op & 0x78 ) ) {                   /* SXTH, SXTB, UXTH, UXTB */
        uint32_t m = r[BITS ( i, 5, 3 )], d = i & 7u;
        switch ( BITS ( i, 7, 6 ) ) {
            case 0: r[d] = sext ( m, 16 );
                break;
            case 1: r[d] = sext ( m, 8 );
                break;
            case 2: r[d] = m & 0xFFFFu;
                break;
            default: r[d] = m & 0xFFu;
                break;
        };
    } else if ( 0x20 == ( op & 0x70 ) ) {                   /* PUSH */
        uint32_t list = ( i & 0xFFu ) | ( BIT ( i, 8 ) << 14 ), a = r[13] - 4 * count ( list );
        stm ( c, a, list );
        r[13] = a;
    } else if ( 0x33 == op ) {                              /* CPS: no exceptions here */
    } else if ( 0x50 == ( op & 0x78 ) ) {                   /* REV, REV16, REVSH */
        uint32_t m = r[BITS ( i, 5, 3 )], d = i & 7u;
        switch ( BITS ( i, 7, 6 ) ) {
            case 0: r[d] = rev ( m );
                break;
            case 1: r[d] = ( ( m >> 8 ) & 0x00FF00FFu ) | ( ( m << 8 ) & 0xFF00FF00u );
                break;
            case 3: r[d] = sext ( ( ( m >> 8 ) & 0xFFu ) | ( ( m & 0xFFu ) << 8 ), 16 );
                break;
            default: undef ( c );
                break;
        };
    } else if ( 0x60 == ( op & 0x70 ) ) {                   /* POP */
        uint32_t list = ( i & 0xFFu ) | ( BIT ( i, 8 ) << 15 ), a = r[13];
        r[13] = a + 4 * count ( list );
        ldm ( c, a, list );
    } else if ( 0x70 == ( op & 0x78 ) ) {                   /* BKPT */
        if ( !c->fault ) c->fault = ARMCM4_FAULT_BKPT;
    } else if ( 0x78 == ( op & 0x78 ) ) {                   /* IT, hints */
        if ( i & 0xFu ) c->itstate = i & 0xFFu;
    } else {
        undef ( c );
    };
}


static void exec16 ( st_ARMCM4 *c, uint32_t i, int in_it ) {
    uint32_t *r = c->r, res, cout;
    int sf = !in_it;
    switch ( i >> 11 ) {
        case 0x00: case 0x01: case 0x02:                    /* LSL, LSR, ASR (immediate) */
        {
            int type = (int) ( i >> 11 );
            uint32_t imm5 = BITS ( i, 10, 6 ), d = i & 7u;
            res = shift_c ( r[BITS ( i, 5, 3 )], type, ( ( 0 != type ) && ( 0 == imm5 ) ) ? 32 : imm5, c->c, &cout );
            r[d] = res;
            if ( sf ) {
                nz ( c, res );
                c->c = cout;
            };
            return;
        }
        case 0x03:                                          /* ADD, SUB (register, 3-bit immediate) */
        {
            uint32_t op2 = BIT ( i, 10 ) ? BITS ( i, 8, 6 ) : r[BITS ( i, 8, 6 )], n = r[BITS ( i, 5, 3 )];
            r[i & 7u] = BIT ( i, 9 ) ? add_c ( c, n, ~op2, 1, sf ) : add_c ( c, n, op2, 0, sf );
            return;
        }
        case 0x04:                                          /* MOV (8-bit immediate) */
            res = i & 0xFFu;
            r[BITS ( i, 10, 8 )] = res;
            if ( sf ) nz ( c, res );
            return;
        case 0x05: add_c ( c, r[BITS ( i, 10, 8 )], ~( i & 0xFFu ), 1, 1 );     /* CMP */
            return;
        case 0x06: r[BITS ( i, 10, 8 )] = add_c ( c, r[BITS ( i, 10, 8 )], i & 0xFFu, 0, sf );
            return;
        case 0x07: r[BITS ( i, 10, 8 )] = add_c ( c, r[BITS ( i, 10, 8 )], ~( i & 0xFFu ), 1, sf );
            return;
        case 0x08:
            if ( BIT ( i, 10 ) ) special16 ( c, i );
            else dp16 ( c, i, sf );
            return;
        case 0x09:                                          /* LDR (literal) */
            load ( c, BITS ( i, 10, 8 ), pc_align ( c ) + ( ( i & 0xFFu ) << 2 ), 4, 0 );
            return;
        case 0x0A: case 0x0B:                               /* load, store (register offset) */
        {
            uint32_t a = r[BITS ( i, 5, 3 )] + r[BITS ( i, 8, 6 )], t = i & 7u;
            switch ( BITS ( i, 11, 9 ) ) {
                case 0: wr ( c, a, 4, r[t] );
                    break;
                case 1: wr ( c, a, 2, r[t] );
                    break;
                case 2: wr ( c, a, 1, r[t] );
                    break;
                case 3: load ( c, t, a, 1, 1 );
                    break;
                case 4: load ( c, t, a, 4, 0 );
                    break;
                case 5: load ( c, t, a, 2, 0 );
                    break;
                case 6: load ( c, t, a, 1, 0 );
                    break;
                default: load ( c, t, a, 2, 1 );
                    break;
            };
            return;
        }
        case 0x0C: wr ( c, r[BITS ( i, 5, 3 )] + ( BITS ( i, 10, 6 ) << 2 ), 4, r[i & 7u] );
            return;
        case 0x0D: load ( c, i & 7u, r[BITS ( i, 5, 3 )] + ( BITS ( i, 10, 6 ) << 2 ), 4, 0 );
            return;
        case 0x0E: wr ( c, r[BITS ( i, 5, 3 )] + BITS ( i, 10, 6 ), 1, r[i & 7u] );
            return;
        case 0x0F: load ( c, i & 7u, r[BITS ( i, 5, 3 )] + BITS ( i, 10, 6 ), 1, 0 );
            return;
        case 0x10: wr ( c, r[BITS ( i, 5, 3 )] + ( BITS ( i, 10, 6 ) << 1 ), 2, r[i & 7u] );
            return;
        case 0x11: load ( c, i & 7u, r[BITS ( i, 5, 3 )] + ( BITS ( i, 10, 6 ) << 1 ), 2, 0 );
            return;
        case 0x12: wr ( c, r[13] + ( ( i & 0xFFu ) << 2 ), 4, r[BITS ( i, 10, 8 )] );
            return;
        case 0x13: load ( c, BITS ( i, 10, 8 ), r[13] + ( ( i & 0xFFu ) << 2 ), 4, 0 );
            return;
        case 0x14: r[BITS ( i, 10, 8 )] = pc_align ( c ) + ( ( i & 0xFFu ) << 2 );      /* ADR */
            return;
        case 0x15: r[BITS ( i, 10, 8 )] = r[13] + ( ( i & 0xFFu ) << 2 );               /* ADD Rd, SP, #imm */
            return;
        case 0x16: case 0x17: misc16 ( c, i );
            return;
        case 0x18:                                          /* STM (writeback) */
        {
            uint32_t n = BITS ( i, 10, 8 ), list = i & 0xFFu, a = r[n];
            stm ( c, a, list );
            r[n] = a + 4 * count ( list );
            return;
        }
        case 0x19:                                          /* LDM (writeback when Rn not in the list) */
        {
            uint32_t n = BITS ( i, 10, 8 ), list = i & 0xFFu, a = r[n];
            if ( !( list & ( 1u << n ) ) ) r[n] = a + 4 * count ( list );
            ldm ( c, a, list );
            return;
        }
        case 0x1A: case 0x1B:                               /* B<cond>, UDF, SVC */
        {
            uint32_t cond = BITS ( i, 11, 8 );
            if ( 0xE == cond ) {
                undef ( c );
            } else if ( 0xF == cond ) {
                if ( c->svc ) c->svc ( c, i & 0xFFu );
            } else if ( cond_pass ( c, cond ) ) {
                branch ( c, r[15] + 4 + sext ( ( i & 0xFFu ) << 1, 9 ) );
            };
            return;
        }
        case 0x1C: branch ( c, r[15] + 4 + sext ( ( i & 0x7FFu ) << 1, 12 ) );           /* B */
            return;
        default: undef ( c );
            return;
    };
}


/* ---------------- 32-bit instructions: data processing ---------------- */

/* the data processing operations of the shifted register and the modified immediate forms */
static void dp_op ( st_ARMCM4 *c, uint32_t op, uint32_t s, uint32_t n, uint32_t d, uint32_t op2, uint32_t carry ) {
    uint32_t res;
    switch ( op ) {
        case 0x0: res = R ( c, n ) & op2;                   /* AND, TST */
            break;
        case 0x1: res = R ( c, n ) & ~op2;                  /* BIC */
            break;
        case 0x2: res = ( 15 == n ) ? op2 : ( R ( c, n ) | op2 );       /* ORR, MOV */
            break;
        case 0x3: res = ( 15 == n ) ? ~op2 : ( R ( c, n ) | ~op2 );     /* ORN, MVN */
            break;
        case 0x4: res = R ( c, n ) ^ op2;                   /* EOR, TEQ */
            break;
        case 0x8: res = add_c ( c, R ( c, n ), op2, 0, s ); /* ADD, CMN */
            if ( 15 != d ) wreg ( c, d, res );
            return;
        case 0xA: res = add_c ( c, R ( c, n ), op2, c->c, s );          /* ADC */
            wreg ( c, d, res );
            return;
        case 0xB: res = add_c ( c, R ( c, n ), ~op2, c->c, s );         /* SBC */
            wreg ( c, d, res );
            return;
        case 0xD: res = add_c ( c, R ( c, n ), ~op2, 1, s );            /* SUB, CMP */
            if ( 15 != d ) wreg ( c, d, res );
            return;
        case 0xE: res = add_c ( c, op2, ~R ( c, n ), 1, s );            /* RSB */
            wreg ( c, d, res );
            return;
        default: undef ( c );
            return;
    };
    if ( s ) {
        nz ( c, res );
        c->c = carry;
    };
    if ( ( 15 != d ) || !s || ( ( 0x0 != op ) && ( 0x4 != op ) ) ) wreg ( c, d, res );
}


static void dp_shifted ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t op = BITS ( h1, 8, 5 ), n, cout, amount;
    int type;
    if ( 0x6 == op ) {                                      /* PKHBT, PKHTB */
        uint32_t imm5 = ( BITS ( h2, 14, 12 ) << 2 ) | BITS ( h2, 7, 6 ), m = R ( c, h2 & 0xFu );
        n = R ( c, h1 & 0xFu );
        if ( BIT ( h2, 5 ) ) {
            uint32_t sh = shift_c ( m, SR_ASR, imm5 ? imm5 : 32, c->c, &cout );
            wreg ( c, BITS ( h2, 11, 8 ), ( n & 0xFFFF0000u ) | ( sh & 0xFFFFu ) );
        } else {
            wreg ( c, BITS ( h2, 11, 8 ), ( ( m << imm5 ) & 0xFFFF0000u ) | ( n & 0xFFFFu ) );
        };
        return;
    };
    imm_shift ( BITS ( h2, 5, 4 ), ( BITS ( h2, 14, 12 ) << 2 ) | BITS ( h2, 7, 6 ), &type, &amount );
    uint32_t op2 = shift_c ( R ( c, h2 & 0xFu ), type, amount, c->c, &cout );
    dp_op ( c, op, BIT ( h1, 4 ), h1 & 0xFu, BITS ( h2, 11, 8 ), op2, cout );
}


static void dp_modimm ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t imm12 = ( BIT ( h1, 10 ) << 11 ) | ( BITS ( h2, 14, 12 ) << 8 ) | ( h2 & 0xFFu ), cout;
    uint32_t imm = expand_imm ( imm12, c->c, &cout );
    dp_op ( c, BITS ( h1, 8, 5 ), BIT ( h1, 4 ), h1 & 0xFu, BITS ( h2, 11, 8 ), imm, cout );
}


static void dp_plainimm ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t *r = c->r, n = h1 & 0xFu, d = BITS ( h2, 11, 8 );
    uint32_t imm12 = ( BIT ( h1, 10 ) << 11 ) | ( BITS ( h2, 14, 12 ) << 8 ) | ( h2 & 0xFFu );
    uint32_t imm16 = ( ( h1 & 0xFu ) << 12 ) | ( BIT ( h1, 10 ) << 11 ) | ( BITS ( h2, 14, 12 ) << 8 ) | ( h2 & 0xFFu );
    uint32_t lsb = ( BITS ( h2, 14, 12 ) << 2 ) | BITS ( h2, 7, 6 ), w = h2 & 0x1Fu;
    switch ( BITS ( h1, 8, 4 ) ) {
        case 0x00: r[d] = ( ( 15 == n ) ? pc_align ( c ) : r[n] ) + imm12;     /* ADDW, ADR */
            break;
        case 0x04: r[d] = imm16;                            /* MOVW */
            break;
        case 0x0A: r[d] = ( ( 15 == n ) ? pc_align ( c ) : r[n] ) - imm12;     /* SUBW, ADR */
            break;
        case 0x0C: r[d] = ( r[d] & 0xFFFFu ) | ( imm16 << 16 ); /* MOVT */
            break;
        case 0x10: case 0x12: case 0x18: case 0x1A:         /* SSAT, USAT (bit 5: ASR, else LSL) */
        {
            int asr = (int) BIT ( h1, 5 );
            if ( asr && !lsb ) {                            /* SSAT16, USAT16 */
                undef ( c );
                break;
            };
            int64_t v = asr ? (int64_t) ( (int32_t) r[n] >> lsb ) : (int64_t) (int32_t) ( r[n] << lsb );
            r[d] = ( BITS ( h1, 8, 4 ) < 0x18 ) ? ssat ( c, v, w + 1 ) : usat ( c, v, w );
            break;
        }
        case 0x14: r[d] = sext ( r[n] >> lsb, (int) w + 1 );  /* SBFX */
            break;
        case 0x16:                                          /* BFI, BFC */
        {
            uint32_t mask = ( ( 2u << w ) - 1u ) & ~( ( 1u << lsb ) - 1u );
            if ( w < lsb ) break;
            r[d] = ( r[d] & ~mask ) | ( ( 15 == n ) ? 0 : ( ( r[n] << lsb ) & mask ) );
            break;
        }
        case 0x1C:                                          /* UBFX */
            r[d] = ( r[n] >> lsb ) & ( 0xFFFFFFFFu >> ( 31 - w ) );
            break;
        default: undef ( c );
            break;
    };
}


/* A5.3.12: shifts by a register, extends, REV, CLZ ... */
static void dp_register ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t *r = c->r, op1 = BITS ( h1, 7, 4 ), op2 = BITS ( h2, 7, 4 ), n = h1 & 0xFu, d = BITS ( h2, 11, 8 );
    uint32_t m = r[h2 & 0xFu], cout;
    if ( !( op1 & 8u ) && ( 0 == op2 ) ) {                  /* LSL, LSR, ASR, ROR (register) */
        uint32_t res = shift_c ( r[n], (int) BITS ( op1, 2, 1 ), m & 0xFFu, c->c, &cout );
        r[d] = res;
        if ( op1 & 1u ) {
            nz ( c, res );
            c->c = cout;
        };
        return;
    };
    if ( !( op1 & 8u ) && ( op2 & 8u ) ) {                  /* SXTAH/SXTH, UXTAH/UXTH, SXTAB/SXTB, UXTAB/UXTB */
        uint32_t rot = BITS ( h2, 5, 4 ) * 8, v = rot ? ( ( m >> rot ) | ( m << ( 32 - rot ) ) ) : m, e;
        switch ( op1 ) {
            case 0: e = sext ( v, 16 );
                break;
            case 1: e = v & 0xFFFFu;
                break;
            case 4: e = sext ( v, 8 );
                break;
            case 5: e = v & 0xFFu;
                break;
            default: undef ( c );
                return;
        };
        r[d] = ( 15 == n ) ? e : r[n] + e;
        return;
    };
    if ( ( 0x8 == ( op1 & 0xC ) ) && ( 0x8 == ( op2 & 0xC ) ) ) {
        switch ( ( ( op1 & 3u ) << 2 ) | ( op2 & 3u ) ) {
            case 0x0: r[d] = ssat ( c, (int64_t) (int32_t) m + (int32_t) r[n], 32 );    /* QADD */
                return;
            case 0x2: r[d] = ssat ( c, (int64_t) (int32_t) m - (int32_t) r[n], 32 );    /* QSUB */
                return;
            case 0x4: r[d] = rev ( m );
                return;
            case 0x5: r[d] = ( ( m >> 8 ) & 0x00FF00FFu ) | ( ( m << 8 ) & 0xFF00FF00u );
                return;
            case 0x6:
            {
                uint32_t v = 0;
                for ( int b = 0; b < 32; b++ ) if ( m & ( 1u << b ) ) v |= 1u << ( 31 - b );
                r[d] = v;
                return;
            }
            case 0x7: r[d] = sext ( ( ( m >> 8 ) & 0xFFu ) | ( ( m & 0xFFu ) << 8 ), 16 );
                return;
            case 0xC:                                       /* CLZ */
            {
                uint32_t z = 0;
                while ( ( z < 32 ) && !( m & ( 0x80000000u >> z ) ) ) z++;
                r[d] = z;
                return;
            }
            default: undef ( c );
                return;
        };
    };
    undef ( c );
}


static void multiply ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t *r = c->r, n = r[h1 & 0xFu], m = r[h2 & 0xFu], a = h2 >> 12, d = BITS ( h2, 11, 8 );
    switch ( ( BITS ( h1, 6, 4 ) << 2 ) | BITS ( h2, 5, 4 ) ) {
        case 0x0: r[d] = n * m + ( ( 15 == a ) ? 0 : r[a] ); /* MUL, MLA */
            if ( 15 != a ) c->cycles += 1;
            return;
        case 0x1: r[d] = r[a] - n * m;                      /* MLS */
            c->cycles += 1;
            return;
        case 0x4: case 0x5: case 0x6: case 0x7:             /* SMULxy, SMLAxy */
        {
            int32_t x = (int16_t) ( BIT ( h2, 5 ) ? ( n >> 16 ) : n ), y = (int16_t) ( BIT ( h2, 4 ) ? ( m >> 16 ) : m );
            int64_t p = (int64_t) x * y + ( ( 15 == a ) ? 0 : (int64_t) (int32_t) r[a] );
            if ( ( p > INT32_MAX ) || ( p < INT32_MIN ) ) c->q = 1;
            r[d] = (uint32_t) p;
            return;
        }
        case 0xC: case 0xD:                                 /* SMULWy, SMLAWy */
        {
            int64_t p = ( (int64_t) (int32_t) n * (int16_t) ( BIT ( h2, 4 ) ? ( m >> 16 ) : m ) ) >> 16;
            if ( 15 != a ) p += (int32_t) r[a];
            r[d] = (uint32_t) p;
            return;
        }
        case 0x14: case 0x15:                               /* SMMUL, SMMLA (R: rounding) */
        {
            int64_t p = (int64_t) (int32_t) n * (int32_t) m;
            if ( 15 != a ) p += (int64_t) r[a] << 32;
            if ( BIT ( h2, 4 ) ) p += 0x80000000LL;
            r[d] = (uint32_t) ( (uint64_t) p >> 32 );
            return;
        }
        case 0x18: case 0x19:                               /* SMMLS */
        {
            int64_t p = ( (int64_t) r[a] << 32 ) - (int64_t) (int32_t) n * (int32_t) m;
            if ( BIT ( h2, 4 ) ) p += 0x80000000LL;
            r[d] = (uint32_t) ( (uint64_t) p >> 32 );
            return;
        }
        default: undef ( c );
            return;
    };
}


static void long_multiply ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t *r = c->r, n = r[h1 & 0xFu], m = r[h2 & 0xFu], lo = h2 >> 12, hi = BITS ( h2, 11, 8 );
    uint32_t op = ( BITS ( h1, 6, 4 ) << 4 ) | BITS ( h2, 7, 4 );
    uint64_t p;
    switch ( op ) {
        case 0x00: p = (uint64_t) ( (int64_t) (int32_t) n * (int32_t) m );     /* SMULL */
            break;
        case 0x1F:                                          /* SDIV */
            r[hi] = !m ? 0 : ( ( 0x80000000u == n ) && ( 0xFFFFFFFFu == m ) ) ? n : (uint32_t) ( (int32_t) n / (int32_t) m );
            c->cycles += 3;
            return;
        case 0x20: p = (uint64_t) n * m;                    /* UMULL */
            break;
        case 0x3F: r[hi] = m ? n / m : 0;                   /* UDIV */
            c->cycles += 3;
            return;
        case 0x40: p = (uint64_t) ( (int64_t) (int32_t) n * (int32_t) m ) + ( ( (uint64_t) r[hi] << 32 ) | r[lo] );   /* SMLAL */
            break;
        case 0x60: p = (uint64_t) n * m + ( ( (uint64_t) r[hi] << 32 ) | r[lo] );       /* UMLAL */
            break;
        case 0x66: p = (uint64_t) n * m + r[hi] + r[lo];    /* UMAAL */
            break;
        default: undef ( c );
            return;
    };
    r[lo] = (uint32_t) p;
    r[hi] = (uint32_t) ( p >> 32 );
    c->cycles += 1;
}


/* ---------------- 32-bit instructions: loads, stores, branches ---------------- */

/* A5.3.7-A5.3.10: LDR, LDRB, LDRH, LDRSB, LDRSH, STR, STRB, STRH (immediate, register, literal, PLD) */
static void ls_single ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t *r = c->r, n = h1 & 0xFu, t = h2 >> 12, a;
    int size = 1 << BITS ( h1, 6, 5 ), isload = (int) BIT ( h1, 4 ), sgn = (int) BIT ( h1, 8 );
    if ( ( 8 == size ) || ( !isload && sgn ) ) {
        undef ( c );
        return;
    };
    if ( isload && ( 15 == n ) ) {                          /* literal */
        a = BIT ( h1, 7 ) ? pc_align ( c ) + ( h2 & 0xFFFu ) : pc_align ( c ) - ( h2 & 0xFFFu );
    } else if ( BIT ( h1, 7 ) ) {                           /* imm12 */
        a = r[n] + ( h2 & 0xFFFu );
    } else if ( BIT ( h2, 11 ) ) {                          /* imm8: P U W */
        uint32_t off = BIT ( h2, 9 ) ? r[n] + ( h2 & 0xFFu ) : r[n] - ( h2 & 0xFFu );
        a = BIT ( h2, 10 ) ? off : r[n];
        if ( BIT ( h2, 8 ) ) r[n] = off;
    } else if ( 0 == BITS ( h2, 11, 6 ) ) {                 /* register, LSL #0-3 */
        a = r[n] + ( r[h2 & 0xFu] << BITS ( h2, 5, 4 ) );
    } else {
        undef ( c );
        return;
    };
    if ( isload ) {
        if ( ( 15 == t ) && ( 4 != size ) ) return;         /* PLD, PLI: hints */
        load ( c, t, a, size, sgn );
    } else {
        wr ( c, a, size, r[t] );
    };
}


static void ldm_stm32 ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t *r = c->r, n = h1 & 0xFu, list = h2 & 0xFFFFu, w = BIT ( h1, 5 ), cnt = count ( list ), a;
    if ( 1 == BITS ( h1, 8, 7 ) ) {                         /* IA */
        a = r[n];
        if ( w && !( BIT ( h1, 4 ) && ( list & ( 1u << n ) ) ) ) r[n] = a + 4 * cnt;
    } else if ( 2 == BITS ( h1, 8, 7 ) ) {                  /* DB */
        a = r[n] - 4 * cnt;
        if ( w && !( BIT ( h1, 4 ) && ( list & ( 1u << n ) ) ) ) r[n] = a;
    } else {
        undef ( c );
        return;
    };
    if ( BIT ( h1, 4 ) ) ldm ( c, a, list );
    else stm ( c, a, list );
}


static void dual_excl_tb ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t *r = c->r, op1 = BITS ( h1, 8, 7 ), op2 = BITS ( h1, 5, 4 ), op3 = BITS ( h2, 7, 4 ), n = h1 & 0xFu;
    uint32_t t = h2 >> 12, t2 = BITS ( h2, 11, 8 );
    if ( ( 0 == op1 ) && ( 0 == op2 ) ) {                   /* STREX: always succeeds */
        wr ( c, r[n] + ( ( h2 & 0xFFu ) << 2 ), 4, r[t] );
        r[t2] = 0;
        return;
    };
    if ( ( 0 == op1 ) && ( 1 == op2 ) ) {                   /* LDREX */
        load ( c, t, r[n] + ( ( h2 & 0xFFu ) << 2 ), 4, 0 );
        return;
    };
    if ( ( !( op1 & 2u ) && ( op2 & 2u ) ) || ( op1 & 2u ) ) {  /* LDRD, STRD */
        uint32_t base = ( 15 == n ) ? pc_align ( c ) : r[n], imm = ( h2 & 0xFFu ) << 2;
        uint32_t off = BIT ( h1, 7 ) ? base + imm : base - imm, a = BIT ( h1, 8 ) ? off : base;
        if ( BIT ( h1, 5 ) ) r[n] = off;
        if ( BIT ( h1, 4 ) ) {
            uint32_t v1 = rd ( c, a, 4 ), v2 = rd ( c, a + 4, 4 );
            r[t] = v1;
            r[t2] = v2;
            c->cycles += 2;
        } else {
            wr ( c, a, 4, r[t] );
            wr ( c, a + 4, 4, r[t2] );
            c->cycles += 1;
        };
        return;
    };
    if ( 1 == op1 ) {
        uint32_t m = h2 & 0xFu;
        if ( 0 == op2 ) {                                   /* STREXB, STREXH */
            wr ( c, r[n], ( 4 == op3 ) ? 1 : 2, r[t] );
            r[h2 & 0xFu] = 0;
            return;
        };
        switch ( op3 ) {
            case 0: branch ( c, c->r[15] + 4 + 2 * rd ( c, R ( c, n ) + r[m], 1 ) );   /* TBB */
                return;
            case 1: branch ( c, c->r[15] + 4 + 2 * rd ( c, R ( c, n ) + ( r[m] << 1 ), 2 ) );   /* TBH */
                return;
            case 4: load ( c, t, r[n], 1, 0 );
                return;
            case 5: load ( c, t, r[n], 2, 0 );
                return;
            default: break;
        };
    };
    undef ( c );
}


static void branch_misc ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t op = BITS ( h1, 10, 4 ), op1 = BITS ( h2, 14, 12 ), s = BIT ( h1, 10 ), j1 = BIT ( h2, 13 ), j2 = BIT ( h2, 11 );
    if ( 0 == ( op1 & 5u ) ) {
        if ( 0x38 != ( op & 0x38 ) ) {                      /* B<cond>.W */
            uint32_t imm = sext ( ( s << 20 ) | ( j2 << 19 ) | ( j1 << 18 ) | ( ( h1 & 0x3Fu ) << 12 ) | ( ( h2 & 0x7FFu ) << 1 ), 21 );
            if ( cond_pass ( c, BITS ( h1, 9, 6 ) ) ) branch ( c, c->r[15] + 4 + imm );
        } else if ( 0x38 == ( op & 0x7E ) ) {               /* MSR: the APSR flags */
            uint32_t v = c->r[h1 & 0xFu];
            if ( ( h2 & 0xFFu ) <= 3 && BIT ( h2, 11 ) ) {
                c->n = v >> 31;
                c->z = BIT ( v, 30 );
                c->c = BIT ( v, 29 );
                c->v = BIT ( v, 28 );
                c->q = BIT ( v, 27 );
            };
        } else if ( ( 0x3A == op ) || ( 0x3B == op ) ) {    /* hints, DSB, DMB, ISB, CLREX */
        } else if ( 0x3E == ( op & 0x7E ) ) {               /* MRS */
            uint32_t sysm = h2 & 0xFFu, d = BITS ( h2, 11, 8 );
            c->r[d] = ( sysm <= 3 ) ? apsr ( c ) : ( ( 8 == sysm ) || ( 9 == sysm ) ) ? c->r[13] : 0;
        } else {
            undef ( c );
        };
        return;
    };
    uint32_t i1 = !( j1 ^ s ), i2 = !( j2 ^ s );
    uint32_t imm = sext ( ( s << 24 ) | ( i1 << 23 ) | ( i2 << 22 ) | ( ( h1 & 0x3FFu ) << 12 ) | ( ( h2 & 0x7FFu ) << 1 ), 25 );
    if ( 1 == ( op1 & 5u ) ) {                              /* B.W */
        branch ( c, c->r[15] + 4 + imm );
    } else if ( 5 == ( op1 & 5u ) ) {                       /* BL */
        c->r[14] = ( c->r[15] + 4 ) | 1u;
        branch ( c, c->r[15] + 4 + imm );
    } else {                                                /* BLX to ARM state, UDF */
        undef ( c );
    };
}


/* ---------------- floating point (FPv4-SP) ---------------- */

static float sf ( st_ARMCM4 *c, uint32_t n ) {
    return f32 ( c->s[n] );
}


static void set_sf ( st_ARMCM4 *c, uint32_t n, float v ) {
    c->s[n] = b32 ( v );
}


static void fcmp ( st_ARMCM4 *c, float a, float b ) {
    uint32_t f;
    if ( isnan ( a ) || isnan ( b ) ) f = 0x3;
    else if ( a == b ) f = 0x6;
    else if ( a < b ) f = 0x8;
    else f = 0x2;
    c->fpscr = ( c->fpscr & 0x0FFFFFFFu ) | ( f << 28 );
}


static uint32_t f2i ( float f, int sgn, int rz ) {
    if ( isnan ( f ) ) return 0;
    double v = rz ? trunc ( (double) f ) : nearbyint ( (double) f );
    if ( sgn ) {
        if ( v >= 2147483648.0 ) return 0x7FFFFFFFu;
        if ( v < -2147483648.0 ) return 0x80000000u;
        return (uint32_t) (int32_t) v;
    };
    if ( v >= 4294967296.0 ) return 0xFFFFFFFFu;
    if ( v < 0.0 ) return 0;
    return (uint32_t) v;
}


static void vfp_dp ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t o1 = BIT ( h1, 7 ), o2 = BITS ( h1, 5, 4 ), op = BIT ( h2, 6 );
    uint32_t d = ( ( h2 >> 12 ) << 1 ) | BIT ( h1, 6 ), n = ( ( h1 & 0xFu ) << 1 ) | BIT ( h2, 7 ), m = ( ( h2 & 0xFu ) << 1 ) | BIT ( h2, 5 );
    volatile float p;
    if ( BIT ( h2, 8 ) ) {                                  /* double precision: no FPv4-SP */
        undef ( c );
        return;
    };
    if ( !o1 ) {
        switch ( o2 ) {
            case 0: p = sf ( c, n ) * sf ( c, m );          /* VMLA, VMLS */
                set_sf ( c, d, op ? sf ( c, d ) - p : sf ( c, d ) + p );
                c->cycles += 2;
                return;
            case 1: p = sf ( c, n ) * sf ( c, m );          /* VNMLS, VNMLA */
                set_sf ( c, d, op ? -sf ( c, d ) - p : -sf ( c, d ) + p );
                c->cycles += 2;
                return;
            case 2: p = sf ( c, n ) * sf ( c, m );          /* VMUL, VNMUL */
                set_sf ( c, d, op ? -p : p );
                return;
            default: set_sf ( c, d, op ? sf ( c, n ) - sf ( c, m ) : sf ( c, n ) + sf ( c, m ) );   /* VADD, VSUB */
                return;
        };
    };
    switch ( o2 ) {
        case 0:
            if ( op ) break;
            set_sf ( c, d, sf ( c, n ) / sf ( c, m ) );     /* VDIV */
            c->cycles += 13;
            return;
        case 1: set_sf ( c, d, op ? fmaf ( -sf ( c, n ), sf ( c, m ), -sf ( c, d ) )   /* VFNMA */
                         : fmaf ( sf ( c, n ), sf ( c, m ), -sf ( c, d ) ) );          /* VFNMS */
            c->cycles += 2;
            return;
        case 2: set_sf ( c, d, op ? fmaf ( -sf ( c, n ), sf ( c, m ), sf ( c, d ) )    /* VFMS */
                         : fmaf ( sf ( c, n ), sf ( c, m ), sf ( c, d ) ) );           /* VFMA */
            c->cycles += 2;
            return;
        default:
        {
            uint32_t opc2 = h1 & 0xFu, opc3 = BITS ( h2, 7, 6 );
            if ( !( opc3 & 1u ) ) {                         /* VMOV (immediate): VFPExpandImm */
                uint32_t i8 = ( ( h1 & 0xFu ) << 4 ) | ( h2 & 0xFu );
                c->s[d] = ( BIT ( i8, 7 ) << 31 ) | ( ( BIT ( i8, 6 ) ^ 1u ) << 30 ) | ( ( BIT ( i8, 6 ) ? 0x1Fu : 0u ) << 25 )
                    | ( BITS ( i8, 5, 4 ) << 23 ) | ( ( i8 & 0xFu ) << 19 );
                return;
            };
            switch ( opc2 ) {
                case 0x0: c->s[d] = ( 3 == opc3 ) ? ( c->s[m] & 0x7FFFFFFFu ) : c->s[m];    /* VABS, VMOV */
                    return;
                case 0x1:
                    if ( 3 == opc3 ) {
                        set_sf ( c, d, sqrtf ( sf ( c, m ) ) );     /* VSQRT */
                        c->cycles += 13;
                    } else {
                        c->s[d] = c->s[m] ^ 0x80000000u;    /* VNEG */
                    };
                    return;
                case 0x4: fcmp ( c, sf ( c, d ), sf ( c, m ) );     /* VCMP, VCMPE */
                    return;
                case 0x5: fcmp ( c, sf ( c, d ), 0.0f );    /* VCMP with zero */
                    return;
                case 0x8:                                   /* VCVT.F32.S32 / .U32 */
                    set_sf ( c, d, BIT ( h2, 7 ) ? (float) (int32_t) c->s[m] : (float) c->s[m] );
                    return;
                case 0xC: case 0xD:                         /* VCVT(R).U32/S32.F32 */
                    c->s[d] = f2i ( sf ( c, m ), ( int ) ( opc2 & 1u ), (int) BIT ( h2, 7 ) );
                    return;
                default: break;
            };
            break;
        }
    };
    undef ( c );
}


/* VLDR, VSTR, VLDM, VSTM, VPUSH, VPOP (single and double registers: D[n] = S[2n], S[2n + 1]) */
static void vfp_ls ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t *r = c->r, p = BIT ( h1, 8 ), u = BIT ( h1, 7 ), w = BIT ( h1, 5 ), l = BIT ( h1, 4 ), n = h1 & 0xFu;
    uint32_t dbl = BIT ( h2, 8 ), imm8 = h2 & 0xFFu, first, words, a;
    first = dbl ? ( ( ( BIT ( h1, 6 ) << 4 ) | ( h2 >> 12 ) ) << 1 ) : ( ( ( h2 >> 12 ) << 1 ) | BIT ( h1, 6 ) );
    if ( p && !w ) {                                        /* VLDR, VSTR */
        uint32_t base = ( 15 == n ) ? pc_align ( c ) : r[n];
        a = u ? base + ( imm8 << 2 ) : base - ( imm8 << 2 );
        words = dbl ? 2 : 1;
    } else if ( !p && u ) {                                 /* IA */
        a = r[n];
        words = imm8;
        if ( w ) r[n] = a + ( imm8 << 2 );
    } else if ( p && !u && w ) {                            /* DB! */
        a = r[n] - ( imm8 << 2 );
        words = imm8;
        r[n] = a;
    } else {
        undef ( c );
        return;
    };
    for ( uint32_t i = 0; ( i < words ) && ( first + i < 32 ); i++, a += 4 ) {
        if ( l ) c->s[first + i] = rd ( c, a, 4 );
        else wr ( c, a, 4, c->s[first + i] );
    };
    c->cycles += words;
}


static void coproc ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t *r = c->r;
    if ( ( 0xA != BITS ( h2, 11, 9 ) * 2 ) || ( 0xE != ( h1 >> 12 ) ) ) {  /* cp10, cp11 only */
        undef ( c );
        return;
    };
    if ( 0xEC40 == ( h1 & 0xFFE0u ) ) {                     /* VMOV: two core registers */
        uint32_t t = h2 >> 12, t2 = h1 & 0xFu, m;
        m = BIT ( h2, 8 ) ? ( ( ( BIT ( h2, 5 ) << 4 ) | ( h2 & 0xFu ) ) << 1 ) : ( ( ( h2 & 0xFu ) << 1 ) | BIT ( h2, 5 ) );
        if ( m > 30 ) {
            undef ( c );
            return;
        };
        if ( BIT ( h1, 4 ) ) {
            r[t] = c->s[m];
            r[t2] = c->s[m + 1];
        } else {
            c->s[m] = r[t];
            c->s[m + 1] = r[t2];
        };
        return;
    };
    if ( 0xEC00 == ( h1 & 0xFE00u ) ) {
        vfp_ls ( c, h1, h2 );
        return;
    };
    if ( 0xEE00 != ( h1 & 0xFF00u ) ) {
        undef ( c );
        return;
    };
    if ( !BIT ( h2, 4 ) ) {
        vfp_dp ( c, h1, h2 );
        return;
    };
    /* 8, 16, 32-bit transfers between core and FPU registers */
    uint32_t a = BITS ( h1, 7, 5 ), l = BIT ( h1, 4 ), t = h2 >> 12;
    if ( BIT ( h2, 8 ) ) {
        undef ( c );
        return;
    };
    if ( 0 == a ) {                                         /* VMOV Sn <-> Rt */
        uint32_t sn = ( ( h1 & 0xFu ) << 1 ) | BIT ( h2, 7 );
        if ( l ) r[t] = c->s[sn];
        else c->s[sn] = r[t];
    } else if ( 7 == a ) {                                  /* VMRS, VMSR */
        if ( l ) {
            if ( 15 == t ) {
                c->n = BIT ( c->fpscr, 31 );
                c->z = BIT ( c->fpscr, 30 );
                c->c = BIT ( c->fpscr, 29 );
                c->v = BIT ( c->fpscr, 28 );
            } else {
                r[t] = c->fpscr;
            };
        } else {
            c->fpscr = r[t];
        };
    } else {
        undef ( c );
    };
}


/* ---------------- 32-bit dispatch ---------------- */

static void exec32 ( st_ARMCM4 *c, uint32_t h1, uint32_t h2 ) {
    uint32_t op1 = BITS ( h1, 12, 11 ), op2 = BITS ( h1, 10, 4 );
    if ( 1 == op1 ) {
        if ( 0x00 == ( op2 & 0x64 ) ) ldm_stm32 ( c, h1, h2 );
        else if ( 0x04 == ( op2 & 0x64 ) ) dual_excl_tb ( c, h1, h2 );
        else if ( 0x20 == ( op2 & 0x60 ) ) dp_shifted ( c, h1, h2 );
        else coproc ( c, h1, h2 );
    } else if ( 2 == op1 ) {
        if ( BIT ( h2, 15 ) ) branch_misc ( c, h1, h2 );
        else if ( op2 & 0x20 ) dp_plainimm ( c, h1, h2 );
        else dp_modimm ( c, h1, h2 );
    } else {
        if ( 0x00 == ( op2 & 0x71 ) ) ls_single ( c, h1, h2 );
        else if ( 0x01 == ( op2 & 0x67 ) || 0x03 == ( op2 & 0x67 ) || 0x05 == ( op2 & 0x67 ) ) ls_single ( c, h1, h2 );
        else if ( 0x20 == ( op2 & 0x70 ) ) dp_register ( c, h1, h2 );
        else if ( 0x30 == ( op2 & 0x78 ) ) multiply ( c, h1, h2 );
        else if ( 0x38 == ( op2 & 0x78 ) ) long_multiply ( c, h1, h2 );
        else if ( op2 & 0x40 ) coproc ( c, h1, h2 );
        else undef ( c );
    };
}


/* ---------------- the run ---------------- */

static void step ( st_ARMCM4 *c ) {
    uint32_t pc = c->r[15], h1 = rd ( c, pc, 2 ), h2 = 0, it = c->itstate;
    int is32 = ( ( h1 >> 11 ) >= 0x1D );
    if ( is32 ) h2 = rd ( c, pc + 2, 2 );
    if ( c->fault ) return;
    c->npc = pc + ( is32 ? 4 : 2 );
    c->cycles += 1;
    int in_it = ( 0 != ( it & 0xFu ) );
    if ( !in_it || cond_pass ( c, it >> 4 ) ) {
        if ( is32 ) exec32 ( c, h1, h2 );
        else exec16 ( c, h1, in_it );
    };
    if ( in_it ) c->itstate = ( 0 == ( it & 7u ) ) ? 0 : ( ( it & 0xE0u ) | ( ( it << 1 ) & 0x1Fu ) );
    if ( c->fault ) {
        c->fault_pc = pc;
        c->fault_insn = is32 ? ( ( h1 << 16 ) | h2 ) : h1;
        return;
    };
    c->r[15] = c->npc;
    c->instructions++;
}


void armcm4_init ( st_ARMCM4 *c ) {
    memset ( c, 0, sizeof ( *c ) );
}


void armcm4_add_region ( st_ARMCM4 *c, uint32_t base, uint32_t size, uint8_t *mem ) {
    if ( c->nregions >= ARMCM4_REGIONS ) return;
    c->region[c->nregions].base = base;
    c->region[c->nregions].size = size;
    c->region[c->nregions].mem = mem;
    c->nregions++;
}


int armcm4_call ( st_ARMCM4 *c, uint32_t addr, uint32_t sp, uint64_t limit ) {
    c->fault = ARMCM4_OK;
    c->itstate = 0;
    c->r[13] = sp;
    c->r[14] = ARMCM4_RETURN | 1u;
    if ( !( addr & 1u ) ) {
        c->fault = ARMCM4_FAULT_STATE;
        c->fault_pc = c->fault_addr = addr;
        return c->fault;
    };
    c->r[15] = addr & ~1u;
    uint64_t end = c->instructions + limit;
    while ( ( ARMCM4_RETURN != c->r[15] ) && !c->fault ) {
        if ( c->instructions >= end ) {
            c->fault = ARMCM4_FAULT_LIMIT;
            c->fault_pc = c->r[15];
            break;
        };
        step ( c );
    };
    return c->fault;
}


const char* armcm4_fault_text ( int fault ) {
    switch ( fault ) {
        case ARMCM4_OK: return "OK";
        case ARMCM4_FAULT_BUS: return "bus fault";
        case ARMCM4_FAULT_UNDEF: return "undefined instruction";
        case ARMCM4_FAULT_STATE: return "branch to ARM state";
        case ARMCM4_FAULT_BKPT: return "BKPT";
        case ARMCM4_FAULT_LIMIT: return "instruction limit";
        default: return "?";
    };
}
