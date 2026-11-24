/*
 * test_armcm4.c - testy interpretu Cortex-M4 (hw-generic/unicard/armcm4.c) pro OpenCPU Unicard.
 *
 * Program tests/data/armcm4/prog.c je tu dvakrát: přeložený pro hostitele (včleněný sem) a pro Cortex-M4
 * s FPU (Keil armcc, tests/data/armcm4/prog_bin.h, build.py). Interpret spustí obraz pro ARM a jeho výsledky
 * se musí shodovat s výsledky hostitele. K tomu krátké ruční programy pro to, co překladač nevygeneruje
 * (blok IT, CBZ, POP do PC, chyby).
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <string.h>
#include <stdio.h>

#include "hw-generic/unicard/armcm4.h"

#include "../data/armcm4/prog.c"
#include "../data/armcm4/prog_bin.h"

#define SRAM3 0x20020000u
#define STACK_TOP 0x20010000u
#define OUT_ADDR 0x2002F000u

static uint8_t s_sram3[0x10000];
static uint8_t s_stack[0x4000];
static st_ARMCM4 s_cpu;


void setUp(void)
{
    memset(s_sram3, 0, sizeof(s_sram3));
    memset(s_stack, 0, sizeof(s_stack));
    armcm4_init(&s_cpu);
    armcm4_add_region(&s_cpu, SRAM3, sizeof(s_sram3), s_sram3);
    armcm4_add_region(&s_cpu, STACK_TOP - sizeof(s_stack), sizeof(s_stack), s_stack);
}


void tearDown(void)
{
}


static uint32_t word_at(uint32_t addr)
{
    const uint8_t *p = &s_sram3[addr - SRAM3];
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}


/* Obraz z armcc proti stejnému programu na hostiteli. */
void test_armcm4_prog_matches_host(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint32_t host[ARMTEST_RESULTS];
    memset(host, 0, sizeof(host));
    armtest_run(host);

    memcpy(s_sram3, armtest_bin, sizeof(armtest_bin));
    s_cpu.r[0] = OUT_ADDR;
    int res = armcm4_call(&s_cpu, ARMTEST_ENTRY, STACK_TOP, 50000000u);
    if (res != ARMCM4_OK) {
        printf("fault %s at %08X, insn %08X, addr %08X\n", armcm4_fault_text(res), s_cpu.fault_pc,
               s_cpu.fault_insn, s_cpu.fault_addr);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARMCM4_OK, res, "běh bez chyby");
    printf("armtest: %llu instructions, %llu cycles\n", (unsigned long long)s_cpu.instructions,
           (unsigned long long)s_cpu.cycles);
    for (int i = 0; i < ARMTEST_RESULTS; i++) {
        char msg[48];
        snprintf(msg, sizeof(msg), "výsledek %d", i);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(host[i], word_at(OUT_ADDR + 4 * (uint32_t)i), msg);
    }
}


/* Ruční program: blok IT (ITTE), CBZ, MOVW/MOVT, PUSH a POP do PC. */
void test_armcm4_it_cbz_pop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static const uint16_t code[] = {
        0xb510,                 /* push {r4, lr} */
        0x2805,                 /* cmp r0, #5 */
        0xbf06,                 /* itte eq */
        0x2101,                 /* movs r1, #1   (eq, no flags inside IT) */
        0x2202,                 /* movs r2, #2   (eq) */
        0x2303,                 /* movs r3, #3   (ne) */
        0xbf08,                 /* it eq         (Z still from the CMP) */
        0x2507,                 /* moveq r5, #7 */
        0xb100,                 /* cbz r0, +0 (to the movw) when r0 == 0 */
        0x3001,                 /* adds r0, #1 */
        0xf241, 0x2434,         /* movw r4, #0x1234 */
        0xf2c5, 0x6478,         /* movt r4, #0x5678 */
        0xbd10,                 /* pop {r4, pc} */
    };
    memcpy(s_sram3, code, sizeof(code));
    s_cpu.r[0] = 5;
    s_cpu.r[1] = s_cpu.r[2] = s_cpu.r[3] = s_cpu.r[4] = 0xAA;
    s_cpu.r[5] = 0;
    TEST_ASSERT_EQUAL_INT(ARMCM4_OK, armcm4_call(&s_cpu, SRAM3 | 1u, STACK_TOP, 1000));
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(1, s_cpu.r[1], "IT eq: r1");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(2, s_cpu.r[2], "IT eq: r2");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xAA, s_cpu.r[3], "IT ne: r3 nezměněný");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(7, s_cpu.r[5], "MOVS v bloku IT příznaky nemění: druhé IT EQ platí");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(6, s_cpu.r[0], "CBZ neskočil, r0 + 1");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xAA, s_cpu.r[4], "POP vrátil r4");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(STACK_TOP, s_cpu.r[13], "zásobník zpátky");
}


/* Chyby: neznámá instrukce (UDF), přístup mimo paměť, limit instrukcí, skok do stavu ARM. */
void test_armcm4_faults(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static const uint16_t udf[] = { 0xde00 };                               /* udf #0 */
    static const uint16_t bus[] = { 0x2000, 0x6800 };                       /* movs r0, #0; ldr r0, [r0] */
    static const uint16_t loop[] = { 0xe7fe };                              /* b . */
    static const uint16_t arm[] = { 0x2000, 0x4700 };                       /* movs r0, #0; bx r0 */
    memcpy(s_sram3, udf, sizeof(udf));
    TEST_ASSERT_EQUAL_INT(ARMCM4_FAULT_UNDEF, armcm4_call(&s_cpu, SRAM3 | 1u, STACK_TOP, 100));
    TEST_ASSERT_EQUAL_HEX32(SRAM3, s_cpu.fault_pc);
    memcpy(s_sram3, bus, sizeof(bus));
    TEST_ASSERT_EQUAL_INT(ARMCM4_FAULT_BUS, armcm4_call(&s_cpu, SRAM3 | 1u, STACK_TOP, 100));
    TEST_ASSERT_EQUAL_HEX32(0, s_cpu.fault_addr);
    memcpy(s_sram3, loop, sizeof(loop));
    TEST_ASSERT_EQUAL_INT(ARMCM4_FAULT_LIMIT, armcm4_call(&s_cpu, SRAM3 | 1u, STACK_TOP, 100));
    memcpy(s_sram3, arm, sizeof(arm));
    TEST_ASSERT_EQUAL_INT(ARMCM4_FAULT_STATE, armcm4_call(&s_cpu, SRAM3 | 1u, STACK_TOP, 100));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();
    UNITY_BEGIN();

    RUN_TEST(test_armcm4_prog_matches_host);
    RUN_TEST(test_armcm4_it_cbz_pop);
    RUN_TEST(test_armcm4_faults);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
