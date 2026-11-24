/*
 * prog.c - a test of the Cortex-M4 interpreter (src/emulator/hw-generic/unicard/armcm4.c).
 *
 * Copyright (C) 2026 Martin Lukasek
 * GPL-3.0-or-later, see LICENSE.
 *
 * The same computations compiled for the Cortex-M4 with the FPU (Keil armcc 5.06, build.py: prog_bin.h) and for
 * the host (tests/peripherals/test_armcm4.c includes this file) must give the same results. armtest_run(out) fills
 * ARMTEST_RESULTS words: integer arithmetic, shifts, bit fields, 64-bit multiplies, divisions, a switch (table
 * branch), signed loads, structure copies, intrinsics (CLZ, RBIT, REV, SSAT, USAT) and single precision floating
 * point (no denormals, NaNs or overflows: the host and the M4 agree on those only by luck). No library calls and no
 * writable static data: the ARM image is one read-only region.
 */

#include <stdint.h>

#define ARMTEST_RESULTS 128

#if defined(__ARMCC_VERSION)
#define CLZ(x) __clz(x)
#define RBIT(x) __rbit(x)
#define REV(x) __rev(x)
#define SSAT8(x) __ssat((x), 8)
#define USAT10(x) __usat((x), 10)
#define SQRTF(x) __sqrtf(x)

/* the run-time helpers the compiler calls (no library is linked) */
unsigned long long __aeabi_llsr ( unsigned long long v, int n ) {
    uint32_t lo = (uint32_t) v, hi = (uint32_t) ( v >> 32 );
    if ( n >= 32 ) {
        lo = hi >> ( n - 32 );
        hi = 0;
    } else if ( n ) {
        lo = ( lo >> n ) | ( hi << ( 32 - n ) );
        hi >>= n;
    }
    return ( (unsigned long long) hi << 32 ) | lo;
}

void __aeabi_memcpy4 ( void *d, const void *s, unsigned n ) {
    volatile uint8_t *dd = (volatile uint8_t *) d;
    const volatile uint8_t *ss = (const volatile uint8_t *) s;
    while ( n-- ) *dd++ = *ss++;
}
#else
#include <math.h>
static uint32_t CLZ ( uint32_t x ) {
    uint32_t n = 0;
    while ( ( n < 32 ) && !( x & ( 0x80000000u >> n ) ) ) n++;
    return n;
}
static uint32_t RBIT ( uint32_t x ) {
    uint32_t v = 0;
    for ( int b = 0; b < 32; b++ ) if ( x & ( 1u << b ) ) v |= 1u << ( 31 - b );
    return v;
}
static uint32_t REV ( uint32_t x ) {
    return ( x >> 24 ) | ( ( x >> 8 ) & 0xFF00u ) | ( ( x << 8 ) & 0xFF0000u ) | ( x << 24 );
}
static int32_t SSAT8 ( int32_t x ) {
    return ( x > 127 ) ? 127 : ( x < -128 ) ? -128 : x;
}
static int32_t USAT10 ( int32_t x ) {
    return ( x > 1023 ) ? 1023 : ( x < 0 ) ? 0 : x;
}
#define SQRTF(x) sqrtf(x)
#endif

typedef struct {
    uint32_t a, b, c, d;
    int16_t h;
    int8_t e;
    uint8_t f;
} armtest_rec;

typedef struct {
    uint32_t lo : 5, mid : 11, hi : 16;
} armtest_bits;

static const int8_t k_bytes[8] = { -128, -1, 0, 1, 127, -77, 55, -3 };
static const int16_t k_halves[6] = { -32768, -1, 0, 1, 32767, -1234 };


static uint32_t fbits ( float f ) {
    union {
        float f;
        uint32_t u;
    } v;
    v.f = f;
    return v.u;
}


static uint32_t lcg ( uint32_t *s ) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}


static uint32_t pick ( uint32_t v ) {                     /* a switch: a table branch */
    switch ( v & 15u ) {
        case 0: return v * 3u;
        case 1: return v ^ 0x5A5A5A5Au;
        case 2: return v >> 3;
        case 3: return v + 0x12345u;
        case 4: return ~v;
        case 5: return v << 7;
        case 6: return v - 1u;
        case 7: return v | 0xF0u;
        case 8: return v & 0x0FF0u;
        case 9: return v * v;
        case 10: return v / 7u;
        case 11: return (uint32_t) ( (int32_t) v >> 9 );
        default: return v + 1u;
    }
}


/* r0 = out: ARMTEST_RESULTS words */
void armtest_run ( uint32_t *out ) {
    uint32_t s = 12345u, i, k = 0, acc;
    int32_t sacc;
    uint64_t u64 = 0;
    int64_t s64 = 0;
    float fa = 0.0f, fb = 1.0f, fc;
    armtest_rec r1, r2;
    armtest_bits bits;

    /* integer arithmetic, shifts, flags */
    acc = 0;
    for ( i = 0; i < 64; i++ ) {
        uint32_t x = lcg ( &s ), y = lcg ( &s );
        acc += x;
        acc ^= y >> ( i & 31u );
        acc += ( x << ( y & 31u ) ) | ( x >> ( 32u - ( ( y & 31u ) | 1u ) ) );
        acc -= (uint32_t) ( (int32_t) y >> ( x & 31u ) );
        if ( (int32_t) x < (int32_t) y ) acc += 3u; else acc -= 5u;
        if ( x > y ) acc ^= 0x80000001u;
        acc += ( x & 0xFFu ) * ( y & 0xFFFu );
        acc += pick ( x );
    }
    out[k++] = acc;

    /* divisions */
    sacc = 0;
    acc = 0;
    for ( i = 0; i < 32; i++ ) {
        uint32_t x = lcg ( &s ), y = ( lcg ( &s ) >> ( i & 15u ) ) | 1u;
        acc += x / y + x % y;
        sacc += (int32_t) x / ( (int32_t) ( y >> 1 ) | 1 ) - (int32_t) x % 13;
    }
    out[k++] = acc;
    out[k++] = (uint32_t) sacc;

    /* 64-bit multiplies and shifts */
    for ( i = 0; i < 32; i++ ) {
        uint32_t x = lcg ( &s ), y = lcg ( &s );
        u64 += (uint64_t) x * y;
        s64 += (int64_t) (int32_t) x * (int32_t) y;
        u64 ^= u64 >> ( 1 + ( x & 15u ) );
    }
    out[k++] = (uint32_t) u64;
    out[k++] = (uint32_t) ( u64 >> 32 );
    out[k++] = (uint32_t) s64;
    out[k++] = (uint32_t) ( (uint64_t) s64 >> 32 );

    /* signed loads, structure copies, bit fields */
    sacc = 0;
    for ( i = 0; i < 8; i++ ) sacc = sacc * 3 + k_bytes[i];
    for ( i = 0; i < 6; i++ ) sacc = sacc * 5 + k_halves[i];
    out[k++] = (uint32_t) sacc;
    r1.a = lcg ( &s );
    r1.b = lcg ( &s );
    r1.c = lcg ( &s );
    r1.d = lcg ( &s );
    r1.h = (int16_t) lcg ( &s );
    r1.e = (int8_t) lcg ( &s );
    r1.f = (uint8_t) lcg ( &s );
    r2 = r1;
    out[k++] = r2.a + r2.b * 3u + r2.c * 5u + r2.d * 7u + (uint32_t) ( r2.h * 11 ) + (uint32_t) ( r2.e * 13 ) + r2.f * 17u;
    bits.lo = lcg ( &s );
    bits.mid = lcg ( &s );
    bits.hi = lcg ( &s );
    bits.mid += bits.lo;
    out[k++] = bits.lo + ( bits.mid << 5 ) + ( (uint32_t) bits.hi << 16 );

    /* intrinsics */
    acc = 0;
    for ( i = 0; i < 16; i++ ) {
        uint32_t x = lcg ( &s ) >> ( i * 2u );
        acc = acc * 31u + CLZ ( x ) + RBIT ( x ) + REV ( x );
        acc += (uint32_t) SSAT8 ( (int32_t) ( x >> 20 ) - 2048 ) + (uint32_t) USAT10 ( (int32_t) ( x >> 18 ) - 4096 );
    }
    out[k++] = acc;

    /* floating point */
    for ( i = 0; i < 48; i++ ) {
        float x = (float) ( lcg ( &s ) >> 16 ) / 512.0f + 0.25f, y = (float) (int32_t) ( lcg ( &s ) >> 20 ) - 2000.0f;
        fa = fa + x * y;
        fb = fb * 0.75f + x / ( y < 0.0f ? -y + 1.0f : y + 1.0f );
        fc = SQRTF ( x ) * 3.0f - fb;
        if ( fc > fa ) fa = fa - fc * 0.5f; else fa = fa + fc;
        if ( i < 40 ) out[k++] = fbits ( fa );
        out[k] = (uint32_t) (int32_t) fa + (uint32_t) (int32_t) ( fb * 1000.0f );
        out[k] += ( fa < 0.0f ) ? (uint32_t) (int32_t) ( -fa ) : 7u;
    }
    k++;
    out[k++] = fbits ( fb );
    fc = fa < 0.0f ? -fa : fa;
    out[k++] = (uint32_t) (int32_t) fc;
    out[k] = k;
    k++;
}
