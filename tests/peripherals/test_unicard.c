/*
 * test_unicard.c - unit testy pro Unicard / UNIMGR
 *
 * Pokrývá:
 *   - smoke: cmdRESET, cmdREV, cmdGETCWD
 *   - parser-equivalence: cmdOPEN s param_format "BS"
 *
 * Test komunikuje s unicard přes IORQ na portech 0x50 (CMD) a 0x51 (DATA),
 * stejně jako reálný Z80 kód. Pracovní SD root je vytvořen v dočasném
 * adresáři (g_dir_make_tmp) a po každém testu se rekurzivně smaže.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "hw-generic/unicard/unicard.h"
#include "hw-generic/unicard/unicard_sfn.h"
#include "hw-generic/unicard/unicard_ini.h"
#include "hw-generic/unicard/unimgr_commands.h"
#include "hw-generic/unicard/unimgr.h"
#include "hw-generic/unicard/unicard_tcp.h"
#include "baseui/baseui_tools.h"
#include "emulator/mzarch/mzarch_platform.h"
#include "mzarch/mzarch.h"
#include "hw-generic/devbus/devbus.h"

/* Porty unicard (interní mapování unimgr_write_byte/unimgr_read_byte). */
#define UNICARD_PORT_CMD   0x50
#define UNICARD_PORT_DATA  0x51

/* Status master bity (1. bajt statusu). */
#define UNICARD_STS_BUSY      0x01  /* PARAMRQ - čekáme parametry */
#define UNICARD_STS_DOUTRQ    0x02  /* máme výstupní data */
#define UNICARD_STS_READDIR   0x04
#define UNICARD_STS_READ      0x08
#define UNICARD_STS_WRITE     0x10
#define UNICARD_STS_EOF       0x20
#define UNICARD_STS_PROGRESS  0x40  /* IN_PROGRESS - operace na pozadí (COPY) */
#define UNICARD_STS_ERROR     0x80

/* Jméno souboru Unicard Manageru na SD kartě je per-architektura + per-FW.
 * Default FW je uc3 -> per-mzarch MGR V2.11b. uc1 (= MZ-800 only)
 * instaluje MGR V2.4 (mgr.mzf). Musí ladit s unicard_init_sd dispatch. */
#if MZARCH == 800
    #define TEST_UNICARD_MGR_MZF      UNICARD_UNIMGR_MGR800_MZF
#elif MZARCH == 700
    #define TEST_UNICARD_MGR_MZF      UNICARD_UNIMGR_MGR700_MZF
#else
    #define TEST_UNICARD_MGR_MZF      UNICARD_UNIMGR_MGR1500_MZF
#endif
#define TEST_UNICARD_MGR_MZF_PATH     "/" UNICARD_UNIMGR_DIR "/" TEST_UNICARD_MGR_MZF

/* Per-test SD root (tmpdir vytvořený v setUp, smazaný v tearDown). */
static char *s_test_sd_root = NULL;


/* Rekurzivní smazání adresáře pomocí GLib.
 * Vrací 0 při úspěchu, -1 při chybě. */
static int rmtree(const char *path)
{
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        return 0;
    }

    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        GError *err = NULL;
        GDir *d = g_dir_open(path, 0, &err);
        if (!d) {
            if (err) g_error_free(err);
            return -1;
        }
        const gchar *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            char *child = g_build_filename(path, name, NULL);
            rmtree(child);
            g_free(child);
        }
        g_dir_close(d);
        return g_rmdir(path);
    }
    return g_remove(path);
}


void setUp(void)
{
    /* Vytvořit unikátní tmpdir pro tento test. */
    GError *err = NULL;
    s_test_sd_root = g_dir_make_tmp("mztest-unicard-XXXXXX", &err);
    if (!s_test_sd_root) {
        if (err) g_error_free(err);
        TEST_FAIL_MESSAGE("Nepodařilo se vytvořit tmpdir pro SD root");
        return;
    }

    /* Po debugger-fixes-3 auto-init běží jen pokud SD root NEexistuje
     * (= chrání existující uživatelská data). g_dir_make_tmp ale tmpdir
     * vytvořil = init by se přeskočil a runtime soubory by nevznikly.
     * Reservované jméno použijeme, ale samotný adresář smažeme, ať
     * unicard_set_connected uvidí "prvotní setup" stav a provede plnou
     * inicializaci. */
    g_rmdir(s_test_sd_root);

    /* Nastavit SD root + připojit unicard. unicard_set_connected má
     * early-return pokud g_unicard.connected == conn (= no-op), takže
     * pokud cfgmain default startuje CONNECTED, nedošlo by k volání
     * unicard_init_sd (= struktura SD by nevznikla, první test fail).
     *
     * Vždy nejdřív disconnect, pak connect = guarantee re-init. */
    unicard_set_sd_root_dirpath(s_test_sd_root);
    unicard_set_connected(UNICARD_CONNECTION_DISCONNECTED);
    /* Deterministický FW pro testy = uc3 (nezávisle na per-platform defaultu
     * prvního spuštění, který je na MZ-800 uc1). uc1-specifické testy si FW
     * přepnou samy (set_fw + restore). */
    unicard_set_fw(UNICARD_FW_UC3);
    unicard_set_connected(UNICARD_CONNECTION_CONNECTED);
}


void tearDown(void)
{
    /* Odpojit unicard - zavře otevřené file/dir handly. */
    unicard_set_connected(UNICARD_CONNECTION_DISCONNECTED);
    /* Hodiny karty zase podle hostitele (cmdRTCSETD/T v testech). */
    unicard_rtc_reset();
    /* fw 0.26 (testy UC API rev 2 si ho zapínají samy). */
    unicard_set_api_rev2(0);

    /* Smazat tmpdir rekurzivně. */
    if (s_test_sd_root) {
        rmtree(s_test_sd_root);
        g_free(s_test_sd_root);
        s_test_sd_root = NULL;
    }
}


/* ================================================================
 * Helpery pro IORQ na unicard portech
 * ================================================================ */

/* Poslat command byte na CMD port (0x50). */
static void uc_send_cmd(uint8_t cmd)
{
    unicard_write_byte(UNICARD_PORT_CMD, cmd);
}

/* Poslat parametr na DATA port (0x51). */
static void uc_send_data(uint8_t data)
{
    unicard_write_byte(UNICARD_PORT_DATA, data);
}

/* Poslat string + terminátor (libovolný znak < 0x20). */
static void uc_send_str(const char *s)
{
    while (*s) {
        uc_send_data((uint8_t)*s++);
    }
    uc_send_data(0x0d); /* terminátor */
}

/* Přečíst byte z DATA portu. */
static uint8_t uc_recv_data(void)
{
    return unicard_read_byte(UNICARD_PORT_DATA);
}

/* Přečíst N-tý byte statusu (0..3).
 * Pošle cmdSTSR (resetuje sts pointer), pak N+1 čtení CMD portu. */
static uint8_t uc_read_status_byte(uint8_t idx)
{
    unicard_write_byte(UNICARD_PORT_CMD, cmdSTSR);
    uint8_t v = 0x00;
    for (uint8_t i = 0; i <= idx; i++) {
        v = unicard_read_byte(UNICARD_PORT_CMD);
    }
    return v;
}

/* Master status byte (1. bajt). */
static uint8_t uc_master_status(void)
{
    return uc_read_status_byte(0);
}


/* ================================================================
 * SMOKE TESTY
 * ================================================================ */

/* setUp/tearDown samy o sobě musí projít bez padání - SD root se
 * vytvoří, unicard se připojí, vznikne struktura /unicard/ s obsahem. */
void test_unicard_setup_creates_sd_structure(void)
{
    char *unimgr_dir = g_build_filename(s_test_sd_root, UNICARD_UNIMGR_DIR, NULL);
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(unimgr_dir, G_FILE_TEST_IS_DIR),
                             "Adresář /unicard musí existovat po set_connected");
    g_free(unimgr_dir);

    char *mgr_mzf = g_build_filename(s_test_sd_root, UNICARD_UNIMGR_DIR,
                                     TEST_UNICARD_MGR_MZF, NULL);
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(mgr_mzf, G_FILE_TEST_IS_REGULAR),
                             "Soubor " TEST_UNICARD_MGR_MZF_PATH " musí být zkopírován");
    g_free(mgr_mzf);
}


/* cmdRESET - master status musí být po RESET bez ERROR a bez BUSY/DOUTRQ. */
void test_unicard_smoke_cmdRESET(void)
{
    uc_send_cmd(cmdRESET);

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "Po cmdRESET nesmí být ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_BUSY,
                                    "Po cmdRESET nesmí být BUSY");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_DOUTRQ,
                                    "Po cmdRESET nesmí být DOUTRQ");
}


/* cmdREV - vrací firmware revision string ukončený 0x0d. */
void test_unicard_smoke_cmdREV(void)
{
    uc_send_cmd(cmdREV);

    /* Status musí mít DOUTRQ a žádný ERROR. */
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "Po cmdREV nesmí být ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_DOUTRQ, sts & UNICARD_STS_DOUTRQ,
                                    "Po cmdREV musí být DOUTRQ=1");

    /* Vyčíst revision string a ověřit, že je neprázdný a končí 0x0d. */
    char buf[64];
    size_t n = 0;
    while (n < sizeof(buf) - 1) {
        uint8_t b = uc_recv_data();
        buf[n++] = (char)b;
        if (b == 0x0d) break;
    }
    buf[n] = '\0';

    TEST_ASSERT_TRUE_MESSAGE(n >= 2, "Revision string musí mít aspoň 1 znak + 0x0d");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x0d, (uint8_t)buf[n - 1],
                                    "Revision string musí končit 0x0d");

    /* Po vyčtení DOUTRQ shozeno. */
    sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_DOUTRQ,
                                    "Po vyčtení DOUTRQ nesmí být DOUTRQ");
}


/* Pošle cmdGETCWD a ověří, že výstup (CWD string + 0x0d) == expected.
 * NEresetuje CWD - použitelné i pro test po cmdCHDIR. */
static void recv_cmdGETCWD_expect(const char *expected)
{
    uc_send_cmd(cmdGETCWD);

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "Po cmdGETCWD nesmí být ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_DOUTRQ, sts & UNICARD_STS_DOUTRQ,
                                    "Po cmdGETCWD musí být DOUTRQ=1");

    char buf[32];
    size_t n = 0;
    while (n < sizeof(buf) - 1) {
        uint8_t b = uc_recv_data();
        buf[n++] = (char)b;
        if (b == 0x0d) break;
    }
    buf[n] = '\0';

    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, buf, "CWD format mismatch");
}

/* cmdGETCWD - po RESET je CWD "/" (uc1) resp. "0:/" (uc3) + 0x0d. */
static void check_cmdGETCWD_expect(const char *expected)
{
    uc_send_cmd(cmdRESET);
    recv_cmdGETCWD_expect(expected);
}

void test_unicard_smoke_cmdGETCWD(void)
{
    /* uc3 (default): CWD s drive prefixem "0:". Dostupné pro všechny
     * mzarch. */
    unicard_set_fw(UNICARD_FW_UC3);
    check_cmdGETCWD_expect("0:/\x0d");

    /* uc1: CWD bez prefixu. Jen na MZ-800 (na jiných platformách
     * unicard_set_fw(UC1) je no-op a FW zůstane UC3). */
    if (g_mzarch_platform_numeric == 800) {
        unicard_set_fw(UNICARD_FW_UC1);
        check_cmdGETCWD_expect("/\x0d");
        /* Vrátit default pro další testy. */
        unicard_set_fw(UNICARD_FW_UC3);
    }
}


/* Vejde do /sub (cmdCHDIR) a ověří, že chdir neselhal.
 * Předpoklad: /sub na SD existuje. Cesta je absolutní, takže je odolná
 * vůči případnému resetu CWD při přepnutí FW (disconnect/reconnect). */
static void chdir_to_sub_ok(void)
{
    uc_send_cmd(cmdCHDIR);
    uc_send_str("/sub");
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "cmdCHDIR /sub nesmí ERROR");
}

/* Regrese (bugreport 0010): cmdGETCWD po vstupu do podadresáře MUSÍ vrátit
 * single-slash cestu - "0:/sub" (uc3) resp. "/sub" (uc1) - shodně s reálnou
 * Unikartou. Dřív emu path-normalizér produkoval vedoucí dvojité lomítko
 * ("0://sub" / "//sub"), což se v mzdos shellu projevovalo jako "//dir"
 * ve výpisu PWD. Bug se týkal obou FW (sdílený normalizér), oba real FW
 * (přes FatFS f_getcwd) přitom vrací single slash. */
void test_unicard_cmdGETCWD_subdir_single_slash(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Vytvořit /sub na SD (per-test tmpdir, tj. neexistuje). */
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdMKDIR);
    uc_send_str("/sub");
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "cmdMKDIR /sub nesmí ERROR");

    /* uc3 (default, všechny mzarch): "0:/sub" + CR. chdir AŽ po set_fw
     * (reconnect při přepnutí FW může resetovat CWD na "/"). */
    unicard_set_fw(UNICARD_FW_UC3);
    chdir_to_sub_ok();
    recv_cmdGETCWD_expect("0:/sub\x0d");

    /* uc1 (jen MZ-800): "/sub" + CR bez drive prefixu. */
    if (g_mzarch_platform_numeric == 800) {
        unicard_set_fw(UNICARD_FW_UC1);
        chdir_to_sub_ok();
        recv_cmdGETCWD_expect("/sub\x0d");
        /* Vrátit default pro další testy. */
        unicard_set_fw(UNICARD_FW_UC3);
    }
}


/* ================================================================
 * UNIT TESTY - parser equivalence
 * ================================================================ */

/* Parser test: cmdOPEN ("BS") na neexistující soubor. Ověřuje že
 * parser:
 *   1. načte 1 byte parametr (mode)
 *   2. načte string parametr (path) ukončený 0x0d
 *   3. dispatch do do_cmdOPEN volá unicard_file_open
 *   4. fopen selže (FR_NO_FILE) a sts_err=ERROR
 *
 * Tím nepřímo ověřujeme, že parser zvládne libovolnou kombinaci
 * B a S znaků - cmdOPEN je jediný existující dispatched příkaz s
 * mixem byte+string. */
void test_unicard_parser_BS_via_cmdOPEN(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);

    /* OPEN s mode=FA_READ na neexistující path. */
    uc_send_cmd(cmdOPEN);

    /* Po cmdOPEN musí být BUSY (čeká parametry). */
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_BUSY, sts & UNICARD_STS_BUSY,
                                    "Po cmdOPEN bez parametrů musí být BUSY");

    /* Pošleme byte parametr (mode = FA_READ = 0x01). */
    uc_send_data(0x01);

    /* Po jednom byte je parser dál v BUSY (čeká string). */
    sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_BUSY, sts & UNICARD_STS_BUSY,
                                    "Po prvním byte cmdOPEN musí být dál BUSY (čeká S)");

    /* Pošleme path - neexistující, terminátor 0x0d. */
    uc_send_str("/nonexistent_file.txt");

    /* Po dokončení parametrů: BUSY=0, ERROR=1 (fopen selhal). */
    sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_BUSY,
                                    "Po posledním parametru cmdOPEN nesmí být BUSY");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR,
                                    "OPEN neexistujícího souboru musí dát ERROR");
}


/* Parser test: cmdOPEN ("BS") na existující soubor manageru
 * (vytvořen unicard_init_sd v setUp, per-arch jméno). Otevření musí uspět. */
void test_unicard_parser_BS_open_existing(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str(TEST_UNICARD_MGR_MZF_PATH);

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "OPEN existujícího souboru nesmí dát ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_READ, sts & UNICARD_STS_READ,
                                    "Po OPEN ve FA_READ módu musí být READ_FILE bit");
}


/* ================================================================
 * UNIT TESTY - Fáze 1: Read-only metadata
 * ================================================================ */

/* Helper: vyčte 4B DWORD LE z DATA portu. */
static uint32_t uc_recv_dword_le(void)
{
    uint32_t v = 0;
    v |= (uint32_t)uc_recv_data();
    v |= (uint32_t)uc_recv_data() << 8;
    v |= (uint32_t)uc_recv_data() << 16;
    v |= (uint32_t)uc_recv_data() << 24;
    return v;
}

/* Helper: vyčte 2B WORD LE. */
static uint16_t uc_recv_word_le(void)
{
    uint16_t v = 0;
    v |= (uint16_t)uc_recv_data();
    v |= (uint16_t)uc_recv_data() << 8;
    return v;
}

/* Helper: vytvoří test soubor v SD root o dané velikosti, naplněný
 * sekvencí 0..255 cyklicky. Cesta v SD root, ne emulátorová. */
static void create_test_file(const char *rel_path, size_t size)
{
    char *full = g_build_filename(s_test_sd_root, rel_path, NULL);
    FILE *f = g_fopen(full, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Nepodařilo se vytvořit testovací soubor");
    for (size_t i = 0; i < size; i++) {
        uint8_t b = (uint8_t)(i & 0xff);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    g_free(full);
}


/* cmdSIZE - po OPEN existujícího souboru vrátí jeho přesnou velikost. */
void test_unicard_cmdSIZE(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("test_size.bin", 1234);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str("/test_size.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "OPEN test_size.bin nesmí dát ERROR");

    uc_send_cmd(cmdSIZE);
    sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_DOUTRQ, sts & UNICARD_STS_DOUTRQ,
                                    "Po cmdSIZE musí být DOUTRQ=1");

    uint32_t size = uc_recv_dword_le();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1234, size, "cmdSIZE musí vrátit 1234");
}


/* cmdTELL - po OPEN je TELL=0; po několika čteních se posune. */
void test_unicard_cmdTELL(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("test_tell.bin", 100);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);
    uc_send_str("/test_tell.bin");

    /* Po OPEN: TELL = 0. */
    uc_send_cmd(cmdTELL);
    uint32_t pos = uc_recv_dword_le();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pos, "TELL po OPEN musí být 0");

    /* Přečíst 7 bytů přes data port (cmdINTGETC implicit), pak TELL=7. */
    for (int i = 0; i < 7; i++) (void)uc_recv_data();

    uc_send_cmd(cmdTELL);
    pos = uc_recv_dword_le();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(7, pos, "TELL po 7 čteních musí být 7");
}


/* cmdSTAT - na existující soubor vrací FILINFO s odpovídající fsize a
 * lfname rovným basename. */
void test_unicard_cmdSTAT_file(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("stat_target.bin", 4242);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdSTAT);

    /* cmdSTAT čeká na string - po cmd musí být BUSY. */
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_BUSY, sts & UNICARD_STS_BUSY,
                                    "cmdSTAT musí čekat na string parametr");

    uc_send_str("/stat_target.bin");

    sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "STAT existujícího souboru nesmí ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_DOUTRQ, sts & UNICARD_STS_DOUTRQ,
                                    "STAT musí dát DOUTRQ");

    /* Rozparsovat FILINFO výstup. */
    uint32_t fsize = uc_recv_dword_le();
    uint16_t fdate = uc_recv_word_le();
    uint16_t ftime = uc_recv_word_le();
    uint8_t fattrib = uc_recv_data();

    char fname[13];
    for (int i = 0; i < 13; i++) fname[i] = (char)uc_recv_data();

    uint8_t lfn_strlen = uc_recv_data();

    char lfname[33]; /* _MAX_LFN = 32 */
    for (int i = 0; i < 32; i++) lfname[i] = (char)uc_recv_data();
    lfname[32] = '\0';

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4242, fsize, "fsize musí být 4242");
    TEST_ASSERT_TRUE_MESSAGE(fdate != 0, "fdate musí být nenulové (mtime)");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, fattrib & UNICARD_AM_DIR,
                                    "fattrib pro soubor nesmí mít AM_DIR");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("stat_target.bin", lfname,
                                     "lfname musí být basename");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(strlen("stat_target.bin"), lfn_strlen,
                                     "lfn_strlen musí odpovídat délce basename");

    /* Suppress unused warning. */
    (void)ftime;
    (void)fname;
}


/* cmdSTAT - na adresář /unicard musí mít AM_DIR atribut. */
void test_unicard_cmdSTAT_dir(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdSTAT);
    uc_send_str("/unicard");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "STAT /unicard nesmí ERROR");

    /* Skip prvních 8 bytů (fsize + fdate + ftime), čtu fattrib. */
    for (int i = 0; i < 8; i++) (void)uc_recv_data();
    uint8_t fattrib = uc_recv_data();

    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_AM_DIR, fattrib & UNICARD_AM_DIR,
                                    "STAT /unicard musí mít AM_DIR");

    /* Zbytek (zatím nedůležitý) jen vyčíst aby uvolnil DOUTRQ. */
    int remaining = (23 + _MAX_LFN) - 8 - 1;
    for (int i = 0; i < remaining; i++) (void)uc_recv_data();
}


/* cmdSTAT na neexistující path - ERROR + ff_res = FR_NO_FILE. */
void test_unicard_cmdSTAT_missing(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdSTAT);
    uc_send_str("/does_not_exist.xyz");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR,
                                    "STAT na neexistující path musí ERROR");

    /* 4. status byte = ff_res. */
    uint8_t ff_res = uc_read_status_byte(3);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(FR_NO_FILE, ff_res,
                                    "ff_res po STAT missing musí být FR_NO_FILE");
}


/* cmdGETFREE - vrátí 8 bytů (total + free), oba > 0, free <= total. */
void test_unicard_cmdGETFREE(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdGETFREE);

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "cmdGETFREE nesmí ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_DOUTRQ, sts & UNICARD_STS_DOUTRQ,
                                    "cmdGETFREE musí dát DOUTRQ");

    uint32_t total = uc_recv_dword_le();
    uint32_t freev = uc_recv_dword_le();

    TEST_ASSERT_TRUE_MESSAGE(total > 0, "total_sect musí být > 0");
    TEST_ASSERT_TRUE_MESSAGE(freev > 0, "free_sect musí být > 0");
    TEST_ASSERT_TRUE_MESSAGE(freev <= total, "free_sect musí být <= total_sect");
}


/* ================================================================
 * UNIT TESTY - Fáze 2: Seek
 * ================================================================ */

/* Helper: pošle cmdSEEK s daným módem a offsetem (BBBBB). */
static void uc_seek(uint8_t mode, uint32_t offset)
{
    uc_send_cmd(cmdSEEK);
    uc_send_data(mode);
    uc_send_data((uint8_t)(offset & 0xff));
    uc_send_data((uint8_t)((offset >> 8) & 0xff));
    uc_send_data((uint8_t)((offset >> 16) & 0xff));
    uc_send_data((uint8_t)((offset >> 24) & 0xff));
}

/* Helper: vrátí TELL pozici (předpokládá otevřený soubor). */
static uint32_t uc_tell(void)
{
    uc_send_cmd(cmdTELL);
    return uc_recv_dword_le();
}


/* cmdSEEK mode 0 (SET) - absolutní pozice. */
void test_unicard_cmdSEEK_set(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("seek_set.bin", 1000);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);
    uc_send_str("/seek_set.bin");

    uc_seek(0, 100);
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "SEEK SET 100 nesmí ERROR");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(100, uc_tell(),
                                     "Po SEEK SET 100 musí být TELL=100");
}


/* cmdSEEK mode 1 (END) - vzdálenost OD KONCE. */
void test_unicard_cmdSEEK_end(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("seek_end.bin", 500);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);
    uc_send_str("/seek_end.bin");

    /* mode 1, offset 0 → konec souboru (= 500). */
    uc_seek(1, 0);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(500, uc_tell(),
                                     "SEEK END 0 musí dát TELL=size");

    /* mode 1, offset 50 → 50 bytů PŘED koncem (= 450). */
    uc_seek(1, 50);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(450, uc_tell(),
                                     "SEEK END 50 musí dát TELL=450");
}


/* cmdSEEK mode 2 (REL_UP) a mode 3 (REL_DOWN) - relativní posun. */
void test_unicard_cmdSEEK_rel(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("seek_rel.bin", 1000);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);
    uc_send_str("/seek_rel.bin");

    uc_seek(0, 200);                     /* TELL=200 */
    TEST_ASSERT_EQUAL_UINT32(200, uc_tell());

    uc_seek(2, 50);                      /* +50 → TELL=250 */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(250, uc_tell(),
                                     "REL_UP +50 musí dát TELL=250");

    uc_seek(3, 30);                      /* -30 → TELL=220 */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(220, uc_tell(),
                                     "REL_DOWN -30 musí dát TELL=220");
}


/* cmdSEEK na zavřený soubor → ERROR. */
void test_unicard_cmdSEEK_no_file(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_seek(0, 100);

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR,
                                    "SEEK bez otevřeného souboru musí ERROR");
}


/* ================================================================
 * UNIT TESTY - Fáze 3: File WRITE
 * ================================================================ */

/* Pomocný helper: zavře otevřený soubor (cmdCLOSE). */
static void uc_close(void)
{
    uc_send_cmd(cmdCLOSE);
}


/* Stav bit 5 (EOF) jako firmware: hned jak pozice dojde na velikost
 * souboru, ne až po čtení za konec; prázdný soubor hned po OPEN. */
void test_unicard_status_eof_at_size(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("eof3.bin", 3);
    create_test_file("eof0.bin", 0);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);                         /* FA_READ */
    uc_send_str("eof3.bin");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_EOF, "po OPEN bez EOF");
    uc_recv_data();
    uc_recv_data();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_EOF, "po 2 z 3 B bez EOF");
    uc_recv_data();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_EOF, uc_master_status() & UNICARD_STS_EOF, "po 3 B EOF");
    uc_seek(0, 1);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_EOF, "SEEK zpět: bez EOF");
    uc_close();

    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);
    uc_send_str("eof0.bin");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_EOF, uc_master_status() & UNICARD_STS_EOF, "prázdný soubor: EOF hned");
    uc_close();
}


/* uc3: složka cesty delší než 32 znaků = FR_INVALID_NAME jako FatFS karty
 * (FF_MAX_LFN 32, u každé operace s cestou); 32 znaků projde. uc1 ne. */
void test_unicard_uc3_long_name_invalid(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    const char *n32 = "/abcdefghijklmnopqrstuvwxyz012345";      /* 32 znaků */
    const char *n33 = "/abcdefghijklmnopqrstuvwxyz0123456";     /* 33 znaků */
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x0a);                         /* FA_WRITE | FA_CREATE_ALWAYS */
    uc_send_str(n32);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "32 znaků = OK");
    uc_close();
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x0a);
    uc_send_str(n33);
    TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_ERROR, "33 znaků = chyba");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_INVALID_NAME, uc_read_status_byte(3), "33 znaků = FR_INVALID_NAME");
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(g_build_filename(s_test_sd_root, n33 + 1, NULL), G_FILE_TEST_EXISTS),
                              "soubor 33 znaků nevznikl");
    uc_send_cmd(cmdMKDIR);
    uc_send_str("/dir/abcdefghijklmnopqrstuvwxyz0123456");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_INVALID_NAME, uc_read_status_byte(3), "MKDIR: složka 33 znaků");
    uc_send_cmd(cmdCHDIR);
    uc_send_str("/abcdefghijklmnopqrstuvwxyz0123456/x");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_INVALID_NAME, uc_read_status_byte(3), "CHDIR: první složka 33 znaků");

    if (g_mzarch_platform_numeric != 800) return;
    unicard_set_fw(UNICARD_FW_UC1);                 /* uc1 nekontroluje */
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x0a);
    uc_send_str(n33);
    uint8_t sts = uc_master_status();
    uc_close();
    unicard_set_fw(UNICARD_FW_UC3);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR, "uc1: 33 znaků projde");
}


/* DNS jako lwIP firmwaru: číselná adresa hned RESOLVED, jméno napřed jednou
 * RESOLVING (karta se ptá serveru), pak výsledek. */
void test_unicard_dns_resolving(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t r[5];
    unicard_dns_request("127.0.0.1");
    unicard_dns_result(r);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UNICARD_DNS_RESOLVED, r[0], "číselná adresa hned");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(127, r[1], "127.0.0.1");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, r[4], "127.0.0.1");

    unicard_dns_request("localhost");
    unicard_dns_result(r);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UNICARD_DNS_RESOLVING, r[0], "jméno: napřed RESOLVING");
    unicard_dns_result(r);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UNICARD_DNS_RESOLVED, r[0], "pak výsledek (localhost)");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(127, r[1], "localhost = 127.x");

    unicard_dns_request("");
    unicard_dns_result(r);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UNICARD_DNS_BAD_ARGS, r[0], "prázdné jméno");
}


/* Čekací stavy přidané od posledního volání (a vynulování). */
static uint32_t uc_waited(void)
{
    uint32_t w = g_mzarch_main.instruction_wait_tstates;
    g_mzarch_main.instruction_wait_tstates = 0;
    return w;
}

/* Čekací stavy jedné verze uc3: rychlá cesta (čtení otevřeného souboru, STSR, RKEY), hlavní smyčka
 * (stav, příkaz, výstup příkazu, zápis dat, soubor s převodem SharpASCII), přístup debuggeru nic. */
static void uc3_wait_check(int rev2, uint32_t status_read, uint32_t data_read, uint32_t file_read,
                           uint32_t write_fast, uint32_t write)
{
    unicard_set_api_rev2(rev2);
    create_test_file("wait.bin", 4);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);
    uc_send_str("wait.bin");
    uc_waited();
    uc_recv_data();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(file_read, uc_waited(), "čtení otevřeného souboru: rychlá cesta");
    unicard_read_byte(UNICARD_PORT_CMD);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(status_read, uc_waited(), "čtení stavu: hlavní smyčka");
    uc_send_cmd(cmdSTSR);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(write_fast, uc_waited(), "STSR: rychlá cesta");
    devbus_debugger_access_begin();
    uc_recv_data();
    unicard_read_byte(UNICARD_PORT_CMD);
    devbus_debugger_access_end();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, uc_waited(), "přístup debuggeru bez čekání");
    uc_send_cmd(cmdREVD);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(write, uc_waited(), "příkaz: hlavní smyčka");
    uc_recv_data();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(data_read, uc_waited(), "výstup příkazu: hlavní smyčka");
    uc_send_data(0x00);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(write, uc_waited(), "zápis dat: hlavní smyčka");
    uc_close();
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01 | 0x20);
    uc_send_str("wait.bin");
    uc_waited();
    uc_recv_data();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(data_read, uc_waited(), "soubor s převodem SharpASCII: hlavní smyčka");
    uc_close();
    uc_waited();
}

/* Čekací stavy karty uc3 (změřené programem IOWAIT): v0.26 podle fw 0.23, v0.37 podle fw 0.38b1;
 * v0.37 RKEY rychle (zápis i čtení kódu). */
void test_unicard_uc3_wait_states(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc3_wait_check(0, UNICARD_UC3_026_WAIT_STATUS_READ, UNICARD_UC3_026_WAIT_DATA_READ,
                   UNICARD_UC3_026_WAIT_FILE_READ, UNICARD_UC3_026_WAIT_WRITE_FAST, UNICARD_UC3_026_WAIT_WRITE);
    uc3_wait_check(1, UNICARD_UC3_037_WAIT_STATUS_READ, UNICARD_UC3_037_WAIT_DATA_READ,
                   UNICARD_UC3_037_WAIT_FILE_READ, UNICARD_UC3_037_WAIT_WRITE_FAST, UNICARD_UC3_037_WAIT_WRITE);
    uc_send_cmd(cmdRESET);
    uc_waited();
    uc_send_cmd(cmdRKEY);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(UNICARD_UC3_037_WAIT_WRITE_FAST, uc_waited(), "v0.37 RKEY: rychlá cesta");
    uc_recv_data();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(UNICARD_UC3_037_WAIT_FILE_READ, uc_waited(), "kód RKEY: rychlá cesta");
}


/* Složka cloudu z INI cloud_dir, když proměnná prostředí chybí; proměnná má přednost. */
void test_unicard_cloud_dir_ini_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t info[16];
    g_unsetenv("MZ800EMU_UNICARD_CLOUD");
    unicard_set_api_rev2(1);
    unicard_set_cloud_dir(s_test_sd_root);          /* existující složka */
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdINFO);
    for (int i = 0; i < 16; i++) info[i] = uc_recv_data();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01, info[3] & 0x01, "cloud_dir: cloud dostupný");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(s_test_sd_root, unicard_cloud_root(), "cesta z INI");

    char *missing = g_build_filename(s_test_sd_root, "missing", NULL);
    g_setenv("MZ800EMU_UNICARD_CLOUD", missing, TRUE);
    uc_send_cmd(cmdINFO);
    for (int i = 0; i < 16; i++) info[i] = uc_recv_data();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, info[3] & 0x01, "proměnná prostředí má přednost");
    g_unsetenv("MZ800EMU_UNICARD_CLOUD");
    g_free(missing);

    unicard_set_cloud_dir("");
    TEST_ASSERT_NULL_MESSAGE(unicard_cloud_root(), "prázdné = bez cloudu");
    unicard_set_rkey_file("");
    TEST_ASSERT_NULL_MESSAGE(unicard_rkey_file(), "prázdné = bez kláves");
}


/* Read full file content from host FS pro verifikaci. */
static size_t read_file_content(const char *rel_path, uint8_t *buf, size_t cap)
{
    char *full = g_build_filename(s_test_sd_root, rel_path, NULL);
    FILE *f = g_fopen(full, "rb");
    g_free(full);
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}


/* Write workflow: vytvořit soubor přes FA_CREATE_NEW|FA_WRITE, zapsat
 * 100 bytů přes cmdINTPUTC, zavřít, znovu otevřít READ a ověřit. */
void test_unicard_write_create_new(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);

    /* OPEN s FA_CREATE_NEW (0x04) | FA_WRITE (0x02) = 0x06 */
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x06);
    uc_send_str("/write_test.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "OPEN CREATE_NEW|WRITE nesmí ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_WRITE, sts & UNICARD_STS_WRITE,
                                    "Po OPEN ve WRITE módu musí být WRITE_FILE bit");

    /* Zapsat 100 bytů přes cmdINTPUTC (zápis na DATA portu). */
    for (int i = 0; i < 100; i++) {
        uc_send_data((uint8_t)(i & 0xff));
    }

    uc_close();

    /* Verifikace - přečíst host FS. */
    uint8_t buf[256];
    size_t n = read_file_content("write_test.bin", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(100, n,
                                     "Soubor musí mít 100 bytů");
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL_UINT8(i & 0xff, buf[i]);
    }
}


/* FA_WRITE samotné → open existing for write (mapping na "r+b").
 * Po fix v unicard_file_open() už nesmí vracet FR_INVALID_PARAMETER. */
void test_unicard_write_existing_FA_WRITE(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("rewrite.bin", 50); /* obsah 0..49 */

    uc_send_cmd(cmdRESET);

    /* OPEN s pouze FA_WRITE (0x02). Po fix musí uspět. */
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x02);
    uc_send_str("/rewrite.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "OPEN FA_WRITE existujícího nesmí ERROR");

    /* Přepsat prvních 10 bytů hodnotou 0xFF. */
    for (int i = 0; i < 10; i++) {
        uc_send_data(0xFF);
    }

    uc_close();

    /* Verifikace - prvních 10 = 0xFF, zbytek beze změny (10..49). */
    uint8_t buf[100];
    size_t n = read_file_content("rewrite.bin", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(50, n, "Soubor musí mít původní 50 bytů");
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, buf[i]);
    }
    for (int i = 10; i < 50; i++) {
        TEST_ASSERT_EQUAL_HEX8(i & 0xff, buf[i]);
    }
}


/* FA_CREATE_ALWAYS na neexistující soubor → vznikne nový s daty.
 * Regresní test pro bug: chyběl FA_CREATE_ALWAYS branch v
 * unicard_file_open(), mode 0x0A padalo do else větve "r+b" a fopen
 * selhal s "Cant open ... in mode 'r+b'" (file not exists). */
void test_unicard_write_FA_CREATE_ALWAYS_new(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);

    /* OPEN s FA_CREATE_ALWAYS (0x08) | FA_WRITE (0x02) = 0x0A. */
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x0A);
    uc_send_str("/create_always_new.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "OPEN FA_CREATE_ALWAYS na neexistující nesmí ERROR");

    for (int i = 0; i < 30; i++) {
        uc_send_data((uint8_t)(i & 0xff));
    }
    uc_close();

    uint8_t buf[100];
    size_t n = read_file_content("create_always_new.bin", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(30, n, "Soubor musí mít 30 bytů");
    for (int i = 0; i < 30; i++) {
        TEST_ASSERT_EQUAL_UINT8(i & 0xff, buf[i]);
    }
}


/* FA_CREATE_ALWAYS na existující soubor → truncate na 0 a přepsat.
 * Sémantika dle FatFS: vždy začít s prázdným obsahem. */
void test_unicard_write_FA_CREATE_ALWAYS_truncate(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("create_always_trunc.bin", 100); /* obsah 0..99 */

    uc_send_cmd(cmdRESET);

    uc_send_cmd(cmdOPEN);
    uc_send_data(0x0A); /* FA_CREATE_ALWAYS | FA_WRITE */
    uc_send_str("/create_always_trunc.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "OPEN FA_CREATE_ALWAYS na existující nesmí ERROR");

    /* Zapsat jen 5 bytů. */
    for (int i = 0; i < 5; i++) {
        uc_send_data(0xAB);
    }
    uc_close();

    /* Soubor musí mít přesně 5 bytů (= truncated, ne 100). */
    uint8_t buf[200];
    size_t n = read_file_content("create_always_trunc.bin", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(5, n,
                                     "FA_CREATE_ALWAYS musí soubor zkrátit na 5 bytů");
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xAB, buf[i]);
    }
}


/* FA_OPEN_ALWAYS na existující soubor → zachovat obsah, dovolit přepis
 * od začátku (= ne truncate). Regresní test pro bug: před fix mapoval
 * FA_OPEN_ALWAYS na "wb" / "w+b" (truncate), což porušovalo FatFS
 * sémantiku (open existing or create new bez truncate). */
void test_unicard_write_FA_OPEN_ALWAYS_existing_preserves(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("open_always_exist.bin", 100); /* obsah 0..99 */

    uc_send_cmd(cmdRESET);

    /* OPEN s FA_OPEN_ALWAYS (0x10) | FA_WRITE (0x02) = 0x12. */
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x12);
    uc_send_str("/open_always_exist.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "OPEN FA_OPEN_ALWAYS na existující nesmí ERROR");

    /* Přepsat prvních 10 bytů hodnotou 0xCC. */
    for (int i = 0; i < 10; i++) {
        uc_send_data(0xCC);
    }
    uc_close();

    /* Soubor musí mít stále 100 bytů (= ne truncate); prvních 10 = 0xCC,
     * zbytek (10..99) musí být beze změny. */
    uint8_t buf[200];
    size_t n = read_file_content("open_always_exist.bin", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(100, n,
                                     "FA_OPEN_ALWAYS NESMÍ zkrátit existující soubor");
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xCC, buf[i]);
    }
    for (int i = 10; i < 100; i++) {
        TEST_ASSERT_EQUAL_HEX8(i & 0xff, buf[i]);
    }
}


/* FA_OPEN_ALWAYS na neexistující soubor → vytvořit nový. */
void test_unicard_write_FA_OPEN_ALWAYS_creates_new(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);

    uc_send_cmd(cmdOPEN);
    uc_send_data(0x12); /* FA_OPEN_ALWAYS | FA_WRITE */
    uc_send_str("/open_always_new.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "OPEN FA_OPEN_ALWAYS na neexistující nesmí ERROR");

    for (int i = 0; i < 7; i++) {
        uc_send_data(0x55);
    }
    uc_close();

    uint8_t buf[50];
    size_t n = read_file_content("open_always_new.bin", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(7, n, "Soubor musí mít 7 bytů");
    for (int i = 0; i < 7; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x55, buf[i]);
    }
}


/* cmdTRUNC - zkrátit soubor na aktuální pozici. */
void test_unicard_cmdTRUNC(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("trunc_test.bin", 100);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x02); /* FA_WRITE */
    uc_send_str("/trunc_test.bin");

    /* SEEK na 50, TRUNC. */
    uc_seek(0, 50);
    uc_send_cmd(cmdTRUNC);

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "cmdTRUNC nesmí ERROR");

    uc_close();

    /* Soubor musí mít 50 bytů. */
    uint8_t buf[200];
    size_t n = read_file_content("trunc_test.bin", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(50, n,
                                     "Po TRUNC na pozici 50 musí mít soubor 50 B");
}


/* cmdSYNC - smoke test, nesmí padnout. */
void test_unicard_cmdSYNC(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);

    uc_send_cmd(cmdOPEN);
    uc_send_data(0x06); /* FA_CREATE_NEW | FA_WRITE */
    uc_send_str("/sync_test.bin");

    /* Zapsat 5 bytů. */
    for (int i = 0; i < 5; i++) uc_send_data((uint8_t)i);

    uc_send_cmd(cmdSYNC);
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "cmdSYNC po zápisu nesmí ERROR");

    uc_close();
}


/* ================================================================
 * UNIT TESTY - Fáze 4: cmdMKDIR / cmdUNLINK / cmdRENAME
 * ================================================================ */

/* cmdMKDIR - vytvoření adresáře. */
void test_unicard_cmdMKDIR(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdMKDIR);
    uc_send_str("/newdir");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "cmdMKDIR /newdir nesmí ERROR");

    /* Verifikace na host FS. */
    char *full = g_build_filename(s_test_sd_root, "newdir", NULL);
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(full, G_FILE_TEST_IS_DIR),
                             "Adresář /newdir musí existovat");
    g_free(full);
}


/* cmdMKDIR na existující path → ERROR (FR_EXIST). */
void test_unicard_cmdMKDIR_exists(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdMKDIR);
    uc_send_str("/unicard"); /* už existuje z setUp */

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR,
                                    "MKDIR existujícího musí ERROR");

    uint8_t ff_res = uc_read_status_byte(3);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(FR_EXIST, ff_res,
                                    "ff_res musí být FR_EXIST");
}


/* cmdUNLINK - smazání souboru. */
void test_unicard_cmdUNLINK_file(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("to_delete.bin", 50);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdUNLINK);
    uc_send_str("/to_delete.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "UNLINK existujícího nesmí ERROR");

    char *full = g_build_filename(s_test_sd_root, "to_delete.bin", NULL);
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(full, G_FILE_TEST_EXISTS),
                              "Soubor po UNLINK nesmí existovat");
    g_free(full);
}


/* cmdUNLINK na neexistující → ERROR (FR_NO_FILE). */
void test_unicard_cmdUNLINK_missing(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdUNLINK);
    uc_send_str("/does_not_exist.txt");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR,
                                    "UNLINK na missing musí ERROR");

    uint8_t ff_res = uc_read_status_byte(3);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(FR_NO_FILE, ff_res,
                                    "ff_res musí být FR_NO_FILE");
}


/* cmdRENAME - parser SS, přejmenování souboru. */
void test_unicard_cmdRENAME(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("old_name.bin", 30);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdRENAME);

    /* První S: old_name. */
    uc_send_str("/old_name.bin");
    /* Po prvním stringu musí být dál BUSY (čeká druhý S). */
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_BUSY, sts & UNICARD_STS_BUSY,
                                    "Mezi S a S musí být dál BUSY");

    /* Druhý S: new_name. */
    uc_send_str("/new_name.bin");

    sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "RENAME nesmí ERROR");

    char *old_full = g_build_filename(s_test_sd_root, "old_name.bin", NULL);
    char *new_full = g_build_filename(s_test_sd_root, "new_name.bin", NULL);
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(old_full, G_FILE_TEST_EXISTS),
                              "Starý název nesmí existovat");
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(new_full, G_FILE_TEST_EXISTS),
                             "Nový název musí existovat");
    g_free(old_full);
    g_free(new_full);
}


/* ================================================================
 * UNIT TESTY - Fáze 5: cmdCHMOD / cmdUTIME
 * ================================================================ */

/* cmdCHMOD - no-op, ale musí prošlo OK na existující path. */
void test_unicard_cmdCHMOD_noop(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("chmod_target.bin", 10);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdCHMOD);
    uc_send_data(0x01);  /* attr = AM_RDO */
    uc_send_data(0x21);  /* mask = AM_RDO | AM_ARC */
    uc_send_str("/chmod_target.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "cmdCHMOD na existující path nesmí ERROR");
}


/* cmdCHMOD na neexistující → ERROR FR_NO_FILE. */
void test_unicard_cmdCHMOD_missing(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdCHMOD);
    uc_send_data(0x00);
    uc_send_data(0x00);
    uc_send_str("/missing.txt");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR,
                                    "CHMOD na missing musí ERROR");
}


/* cmdUTIME - nastavení známé časové značky a verifikace přes cmdSTAT. */
void test_unicard_cmdUTIME(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("utime_target.bin", 5);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdUTIME);
    uc_send_data(15);   /* D */
    uc_send_data(7);    /* M */
    uc_send_data(40);   /* Y-1980 = 2020 */
    uc_send_data(12);   /* H */
    uc_send_data(30);   /* M */
    uc_send_data(20);   /* S */
    uc_send_str("/utime_target.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "cmdUTIME nesmí ERROR");

    /* Ověření přes cmdSTAT - fdate musí odpovídat 2020-07-15. */
    uc_send_cmd(cmdSTAT);
    uc_send_str("/utime_target.bin");

    /* Skip fsize. */
    for (int i = 0; i < 4; i++) (void)uc_recv_data();
    uint16_t fdate = uc_recv_word_le();
    uint16_t ftime = uc_recv_word_le();

    /* FAT date: (year-1980)<<9 | month<<5 | day = (40<<9)|(7<<5)|15 = 0x50EF */
    uint16_t expected_date = (40 << 9) | (7 << 5) | 15;
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(expected_date, fdate,
                                     "fdate musí odpovídat 2020-07-15");

    /* FAT time: hour<<11 | min<<5 | sec/2 = (12<<11)|(30<<5)|10 = 0x63CA */
    uint16_t expected_time = (12 << 11) | (30 << 5) | (20 / 2);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(expected_time, ftime,
                                     "ftime musí odpovídat 12:30:20");

    /* Vyčíst zbytek FILINFO aby se uvolnil DOUTRQ. */
    int remaining = (23 + _MAX_LFN) - 8;
    for (int i = 0; i < remaining; i++) (void)uc_recv_data();
}


/* cmdUTIME na neexistující → ERROR. */
void test_unicard_cmdUTIME_missing(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdUTIME);
    uc_send_data(1);
    uc_send_data(1);
    uc_send_data(40);
    uc_send_data(0);
    uc_send_data(0);
    uc_send_data(0);
    uc_send_str("/missing.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR,
                                    "UTIME na missing musí ERROR");
}


/* ================================================================
 * UNIT TESTY - Fáze 6: cmdREADDIR (binární FILINFO output)
 * ================================================================ */

/* Helper: vyčte kompletní FILINFO (23+_MAX_LFN bytů) z DATA portu
 * a uloží lfname do out_name. Vrátí 1 pokud načteno, 0 pokud konec. */
static int uc_read_dir_filinfo(uint32_t *fsize, uint8_t *fattrib, char *out_name, size_t out_cap)
{
    /* Pokud máme READDIR_DATA bit, máme co číst. */
    uint8_t sts = uc_master_status();
    if (!(sts & UNICARD_STS_READDIR)) {
        return 0;
    }

    *fsize = uc_recv_dword_le();
    (void)uc_recv_word_le(); /* fdate */
    (void)uc_recv_word_le(); /* ftime */
    *fattrib = uc_recv_data();

    /* fname[13] = S (8.3 short), offset 9 */
    char sbuf[13];
    for (int i = 0; i < 13; i++) sbuf[i] = (char)uc_recv_data();
    sbuf[12] = '\0';

    uint8_t lfn_strlen = uc_recv_data();

    /* lfname[_MAX_LFN] = L (LFN), offset 23 */
    char buf[_MAX_LFN];
    for (int i = 0; i < _MAX_LFN; i++) buf[i] = (char)uc_recv_data();

    /* Stejné pravidlo jako mzdos DIR: L když LEN>0, jinak fallback na S
     * (8.3 short). Reálné HW i emu plní L jen pro jména s LFN entry. */
    if (out_name && out_cap > 0) {
        const char *src = (lfn_strlen > 0) ? buf : sbuf;
        size_t copy = strlen(src);
        if (copy > out_cap - 1) copy = out_cap - 1;
        memcpy(out_name, src, copy);
        out_name[copy] = '\0';
    }
    return 1;
}


/* cmdREADDIR - vyčte všechny položky /unicard/, ověří přítomnost
 * známých souborů (mzfloader.mzq, manager MZF, mzfloader.cfg). */
void test_unicard_cmdREADDIR(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdREADDIR);
    uc_send_str("/" UNICARD_UNIMGR_DIR);

    int found_mgr = 0, found_mzq = 0, found_cfg = 0;

    /* Vyčíst všechny položky, dokud READDIR_DATA bit zhasne. */
    for (int iter = 0; iter < 100; iter++) {
        uint32_t fsize;
        uint8_t fattrib;
        char name[40];
        if (!uc_read_dir_filinfo(&fsize, &fattrib, name, sizeof(name))) break;

        /* Case-insensitive: u jmen bez LFN je fallback na S (8.3), které
         * je v uc3 uppercase (altname). */
        if (g_ascii_strcasecmp(name, TEST_UNICARD_MGR_MZF) == 0) found_mgr = 1;
        else if (g_ascii_strcasecmp(name, UNICARD_UNIMGR_MZFLOADER_MZQ) == 0) found_mzq = 1;
        else if (g_ascii_strcasecmp(name, UNICARD_UNIMGR_MZFLOADER_CFG) == 0) found_cfg = 1;
    }

    TEST_ASSERT_TRUE_MESSAGE(found_mgr, "READDIR musí najít " TEST_UNICARD_MGR_MZF);
    TEST_ASSERT_TRUE_MESSAGE(found_mzq, "READDIR musí najít " UNICARD_UNIMGR_MZFLOADER_MZQ);
    TEST_ASSERT_TRUE_MESSAGE(found_cfg, "READDIR musí najít " UNICARD_UNIMGR_MZFLOADER_CFG);
}


/* cmdREADDIR rozliší soubor od adresáře přes AM_DIR atribut. */
void test_unicard_cmdREADDIR_dir_attribute(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Vytvořit subdir + soubor v root. */
    char *subdir_full = g_build_filename(s_test_sd_root, "subdir1", NULL);
    g_mkdir(subdir_full, 0755);
    g_free(subdir_full);
    create_test_file("file1.bin", 7);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdREADDIR);
    uc_send_str("/");

    int subdir_is_dir = -1, file_is_dir = -1;

    for (int iter = 0; iter < 50; iter++) {
        uint32_t fsize;
        uint8_t fattrib;
        char name[40];
        if (!uc_read_dir_filinfo(&fsize, &fattrib, name, sizeof(name))) break;

        /* Case-insensitive: bez LFN fallback na S (8.3), v uc3 uppercase. */
        if (g_ascii_strcasecmp(name, "subdir1") == 0) subdir_is_dir = (fattrib & UNICARD_AM_DIR) ? 1 : 0;
        else if (g_ascii_strcasecmp(name, "file1.bin") == 0) file_is_dir = (fattrib & UNICARD_AM_DIR) ? 1 : 0;
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, subdir_is_dir,
                                  "subdir1 musí mít AM_DIR=1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, file_is_dir,
                                  "file1.bin musí mít AM_DIR=0");
}


/* ================================================================
 * UNIT TESTY - SD jail (directory traversal protection)
 *
 * Pokrývá nový helper unicard_jail_resolve() a unicard_jail_resolve_with_naked().
 * Cílem je ověřit, že emu-supplied cesty nemůžou opustit SD root - ".." segmenty,
 * absolutní cesty mimo SD root a Windows-style "..\\" sekvence se musí
 * detekovat a operace dostane NULL/FR_NO_PATH.
 *
 * Pre-podmínka: work_dir je "/" (= po cmdRESET). Naked path == relativní
 * k SD root, plný path se sestaví join se s_test_sd_root.
 * ================================================================ */

/* Helper: ověří že 'full' je under SD_root (= začíná SD_rootem) a
 * obsahuje očekávaný basename (post-normalizace).
 *
 * Pozn.: separátor v plné cestě je platform-dependent (na Windows mix
 * "/" a "\\" podle glib build), proto kontrolujeme jen že basename
 * komponenta se v cestě objevuje. */
static void assert_full_under_sd_root(const char *full, const char *expected_basename,
                                      const char *msg)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(full, msg);
    /* Plný path musí začínat SD_root prefixem - to je security invariant. */
    TEST_ASSERT_TRUE_MESSAGE(g_str_has_prefix(full, s_test_sd_root),
                             "Plný path musí být uvnitř SD_root prefix");
    /* A obsahovat očekávaný basename (po normalizaci). */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(full, expected_basename), msg);
}


/* Bezpečné cesty uvnitř SD root - jail musí vrátit non-NULL plný path
 * začínající SD_root prefixem.
 *
 * Naked path má vždy single-slash tvar ("/foo", "/subdir/foo.mzf"),
 * shodný s tím, co vrací reálná Unikarta (uc1 i uc3 přes FatFS f_getcwd).
 * Dřívější quirk s vedoucím dvojitým lomítkem ("/foo" -> "//foo") byl
 * příčinou bugu v cmdGETCWD (mzdos PWD zobrazoval "//dir") a je opraven
 * v unicard_normalize_emu_path. */
void test_unicard_jail_basic_paths(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Po cmdRESET je work_dir = "/", tj. všechny relativní cesty
     * se rootují do SD root. */
    uc_send_cmd(cmdRESET);

    /* Case A: prázdná cesta - resolve na SD root. */
    char *naked = NULL;
    char *full = unicard_jail_resolve_with_naked("", &naked);
    TEST_ASSERT_NOT_NULL_MESSAGE(full, "Prázdná cesta musí být platná (SD root)");
    TEST_ASSERT_NOT_NULL(naked);
    /* Root naked musí být přesně single-slash "/". */
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/", naked,
                                     "Naked pro \"\" (SD root) musí být \"/\"");
    /* Plný path = SD_root (suffix s_test_sd_root sám sebe = true). */
    TEST_ASSERT_TRUE_MESSAGE(g_str_has_prefix(full, s_test_sd_root),
                             "Plný path \"\" musí být SD_root");
    baseui_tools_mem_free(full);
    baseui_tools_mem_free(naked);

    /* Case B: leading slash - SD-rooted path. Naked musí být single-slash. */
    naked = NULL;
    full = unicard_jail_resolve_with_naked("/foo", &naked);
    TEST_ASSERT_NOT_NULL_MESSAGE(naked, "Naked pro /foo nesmí být NULL");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/foo", naked,
                                     "Naked pro /foo musí být single-slash \"/foo\"");
    assert_full_under_sd_root(full, "foo", "Plný path /foo musí být SD_root/.../foo");
    baseui_tools_mem_free(full);
    baseui_tools_mem_free(naked);

    /* Case C: relativní subdir s souborem. Naked musí být single-slash. */
    naked = NULL;
    full = unicard_jail_resolve_with_naked("subdir/foo.mzf", &naked);
    TEST_ASSERT_NOT_NULL_MESSAGE(naked, "Naked pro subdir/foo.mzf nesmí být NULL");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/subdir/foo.mzf", naked,
                                     "Naked pro subdir/foo.mzf musí být \"/subdir/foo.mzf\"");
    assert_full_under_sd_root(full, "foo.mzf",
                              "Plný path musí obsahovat foo.mzf");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(full, "subdir"),
                                 "Plný path musí obsahovat subdir komponentu");
    baseui_tools_mem_free(full);
    baseui_tools_mem_free(naked);

    /* Case D: tečka jako CWD. */
    naked = NULL;
    full = unicard_jail_resolve_with_naked(".", &naked);
    TEST_ASSERT_NOT_NULL_MESSAGE(full, "Cesta \".\" musí být platná (SD root)");
    TEST_ASSERT_TRUE_MESSAGE(g_str_has_prefix(full, s_test_sd_root),
                             "Plný path \".\" musí být SD_root");
    baseui_tools_mem_free(full);
    baseui_tools_mem_free(naked);
}


/* Pokusy o escape ze SD root - jail MUSÍ vrátit NULL. */
void test_unicard_jail_escape_blocked(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET); /* work_dir = "/" */

    /* Pokud work_dir == "/" (= žádné absorpční úrovně), JEDEN ".."
     * segment už je escape. */

    const char *escape_paths[] = {
        "../etc/passwd",
        "../../foo",
        "subdir/../../bar",
        "subdir/../../../baz",
        "/..",
        "/../foo",
        NULL
    };

    for (int i = 0; escape_paths[i]; i++) {
        char *naked = NULL;
        char *full = unicard_jail_resolve_with_naked(escape_paths[i], &naked);
        char msg[128];
        g_snprintf(msg, sizeof(msg),
                   "Escape path '%s' musí vrátit NULL (= jail blokuje)",
                   escape_paths[i]);
        TEST_ASSERT_NULL_MESSAGE(full, msg);
        TEST_ASSERT_NULL_MESSAGE(naked,
                                 "Naked out param musí být NULL při escape");
    }
}


/* Bezpečné použití "." a ".." které NEvedou ven ze SD root. */
void test_unicard_jail_dot_segments(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);

    /* "subdir/./foo" -> SD_root/subdir/foo (".\" je no-op) */
    char *naked = NULL;
    char *full = unicard_jail_resolve_with_naked("subdir/./foo", &naked);
    assert_full_under_sd_root(full, "subdir",
                              "subdir/./foo musí projít (subdir komponenta)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(full, "foo"),
                                 "subdir/./foo: foo komponenta musí být přítomna");
    baseui_tools_mem_free(full);
    baseui_tools_mem_free(naked);

    /* "subdir/sub2/../foo" -> SD_root/subdir/foo (".." popne sub2) */
    naked = NULL;
    full = unicard_jail_resolve_with_naked("subdir/sub2/../foo", &naked);
    assert_full_under_sd_root(full, "subdir",
                              "subdir/sub2/../foo: subdir komponenta musí zůstat");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(full, "foo"),
                                 "subdir/sub2/../foo: foo komponenta musí být finální");
    /* Pozitivní: výsledek NESMÍ obsahovat "sub2" (= správně popnuto). */
    TEST_ASSERT_NULL_MESSAGE(strstr(full, "sub2"),
                             "Po normalizaci nesmí v plné cestě zůstat sub2");
    baseui_tools_mem_free(full);
    baseui_tools_mem_free(naked);

    /* "./foo" -> SD_root/foo */
    naked = NULL;
    full = unicard_jail_resolve_with_naked("./foo", &naked);
    assert_full_under_sd_root(full, "foo", "./foo musí projít");
    baseui_tools_mem_free(full);
    baseui_tools_mem_free(naked);
}


/* Windows backslash traversal: "..\\foo" se musí brát jako "../foo"
 * (= escape) - jinak by Windows guest mohl obejít jail. */
void test_unicard_jail_backslash_traversal(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);

    /* Single "..\\foo" - escape ze SD root. */
    char *full = unicard_jail_resolve("..\\foo");
    TEST_ASSERT_NULL_MESSAGE(full,
        "Backslash \"..\\\\foo\" musí být detekován jako escape");

    /* Compound backslash traversal. */
    full = unicard_jail_resolve("subdir\\..\\..\\bar");
    TEST_ASSERT_NULL_MESSAGE(full,
        "subdir\\\\..\\\\..\\\\bar musí být detekován jako escape");

    /* Mixed slash + backslash escape. */
    full = unicard_jail_resolve("subdir/..\\..\\bar");
    TEST_ASSERT_NULL_MESSAGE(full,
        "Mixed slash/backslash traversal musí být escape");

    /* Pozitivní: jeden backslash subdir bez "..\\" musí projít.
     * Implementace přepíše backslash -> forward slash ještě před
     * normalizací, takže naked výsledek je /subdir/foo. */
    char *naked = NULL;
    full = unicard_jail_resolve_with_naked("subdir\\foo", &naked);
    assert_full_under_sd_root(full, "subdir",
        "subdir\\\\foo (bez escape) musí projít po normalizaci backslash");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(full, "foo"),
        "Backslash-separated cesta musí dát plný path s 'foo' suffixem");
    baseui_tools_mem_free(full);
    baseui_tools_mem_free(naked);
}


/* SD jail integration test: cmdSTAT s escape path nesmí dosáhnout
 * souboru, který fyzicky existuje VEN ze SD root.
 *
 * Postup:
 *   1. Vytvořit reálný soubor v parent dir SD root (= simulace "host"
 *      souboru, který by emu mohl chtít vidět)
 *   2. Poslat cmdSTAT s relativním "../escaped.txt"
 *   3. Status musí mít ERROR=1, ff_res = FR_NO_FILE (jail escape)
 *   4. Soubor po testu uklidit
 */
void test_unicard_jail_via_cmdSTAT(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Vytvořit soubor "host_secret.txt" v parent dir SD root. */
    char *parent_dir = g_path_get_dirname(s_test_sd_root);
    char *secret_path = g_build_filename(parent_dir, "host_secret.txt", NULL);
    FILE *f = g_fopen(secret_path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Test setup: musí jít vytvořit parent file");
    fputs("HOSTONLY", f);
    fclose(f);

    /* Verify že soubor opravdu existuje (= sanity check setupu). */
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(secret_path, G_FILE_TEST_IS_REGULAR),
                             "Sanity: secret soubor musí existovat na hostu");

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdSTAT);
    uc_send_str("/../host_secret.txt"); /* escape attempt */

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR,
        "cmdSTAT s escape path MUSÍ vrátit ERROR (jail blokuje)");

    /* Žádné DOUTRQ - klient nesmí dostat žádná data o tom souboru. */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_DOUTRQ,
        "Při jail escape NESMÍ být DOUTRQ (=žádný FILINFO output)");

    /* ff_res = FR_NO_FILE (unicard_stat vrací FR_NO_FILE při jail escape). */
    uint8_t ff_res = uc_read_status_byte(3);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(FR_NO_FILE, ff_res,
        "ff_res po jail escape musí být FR_NO_FILE (ne OK + info)");

    /* Cleanup: smazat secret soubor. */
    g_remove(secret_path);
    g_free(secret_path);
    g_free(parent_dir);
}


/* ================================================================
 * UNIT TESTY - R/O režim emulované SD karty
 *
 * unicard_set_readonly(1) blokuje modifikační operace přes gate
 * v unicard_file_open (write/create flagy), unicard_file_write,
 * unicard_file_truncate, unicard_mkdir, unicard_unlink,
 * unicard_rename, unicard_utime. Read-only operace (FA_READ,
 * dir_open, stat) projdou.
 *
 * Helper teardown_readonly() obnoví R/O na 0 po každém testu, aby
 * nezasahoval do následujících tests (= žije v g_unicard.readonly
 * mezi setUp/tearDown).
 * ================================================================ */

/* Pomocný helper: vrátí R/O režim do default (off) po testu, aby
 * nezasahoval do dalších tests (g_unicard.readonly je persistentní
 * přes connect/disconnect). */
static void teardown_readonly(void)
{
    unicard_set_readonly(0);
}


/* R/O default po unicard_init() je 0 (= R/W). */
void test_unicard_readonly_default_off(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* setUp volá unicard_set_connected(CONNECTED) což interně volá
     * unicard_init - po něm musí být readonly=0. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, unicard_test_readonly(),
        "Default readonly po unicard_init musí být 0 (R/W)");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_unicard.readonly,
        "g_unicard.readonly musí default 0");
}


/* R/O blokuje unicard_file_open s create/write flagy.
 * FR_WRITE_PROTECTED + soubor nesmí vzniknout na hostu. */
void test_unicard_readonly_blocks_write(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_readonly(1);

    /* Try file_open s FA_CREATE_NEW | FA_WRITE - musí selhat. */
    st_UNICARD_FILE file;
    unicard_file_init(&file);
    FRESULT r = unicard_file_open(&file, "/ro_blocked.bin",
                                  FA_CREATE_NEW | FA_WRITE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED, r,
        "file_open s CREATE_NEW|WRITE v R/O musí dát FR_WRITE_PROTECTED");

    /* Soubor NESMÍ vzniknout na host FS. */
    char *full = g_build_filename(s_test_sd_root, "ro_blocked.bin", NULL);
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(full, G_FILE_TEST_EXISTS),
        "V R/O režimu nesmí vzniknout nový soubor");
    g_free(full);

    /* I FA_OPEN_ALWAYS (může vytvořit) musí být blokované. */
    r = unicard_file_open(&file, "/ro_blocked2.bin",
                          FA_OPEN_ALWAYS | FA_WRITE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED, r,
        "FA_OPEN_ALWAYS|FA_WRITE v R/O musí dát FR_WRITE_PROTECTED");

    /* FA_CREATE_ALWAYS taky. */
    r = unicard_file_open(&file, "/ro_blocked3.bin",
                          FA_CREATE_ALWAYS | FA_WRITE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED, r,
        "FA_CREATE_ALWAYS v R/O musí dát FR_WRITE_PROTECTED");

    teardown_readonly();
}


/* R/O blokuje unicard_mkdir. */
void test_unicard_readonly_blocks_mkdir(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_readonly(1);

    FRESULT r = unicard_mkdir("/ro_newdir");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED, r,
        "mkdir v R/O režimu musí dát FR_WRITE_PROTECTED");

    /* Adresář nevznikl. */
    char *full = g_build_filename(s_test_sd_root, "ro_newdir", NULL);
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(full, G_FILE_TEST_IS_DIR),
        "V R/O režimu nesmí vzniknout adresář");
    g_free(full);

    teardown_readonly();
}


/* R/O blokuje unicard_unlink. */
void test_unicard_readonly_blocks_unlink(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Vytvořit soubor v R/W režimu. */
    create_test_file("ro_protected.bin", 25);

    unicard_set_readonly(1);

    FRESULT r = unicard_unlink("/ro_protected.bin");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED, r,
        "unlink v R/O režimu musí dát FR_WRITE_PROTECTED");

    /* Soubor stále existuje. */
    char *full = g_build_filename(s_test_sd_root, "ro_protected.bin", NULL);
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(full, G_FILE_TEST_IS_REGULAR),
        "Soubor po blokovaném unlinku musí stále existovat");
    g_free(full);

    teardown_readonly();
}


/* R/O blokuje unicard_rename. */
void test_unicard_readonly_blocks_rename(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("ro_rename_src.bin", 10);

    unicard_set_readonly(1);

    FRESULT r = unicard_rename("/ro_rename_src.bin", "/ro_rename_dst.bin");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED, r,
        "rename v R/O režimu musí dát FR_WRITE_PROTECTED");

    /* Zdroj musí existovat, cíl ne. */
    char *src = g_build_filename(s_test_sd_root, "ro_rename_src.bin", NULL);
    char *dst = g_build_filename(s_test_sd_root, "ro_rename_dst.bin", NULL);
    TEST_ASSERT_TRUE_MESSAGE(g_file_test(src, G_FILE_TEST_EXISTS),
        "Zdroj rename musí stále existovat");
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(dst, G_FILE_TEST_EXISTS),
        "Cíl rename nesmí existovat");
    g_free(src);
    g_free(dst);

    teardown_readonly();
}


/* R/O blokuje unicard_file_truncate.
 *
 * Postup: nejdřív v R/W otevřít soubor s FA_WRITE, pak zapnout R/O,
 * pokus truncate musí selhat s FR_WRITE_PROTECTED. (Stejně tak by
 * měl selhat samotný file_write na již otevřeném souboru.) */
void test_unicard_readonly_blocks_truncate(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("ro_trunc_target.bin", 100);

    st_UNICARD_FILE file;
    unicard_file_init(&file);
    FRESULT r = unicard_file_open(&file, "/ro_trunc_target.bin", FA_WRITE);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, r, "Open v R/W režimu musí projít");

    /* Zapnout R/O AFTER open. */
    unicard_set_readonly(1);

    r = unicard_file_truncate(&file);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED, r,
        "truncate v R/O režimu musí dát FR_WRITE_PROTECTED");

    /* Pojistka: write na existing otevřený soubor je taky blokovaný. */
    uint8_t data[5] = {1, 2, 3, 4, 5};
    uint32_t written = 0;
    r = unicard_file_write(&file, data, sizeof(data), &written);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED, r,
        "file_write v R/O režimu musí dát FR_WRITE_PROTECTED");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, written,
        "Žádné byty nesmí být zapsané");

    /* Cleanup: zavřít file, vypnout R/O, ověřit že soubor má původní velikost. */
    unicard_file_close(&file);

    char *full = g_build_filename(s_test_sd_root, "ro_trunc_target.bin", NULL);
    GStatBuf st;
    g_stat(full, &st);
    TEST_ASSERT_EQUAL_INT_MESSAGE(100, st.st_size,
        "Velikost souboru po blokovaném truncate musí zůstat 100 B");
    g_free(full);

    teardown_readonly();
}


/* R/O persistuje napříč session - opakované toggling se chová
 * konzistentně (= flag se mění, gate stále drží).
 *
 * Pozn.: g_warning vystavený přes unicard_warn_readonly_blocked se
 * z testu deterministicky nezachycuje (nemáme log capture v Unity),
 * proto kontrolujeme jen state machine: gate musí blokovat při R/O=1
 * a propouštět při R/O=0, opakovaně.
 */
void test_unicard_readonly_persists_session(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* Sekvence: R/W -> blokující operace projde, R/O -> blokuje,
     * R/W -> propouští, R/O -> blokuje znovu. */

    /* Krok 1: R/W, mkdir musí projít. */
    unicard_set_readonly(0);
    TEST_ASSERT_EQUAL_INT(FR_OK, unicard_mkdir("/persist_dir_a"));

    /* Krok 2: R/O on, mkdir blokovaný. */
    unicard_set_readonly(1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED,
        unicard_mkdir("/persist_dir_b"),
        "Po zapnutí R/O musí být mkdir blokovaný");

    /* Druhý write attempt - stále blokovaný (counter potlačí warning,
     * ale gate samotný drží). */
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED,
        unicard_mkdir("/persist_dir_c"),
        "Druhý write v R/O sessions stále blokovaný (gate trvá)");

    /* Soubor /persist_dir_b ani /persist_dir_c NESMÍ existovat. */
    char *b = g_build_filename(s_test_sd_root, "persist_dir_b", NULL);
    char *c = g_build_filename(s_test_sd_root, "persist_dir_c", NULL);
    TEST_ASSERT_FALSE(g_file_test(b, G_FILE_TEST_EXISTS));
    TEST_ASSERT_FALSE(g_file_test(c, G_FILE_TEST_EXISTS));
    g_free(b);
    g_free(c);

    /* Krok 3: R/O off, mkdir znovu projde. */
    unicard_set_readonly(0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK,
        unicard_mkdir("/persist_dir_d"),
        "Po vypnutí R/O musí mkdir znovu projít");

    /* Krok 4: R/O znovu on - znovu blokuje. */
    unicard_set_readonly(1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_WRITE_PROTECTED,
        unicard_mkdir("/persist_dir_e"),
        "Opakované R/O on musí znovu blokovat");

    teardown_readonly();
}


/* R/O režim NESMÍ blokovat read operace. Ověřuje že file_open
 * FA_READ, file_read, dir_open, dir_read_filelist a stat projdou
 * i při readonly=1. */
void test_unicard_readonly_allows_read(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    /* V R/W režimu vytvořit test soubor s daty 0..49. */
    create_test_file("ro_readable.bin", 50);

    /* Zapnout R/O - od teď žádné writes. */
    unicard_set_readonly(1);

    /* 1) file_open s FA_READ na existing musí projít. */
    st_UNICARD_FILE file;
    unicard_file_init(&file);
    FRESULT r = unicard_file_open(&file, "/ro_readable.bin", FA_READ);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, r,
        "FA_READ open existing v R/O režimu MUSÍ projít");

    /* 2) file_read musí přečíst data. */
    uint8_t buf[64];
    uint32_t read_len = 0;
    r = unicard_file_read(&file, buf, 50, &read_len);
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, r,
        "file_read v R/O režimu musí projít");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(50, read_len,
        "Musí přečíst všech 50 bajtů");
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)i, buf[i],
            "Obsah souboru musí být beze změny");
    }
    unicard_file_close(&file);

    /* 3) dir_open + dir_read_filelist musí projít. */
    st_UNICARD_DIR dir;
    unicard_dir_init(&dir);
    r = unicard_dir_open(&dir, "/");
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, r,
        "dir_open v R/O režimu musí projít");

    char list_buf[256];
    r = unicard_dir_read_filelist(&dir, list_buf, sizeof(list_buf));
    TEST_ASSERT_EQUAL_INT_MESSAGE(FR_OK, r,
        "dir_read_filelist v R/O režimu musí projít");
    unicard_dir_close(&dir);

    /* 4) cmdSTAT na existing musí dát OK + DOUTRQ. */
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdSTAT);
    uc_send_str("/ro_readable.bin");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
        "cmdSTAT existing v R/O režimu NESMÍ ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_DOUTRQ, sts & UNICARD_STS_DOUTRQ,
        "cmdSTAT v R/O režimu musí dát DOUTRQ (info available)");

    /* Vyčíst FILINFO output aby se uvolnil DOUTRQ pro další testy. */
    int total_bytes = 23 + _MAX_LFN;
    for (int i = 0; i < total_bytes; i++) (void)uc_recv_data();

    teardown_readonly();
}


/* ================================================================
 * FatFS 8.3 SFN alias generátor (unicard_sfn) - čisté unit testy
 *
 * Očekávané hodnoty odvozeny ze zdroje FW Unicard (FatFS R0.13a,
 * create_name + gen_numname + get_fileinfo). Pole offset 9 = FatFS
 * FILINFO.altname.
 * ================================================================ */

/* Pomocník: vygeneruj alias pro jediné jméno (fresh kontext, daný styl). */
static void sfn_make_s(const char *name, en_UNICARD_SFN_STYLE style, char out[13])
{
    st_UNICARD_SFN_CTX ctx;
    unicard_sfn_ctx_init(&ctx);
    unicard_sfn_make(name, style, &ctx, out, NULL);
    unicard_sfn_ctx_clear(&ctx);
}

/* Pomocník: vrať has_lfn (jméno potřebuje LFN entry). Nezávisí na stylu. */
static gboolean sfn_has_lfn(const char *name)
{
    char out[13];
    gboolean hl = FALSE;
    st_UNICARD_SFN_CTX ctx;
    unicard_sfn_ctx_init(&ctx);
    unicard_sfn_make(name, UNICARD_SFN_STYLE_UC1, &ctx, out, &hl);
    unicard_sfn_ctx_clear(&ctx);
    return hl;
}

/* UC3 styl (Unicard3, FatFS R0.13a). */
static void sfn_single(const char *name, char out[13])
{
    sfn_make_s(name, UNICARD_SFN_STYLE_UC3, out);
}

void test_unicard_sfn_lossy_long_name(void)
{
    char out[13];
    sfn_single("Batman Demo.mzf", out);
    TEST_ASSERT_EQUAL_STRING("BATMAN~1.MZF", out);
}

void test_unicard_sfn_uc3_pure_83_uppercase_filled(void)
{
    /* "Opravený" uc3 plní pole S vždy 8.3 aliasem (velkými) - empty rule
     * (reálný FatFS by dal prázdné) záměrně NEpoužíváme. */
    char out[13];
    sfn_single("Y2K.MZF", out);
    TEST_ASSERT_EQUAL_STRING("Y2K.MZF", out);
    sfn_single("SAPO-P.MZF", out);
    TEST_ASSERT_EQUAL_STRING("SAPO-P.MZF", out);
}

void test_unicard_sfn_83_lowercase_uppercased(void)
{
    /* 8.3 jméno s malými písmeny -> má LFN entry, altname = velká 8.3. */
    char out[13];
    sfn_single("Gp-Simul.mzf", out);
    TEST_ASSERT_EQUAL_STRING("GP-SIMUL.MZF", out);
}

void test_unicard_sfn_space_in_name(void)
{
    /* Mezera v jméně -> lossy -> ~N alias. */
    char out[13];
    sfn_single("Picture Show.MZF", out);
    TEST_ASSERT_EQUAL_STRING("PICTUR~1.MZF", out);
}

void test_unicard_sfn_long_base(void)
{
    /* Základ delší než 8 znaků -> lossy -> ~N alias. */
    char out[13];
    sfn_single("Incopy102.mzf", out);
    TEST_ASSERT_EQUAL_STRING("INCOPY~1.MZF", out);
    sfn_single("Pool_3_V16k.mzf", out);
    TEST_ASSERT_EQUAL_STRING("POOL_3~1.MZF", out);
    sfn_single("Turbo_Copy_V1.21.mzf", out);
    TEST_ASSERT_EQUAL_STRING("TURBO_~1.MZF", out);
}

void test_unicard_sfn_collision_numbering(void)
{
    /* Dvě lossy jména se stejným 6-znak prefixem + ext dostanou ~1 a ~2. */
    char out[13];
    st_UNICARD_SFN_CTX ctx;
    unicard_sfn_ctx_init(&ctx);

    unicard_sfn_make("LongNameOne.txt", UNICARD_SFN_STYLE_UC3, &ctx, out, NULL);
    TEST_ASSERT_EQUAL_STRING("LONGNA~1.TXT", out);

    unicard_sfn_make("LongNameTwo.txt", UNICARD_SFN_STYLE_UC3, &ctx, out, NULL);
    TEST_ASSERT_EQUAL_STRING("LONGNA~2.TXT", out);

    unicard_sfn_ctx_clear(&ctx);
}

/* has_lfn: jméno potřebuje LFN entry jen když není čisté 8.3 (lossy /
 * smíšená velikost / non-ASCII). Reálné HW podle toho plní pole LFN. */
void test_unicard_sfn_has_lfn_flag(void)
{
    /* Mají LFN: smíšená velikost a lossy. */
    TEST_ASSERT_TRUE(sfn_has_lfn("Bomber.mzf"));        /* mixed case */
    TEST_ASSERT_TRUE(sfn_has_lfn("Batman Demo.mzf"));   /* lossy */
    TEST_ASSERT_TRUE(sfn_has_lfn("Gp-Simul.mzf"));      /* mixed case */
    /* Nemají LFN: čistě 8.3 uniform case (velká i malá). */
    TEST_ASSERT_FALSE(sfn_has_lfn("SAPO-P.MZF"));       /* upper 8.3 */
    TEST_ASSERT_FALSE(sfn_has_lfn("Y2K.MZF"));          /* upper 8.3 */
    TEST_ASSERT_FALSE(sfn_has_lfn("seesharp.mzf"));     /* lower 8.3 */
}

/* UC1 styl (MZ800UKP1, FatFS R0.09): offset 9 vždy vyplněné, 8.3 s case.
 * Očekávané hodnoty odvozeny z reálné karty (fotka FW1) + R0.09 zdroje. */
void test_unicard_sfn_uc1_pure_83_filled(void)
{
    /* Reálná uc1 karta plní offset 9 i pro čisté 8.3 (na rozdíl od uc3). */
    char out[13];
    sfn_make_s("Y2K.MZF", UNICARD_SFN_STYLE_UC1, out);
    TEST_ASSERT_EQUAL_STRING("Y2K.MZF", out);
    sfn_make_s("SAPO-P.MZF", UNICARD_SFN_STYLE_UC1, out);
    TEST_ASSERT_EQUAL_STRING("SAPO-P.MZF", out);
}

void test_unicard_sfn_uc1_lowercase_preserved(void)
{
    /* uc1 zachová malá písmena (NT case flag): seesharp.mzf zůstane malými. */
    char out[13];
    sfn_make_s("seesharp.mzf", UNICARD_SFN_STYLE_UC1, out);
    TEST_ASSERT_EQUAL_STRING("seesharp.mzf", out);
}

void test_unicard_sfn_uc1_mixed_uppercased(void)
{
    /* Smíšená velikost (mixed) nemá uniform NT flag -> velká písmena. */
    char out[13];
    sfn_make_s("Gp-Simul.mzf", UNICARD_SFN_STYLE_UC1, out);
    TEST_ASSERT_EQUAL_STRING("GP-SIMUL.MZF", out);
}

void test_unicard_sfn_uc1_lossy(void)
{
    /* Lossy jména mají ~N alias stejně jako uc3 (velkými). */
    char out[13];
    sfn_make_s("Batman Demo.mzf", UNICARD_SFN_STYLE_UC1, out);
    TEST_ASSERT_EQUAL_STRING("BATMAN~1.MZF", out);
    sfn_make_s("Incopy102.mzf", UNICARD_SFN_STYLE_UC1, out);
    TEST_ASSERT_EQUAL_STRING("INCOPY~1.MZF", out);
}

/* Integrace: cmdSTAT na lossy jméno musí v poli offset 9 (8.3 short)
 * vrátit FatFS alias, ne uťatý LFN. */
void test_unicard_cmdSTAT_sfn_alias(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("Batman Demo.mzf", 100);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdSTAT);
    uc_send_str("/Batman Demo.mzf");

    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR,
                                    "STAT existujícího souboru nesmí ERROR");

    /* skip fsize(4)+fdate(2)+ftime(2)+fattrib(1) = 9 bytů */
    for (int i = 0; i < 9; i++) (void)uc_recv_data();
    char fname[13];
    for (int i = 0; i < 13; i++) fname[i] = (char)uc_recv_data();

    TEST_ASSERT_EQUAL_STRING_MESSAGE("BATMAN~1.MZF", fname,
        "offset 9 musí být FatFS 8.3 alias, ne uťatý LFN");

    /* drain zbytek (lfn_strlen + lfname) aby se uvolnil DOUTRQ */
    (void)uc_recv_data();
    for (int i = 0; i < _MAX_LFN; i++) (void)uc_recv_data();
}

/* Integrace: cmdREADDIR musí v poli offset 9 vrátit FatFS alias. */
void test_unicard_cmdREADDIR_sfn_alias(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    char *sub = g_build_filename(s_test_sd_root, "sfndir", NULL);
    g_mkdir(sub, 0777);
    g_free(sub);
    create_test_file("sfndir/Batman Demo.mzf", 100);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdREADDIR);
    uc_send_str("/sfndir");

    int found = 0;
    for (int iter = 0; iter < 100; iter++) {
        uint8_t sts = uc_master_status();
        if (!(sts & UNICARD_STS_READDIR)) break;

        (void)uc_recv_dword_le(); /* fsize */
        (void)uc_recv_word_le();  /* fdate */
        (void)uc_recv_word_le();  /* ftime */
        (void)uc_recv_data();     /* fattrib */
        char fname[13];
        for (int i = 0; i < 13; i++) fname[i] = (char)uc_recv_data();
        (void)uc_recv_data();     /* lfn_strlen */
        for (int i = 0; i < _MAX_LFN; i++) (void)uc_recv_data(); /* lfname */

        if (strcmp(fname, "BATMAN~1.MZF") == 0) found = 1;
    }

    TEST_ASSERT_TRUE_MESSAGE(found,
        "READDIR offset 9 musí obsahovat FatFS alias BATMAN~1.MZF");
}

/* Integrace uc3: off 23 = primární jméno (fname) - VŽDY vyplněné, i pro
 * čisté 8.3 (SAPO-P). off 9 (altname) je naopak u čistě-VELKÝCH 8.3
 * prázdné -> jméno nese pole L. (uc3 default; položky podle fsize.) */
void test_unicard_cmdREADDIR_uc3_lfn_always(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    char *sub = g_build_filename(s_test_sd_root, "lfndir", NULL);
    g_mkdir(sub, 0777);
    g_free(sub);
    create_test_file("lfndir/SAPO-P.MZF", 111);    /* čisté 8.3 UPPER */
    create_test_file("lfndir/Bomber.mzf", 222);    /* mixed case */

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdREADDIR);
    uc_send_str("/lfndir");

    int seen_sapo = 0, seen_bomber = 0;
    for (int iter = 0; iter < 100; iter++) {
        uint8_t sts = uc_master_status();
        if (!(sts & UNICARD_STS_READDIR)) break;

        uint32_t fsize = uc_recv_dword_le();
        (void)uc_recv_word_le();  /* fdate */
        (void)uc_recv_word_le();  /* ftime */
        (void)uc_recv_data();     /* fattrib */
        char sbuf[13];
        for (int i = 0; i < 13; i++) sbuf[i] = (char)uc_recv_data();  /* S (off 9) */
        sbuf[12] = '\0';
        uint8_t len = uc_recv_data();   /* LEN (off 22) */
        char lfname[33];
        for (int i = 0; i < 32; i++) lfname[i] = (char)uc_recv_data();
        lfname[32] = '\0';

        if (fsize == 111) {
            seen_sapo = 1;
            /* uc3 (opravený): off 23 = primární jméno, off 9 = 8.3 alias
             * (vždy vyplněné). */
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(strlen("SAPO-P.MZF"), len, "SAPO-P uc3: LEN=délka jména");
            TEST_ASSERT_EQUAL_STRING_MESSAGE("SAPO-P.MZF", lfname, "SAPO-P uc3: L=primární jméno");
            TEST_ASSERT_EQUAL_STRING_MESSAGE("SAPO-P.MZF", sbuf, "SAPO-P uc3: S=8.3 alias (vždy)");
        } else if (fsize == 222) {
            seen_bomber = 1;
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(strlen("Bomber.mzf"), len, "Bomber uc3: LEN");
            TEST_ASSERT_EQUAL_STRING_MESSAGE("Bomber.mzf", lfname, "Bomber uc3: L=primární jméno");
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(seen_sapo, "READDIR musí vidět SAPO-P.MZF");
    TEST_ASSERT_TRUE_MESSAGE(seen_bomber, "READDIR musí vidět Bomber.mzf");
}

/* Integrace uc1: off 23 = LFN - plněno JEN pro jména s LFN entry. Čisté
 * 8.3 (SAPO-P) má L prázdné + LEN=0 (jméno nese pole S); mixed (Bomber)
 * má L=LFN. Přepíná FW na uc1 (save/restore). */
void test_unicard_cmdREADDIR_uc1_lfn_conditional(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    if (g_mzarch_platform_numeric != 800) {
        TEST_IGNORE_MESSAGE("uc1 firmware jen na MZ-800");
        return;
    }

    en_UNICARD_FW saved_fw = unicard_get_fw();
    unicard_set_fw(UNICARD_FW_UC1);

    char *sub = g_build_filename(s_test_sd_root, "uc1lfn", NULL);
    g_mkdir(sub, 0777);
    g_free(sub);
    create_test_file("uc1lfn/SAPO-P.MZF", 111);
    create_test_file("uc1lfn/Bomber.mzf", 222);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdREADDIR);
    uc_send_str("/uc1lfn");

    int seen_sapo = 0, seen_bomber = 0;
    for (int iter = 0; iter < 100; iter++) {
        uint8_t sts = uc_master_status();
        if (!(sts & UNICARD_STS_READDIR)) break;

        uint32_t fsize = uc_recv_dword_le();
        (void)uc_recv_word_le();
        (void)uc_recv_word_le();
        (void)uc_recv_data();
        char sbuf[13];
        for (int i = 0; i < 13; i++) sbuf[i] = (char)uc_recv_data();  /* S (off 9) */
        sbuf[12] = '\0';
        uint8_t len = uc_recv_data();
        char lfname[33];
        for (int i = 0; i < 32; i++) lfname[i] = (char)uc_recv_data();
        lfname[32] = '\0';

        if (fsize == 111) {
            seen_sapo = 1;
            /* uc1: off 23 prázdné (bez LFN), jméno v S (off 9). */
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, len, "SAPO-P uc1: LEN=0");
            TEST_ASSERT_EQUAL_STRING_MESSAGE("", lfname, "SAPO-P uc1: L prázdné");
            TEST_ASSERT_EQUAL_STRING_MESSAGE("SAPO-P.MZF", sbuf, "SAPO-P uc1: S=8.3 jméno");
        } else if (fsize == 222) {
            seen_bomber = 1;
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(strlen("Bomber.mzf"), len, "Bomber uc1: LEN");
            TEST_ASSERT_EQUAL_STRING_MESSAGE("Bomber.mzf", lfname, "Bomber uc1: L=LFN");
        }
    }

    unicard_set_fw(saved_fw);

    TEST_ASSERT_TRUE_MESSAGE(seen_sapo, "READDIR musí vidět SAPO-P.MZF");
    TEST_ASSERT_TRUE_MESSAGE(seen_bomber, "READDIR musí vidět Bomber.mzf");
}

/* Integrace uc1: v podadresáři posílá ".." jako první položku; pole S má
 * 8.3 s case, L jen pro jména s LFN. Přepíná FW na uc1 (save/restore). */
void test_unicard_cmdREADDIR_uc1_dotdot(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    if (g_mzarch_platform_numeric != 800) {
        TEST_IGNORE_MESSAGE("uc1 firmware jen na MZ-800");
        return;
    }

    en_UNICARD_FW saved_fw = unicard_get_fw();
    unicard_set_fw(UNICARD_FW_UC1);

    char *sub = g_build_filename(s_test_sd_root, "uc1dir", NULL);
    g_mkdir(sub, 0777);
    g_free(sub);
    create_test_file("uc1dir/seesharp.mzf", 333);  /* čistě malá 8.3 */

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdREADDIR);
    uc_send_str("/uc1dir");

    int dotdot_first = -1, idx = 0;
    int seen_lower = 0;
    for (int iter = 0; iter < 100; iter++) {
        uint8_t sts = uc_master_status();
        if (!(sts & UNICARD_STS_READDIR)) break;

        (void)uc_recv_dword_le(); /* fsize */
        (void)uc_recv_word_le();  /* fdate */
        (void)uc_recv_word_le();  /* ftime */
        (void)uc_recv_data();     /* fattrib */
        char fname[13];
        for (int i = 0; i < 13; i++) fname[i] = (char)uc_recv_data();
        (void)uc_recv_data();     /* LEN */
        for (int i = 0; i < 32; i++) (void)uc_recv_data();  /* L */

        if (strcmp(fname, "..") == 0 && dotdot_first < 0) dotdot_first = idx;
        if (strcmp(fname, "seesharp.mzf") == 0) seen_lower = 1;  /* uc1 S = malými */
        idx++;
    }

    unicard_set_fw(saved_fw);  /* restore */

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, dotdot_first,
        "uc1 musí poslat '..' jako PRVNÍ položku v podadresáři");
    TEST_ASSERT_TRUE_MESSAGE(seen_lower,
        "uc1 S pole musí mít malá písmena (seesharp.mzf)");
}


/* ================================================================
 * MAIN
 * ================================================================ */

/* ================================================================
 * Jména v kódové stránce karty (uc3: CP852, FatFS FF_CODE_PAGE 852)
 * ================================================================ */

/* Totéž jméno v UTF-8 (hostitel) a v CP852 (port karty). */
#define CP852_FILE_UTF8 "\xC4" "\x8D" "ten\xC3" "\xAD" ".txt"          /* čtení.txt */
#define CP852_FILE_CARD "\x9F" "ten\xA1" ".txt"
#define CP852_DIR_UTF8  "slo\xC5" "\xBE" "ka"                            /* složka */
#define CP852_DIR_CARD  "slo\xA7" "ka"
/* "Žluťoučký kůň úpěl ďábelské.txt": 31 znaků, v UTF-8 42 bajtů */
#define CP852_LONG_UTF8 "\xC5" "\xBD" "lu\xC5" "\xA5" "ou\xC4" "\x8D" "k\xC3" "\xBD" " k\xC5" "\xAF" "\xC5" \
                        "\x88" " \xC3" "\xBA" "p\xC4" "\x9B" "l \xC4" "\x8F" "\xC3" "\xA1" "belsk\xC3" "\xA9" ".txt"
#define CP852_LONG_CARD "\xA6" "lu\x9C" "ou\x9F" "k\xEC" " k\x85" "\xE5" " \xA3" "p\xD8" "l \xD4" "\xA0" \
                        "belsk\x82" ".txt"
/* "Žluťoučký kůň úpěl ďábelské ódy.txt": 35 znaků, víc než FF_MAX_LFN */
#define CP852_TOOLONG_CARD "\xA6" "lu\x9C" "ou\x9F" "k\xEC" " k\x85" "\xE5" " \xA3" "p\xD8" "l \xD4" "\xA0" \
                           "belsk\x82" " \xA2" "dy.txt"
#define CP852_TOOLONG_UTF8 "\xC5" "\xBD" "lu\xC5" "\xA5" "ou\xC4" "\x8D" "k\xC3" "\xBD" " k\xC5" "\xAF" \
                           "\xC5" "\x88" " \xC3" "\xBA" "p\xC4" "\x9B" "l \xC4" "\x8F" "\xC3" "\xA1" "belsk\xC3" \
                           "\xA9" " \xC3" "\xB3" "dy.txt"

/* Helper: celý výstup cmdFILELIST adresáře @p path ("jméno\r velikost\r"
 * za sebou, bajty tak, jak přišly portem). */
static size_t uc_read_filelist(const char *path, char *out, size_t cap)
{
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdFILELIST);
    uc_send_str(path);
    size_t n = 0;
    while ((n < cap - 1) && (uc_master_status() & UNICARD_STS_READDIR)) {
        out[n++] = (char)uc_recv_data();
    }
    out[n] = '\0';
    return n;
}


/* uc3: FILELIST posílá jména v CP852, ne v UTF-8 hostitele. */
void test_unicard_cp852_filelist(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file(CP852_FILE_UTF8, 3);
    char out[2048];
    uc_read_filelist("/", out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, CP852_FILE_CARD "\r3\r"),
                                 "FILELIST musí poslat jméno v CP852");
    TEST_ASSERT_NULL_MESSAGE(strstr(out, CP852_FILE_UTF8), "FILELIST nesmí poslat UTF-8");
}


/* uc3: cesta v CP852 otevře hostitelský soubor s UTF-8 jménem. */
void test_unicard_cp852_open(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file(CP852_FILE_UTF8, 5);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str("/" CP852_FILE_CARD);
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR, "OPEN cesty v CP852 nesmí ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_READ, sts & UNICARD_STS_READ, "Soubor musí být otevřený");
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)i, uc_recv_data(), "Obsah souboru");
    }
    uc_close();
}


/* uc3: STAT vrátí dlouhé jméno v CP852 a jeho délku v bajtech CP852. */
void test_unicard_cp852_stat(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file(CP852_LONG_UTF8, 1);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdSTAT);
    uc_send_str("/" CP852_LONG_CARD);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "STAT nesmí ERROR");
    for (int i = 0; i < 9 + 13; i++) (void)uc_recv_data();  /* fsize..fattrib, fname[13] */
    uint8_t lfn_strlen = uc_recv_data();
    char lfn[_MAX_LFN + 1];
    for (int i = 0; i < _MAX_LFN; i++) lfn[i] = (char)uc_recv_data();
    lfn[_MAX_LFN] = '\0';
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(31, lfn_strlen, "Délka jména v bajtech CP852");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(CP852_LONG_CARD, lfn, "Dlouhé jméno v CP852");
}


/* uc3: jméno s 31 znaky (v UTF-8 42 bajtů) se vejde (FF_MAX_LFN 32 v CP852),
 * jméno s 35 znaky ne. */
void test_unicard_cp852_length_in_card_cp(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file(CP852_LONG_UTF8, 1);
    create_test_file(CP852_TOOLONG_UTF8, 1);
    char out[2048];
    uc_read_filelist("/", out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, CP852_LONG_CARD "\r1\r"), "Jméno s 31 znaky musí být ve výpisu");
    TEST_ASSERT_NULL_MESSAGE(strstr(out, CP852_TOOLONG_CARD), "Jméno s 35 znaky se pod dlouhým jménem nevejde");

    int found = 0;
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdREADDIR);
    uc_send_str("/");
    for (int iter = 0; iter < 100; iter++) {
        uint32_t fsize;
        uint8_t fattrib;
        char name[_MAX_LFN + 1];
        if (!uc_read_dir_filinfo(&fsize, &fattrib, name, sizeof(name))) break;
        if (0 == strcmp(name, CP852_LONG_CARD)) found = 1;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "READDIR musí vrátit jméno s 31 znaky v CP852");
}


/* uc3: CHDIR do adresáře zadaného v CP852, GETCWD ho vrátí v CP852. */
void test_unicard_cp852_chdir_getcwd(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    char *full = g_build_filename(s_test_sd_root, CP852_DIR_UTF8, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_mkdir(full, 0755), "mkdir adresáře s UTF-8 jménem");
    g_free(full);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdCHDIR);
    uc_send_str("/" CP852_DIR_CARD);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "CHDIR v CP852 nesmí ERROR");
    recv_cmdGETCWD_expect("0:/" CP852_DIR_CARD "\x0d");
}


/* uc1: jména jdou beze změny (jako dřív). Jen MZ-800 (uc1 jinde není). */
void test_unicard_uc1_names_unchanged(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    if (g_mzarch_platform_numeric != 800) return;

    create_test_file(CP852_FILE_UTF8, 3);
    unicard_set_fw(UNICARD_FW_UC1);
    char out[2048];
    uc_read_filelist("/", out, sizeof(out));
    unicard_set_fw(UNICARD_FW_UC3);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, CP852_FILE_UTF8 "\r3\r"), "uc1 posílá jméno beze změny");
}


/* ================================================================
 * Jména, která karta pod dlouhým jménem neukáže (uc3: 8.3 alias jako
 * FatFS s FF_MAX_LFN 32)
 * ================================================================ */

#define LONG_ASCII_NAME "a_very_long_file_name_with_more_than_31_chars.mzf"
#define LONG_DIR_NAME   "a directory with a very long name here"
#define JAPAN_NAME      "\xE3" "\x83" "\x86" "\xE3" "\x82" "\xB9" "\xE3" "\x83" "\x88" ".mzf"   /* テスト.mzf */

/* Alias jména bez kolizí (v testovacím kořeni nic nekoliduje). */
static void alias_of(const char *name, char out[13])
{
    sfn_single(name, out);
    TEST_ASSERT_TRUE_MESSAGE(out[0] != 0x00, "Alias nesmí být prázdný");
}


/* uc3: FILELIST ukáže dlouhé jméno a jméno se znakem mimo CP852 jako 8.3 alias. */
void test_unicard_alias_filelist(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file(LONG_ASCII_NAME, 3);
    create_test_file(JAPAN_NAME, 4);
    char a_long[13], a_jp[13];
    alias_of(LONG_ASCII_NAME, a_long);
    alias_of(JAPAN_NAME, a_jp);
    TEST_ASSERT_NOT_NULL_MESSAGE(strchr(a_long, '~'), "Alias dlouhého jména má ~N");

    char out[2048], line[64];
    uc_read_filelist("/", out, sizeof(out));
    snprintf(line, sizeof(line), "%s\r3\r", a_long);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, line), "Dlouhé jméno jako 8.3 alias");
    snprintf(line, sizeof(line), "%s\r4\r", a_jp);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, line), "Jméno mimo CP852 jako 8.3 alias");
    TEST_ASSERT_NULL_MESSAGE(strstr(out, "a_very_long"), "Dlouhé jméno se nesmí poslat");
}


/* uc3: OPEN a STAT podle aliasu najdou hostitelský soubor; STAT vrátí alias
 * jako jméno. */
void test_unicard_alias_open_stat(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file(LONG_ASCII_NAME, 5);
    char a[13], path[20];
    alias_of(LONG_ASCII_NAME, a);
    snprintf(path, sizeof(path), "/%s", a);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str(path);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "OPEN podle aliasu");
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)i, uc_recv_data(), "Obsah souboru");
    }
    uc_close();

    uc_send_cmd(cmdSTAT);
    uc_send_str(path);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "STAT podle aliasu");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5, uc_recv_dword_le(), "Velikost");
    for (int i = 0; i < 5 + 13; i++) (void)uc_recv_data();  /* fdate, ftime, fattrib, fname[13] */
    uint8_t lfn_strlen = uc_recv_data();
    char lfn[_MAX_LFN + 1];
    for (int i = 0; i < _MAX_LFN; i++) lfn[i] = (char)uc_recv_data();
    lfn[_MAX_LFN] = '\0';
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a, lfn, "STAT: jméno = alias");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(strlen(a), lfn_strlen, "STAT: délka aliasu");
}


/* uc3: READDIR vrátí alias jako jméno (offset 23). */
void test_unicard_alias_readdir(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file(LONG_ASCII_NAME, 1);
    char a[13];
    alias_of(LONG_ASCII_NAME, a);
    int found = 0;
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdREADDIR);
    uc_send_str("/");
    for (int iter = 0; iter < 100; iter++) {
        uint32_t fsize;
        uint8_t fattrib;
        char name[_MAX_LFN + 1];
        if (!uc_read_dir_filinfo(&fsize, &fattrib, name, sizeof(name))) break;
        if (0 == strcmp(name, a)) found = 1;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "READDIR musí vrátit alias dlouhého jména");
}


/* uc3: CHDIR podle aliasu do adresáře s dlouhým jménem, GETCWD vrátí alias. */
void test_unicard_alias_chdir_getcwd(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    char *full = g_build_filename(s_test_sd_root, LONG_DIR_NAME, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_mkdir(full, 0755), "mkdir adresáře s dlouhým jménem");
    g_free(full);
    char a[13], path[20], cwd[24];
    alias_of(LONG_DIR_NAME, a);
    snprintf(path, sizeof(path), "/%s", a);
    snprintf(cwd, sizeof(cwd), "0:/%s\x0d", a);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdCHDIR);
    uc_send_str(path);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "CHDIR podle aliasu");
    recv_cmdGETCWD_expect(cwd);
}


/* uc3: i soubor s dlouhým jménem, které se vejde, jde otevřít 8.3 aliasem
 * (FatFS hledá i podle krátkého jména). */
void test_unicard_alias_of_name_that_fits(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("Moje data.txt", 2);
    char a[13], path[20];
    alias_of("Moje data.txt", a);
    snprintf(path, sizeof(path), "/%s", a);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str(path);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "OPEN podle aliasu");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_READ, uc_master_status() & UNICARD_STS_READ, "Otevřeno");
    uc_close();
}


/* uc1: dlouhé jméno se dál vynechá (beze změny). Jen MZ-800. */
void test_unicard_uc1_long_name_skipped(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    if (g_mzarch_platform_numeric != 800) return;

    create_test_file(LONG_ASCII_NAME, 3);
    char a[13];
    alias_of(LONG_ASCII_NAME, a);
    unicard_set_fw(UNICARD_FW_UC1);
    char out[2048];
    uc_read_filelist("/", out, sizeof(out));
    unicard_set_fw(UNICARD_FW_UC3);
    TEST_ASSERT_NULL_MESSAGE(strstr(out, a), "uc1 alias neposílá");
    TEST_ASSERT_NULL_MESSAGE(strstr(out, "a_very_long"), "uc1 dlouhé jméno vynechá");
}


/* ================================================================
 * cmdMOUNT (uc3 = fw 0.26): konfigurace {SD}/unicard/<jednotka>.cfg
 * ================================================================ */

/* MOUNT jednotky @p dev s cestou @p path. FDC se v těchto testech nespravuje
 * (managed_fdc NONE): testuje se konfigurace a stav, ne načtení DSK. */
static void uc_mount(uint8_t dev, const char *path)
{
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_NONE);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdFDDMOUNT);
    uc_send_data(dev);
    uc_send_str(path);
}

/* Obsah {SD}/unicard/@p name, nebo NULL když není (g_free). */
static char *read_unicard_cfg(const char *name)
{
    char *full = g_build_filename(s_test_sd_root, UNICARD_UNIMGR_DIR, name, NULL);
    char *data = NULL;
    if (!g_file_get_contents(full, &data, NULL, NULL)) data = NULL;
    g_free(full);
    return data;
}


/* Připojení existujícího obrazu: konfigurace = cesta, jak přišla, OK. */
void test_unicard_mount_writes_cfg(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("disk.dsk", 16);
    uc_mount(1, "/disk.dsk");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "MOUNT existujícího obrazu = OK");
    char *cfg = read_unicard_cfg("fd1.cfg");
    TEST_ASSERT_NOT_NULL_MESSAGE(cfg, "fd1.cfg musí vzniknout");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/disk.dsk", cfg, "fd1.cfg = cesta bez konce řádku");
    g_free(cfg);
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
}


/* uc3: čtení fdN.cfg ho nepřepíše (bez spravovaného FDC ani s prázdnou
 * mechanikou: karta po MOUNT chybějícího obrazu); dřív ho emulace přepsala
 * podle FDC (prázdná mechanika = prázdný soubor). */
void test_unicard_fdcfg_read_keeps_cfg(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("disk.dsk", 16);
    uc_mount(1, "/disk.dsk");                   /* managed FDC NONE */
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);
    uc_send_str("/unicard/fd1.cfg");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "OPEN fd1.cfg = OK");
    char got[32];
    size_t n = 0;
    while ((n < sizeof(got) - 1) && !(uc_master_status() & UNICARD_STS_EOF)) got[n++] = (char)uc_recv_data();
    got[n] = 0;
    uc_close();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/disk.dsk", got, "klient čte konfiguraci z MOUNT");
    char *cfg = read_unicard_cfg("fd1.cfg");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/disk.dsk", cfg, "bez spravovaného FDC beze změny");
    g_free(cfg);

    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
    uc_mount(0, "/nothing.dsk");                /* chybějící obraz: konfigurace zůstane, mechanika prázdná */
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);
    uc_send_str("/unicard/fd0.cfg");
    uc_close();
    cfg = read_unicard_cfg("fd0.cfg");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/nothing.dsk", cfg, "prázdná mechanika: konfigurace karty zůstane");
    g_free(cfg);
}


/* Jednotky 4-7 mají vlastní jména konfigurace (ramdisc, qd, emm, sasi). */
void test_unicard_mount_cfg_names_4_7(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static const char *names[4] = { "ramdisc.cfg", "qd.cfg", "emm.cfg", "sasi.cfg" };
    create_test_file("image.bin", 16);
    for (uint8_t dev = 4; dev <= 7; dev++) {
        uc_mount(dev, "/image.bin");
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "MOUNT 4-7 = OK");
        char *cfg = read_unicard_cfg(names[dev - 4]);
        TEST_ASSERT_NOT_NULL_MESSAGE(cfg, names[dev - 4]);
        TEST_ASSERT_EQUAL_STRING("/image.bin", cfg);
        g_free(cfg);
    }
    uc_mount(8, "/image.bin");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & UNICARD_STS_ERROR, "Jednotka 8 = ERROR");
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
}


/* Neexistující obraz: konfigurace se zapíše, ale ERROR (načtení selže). */
void test_unicard_mount_missing_image(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_mount(0, "/nothing.dsk");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & UNICARD_STS_ERROR,
                                    "MOUNT neexistujícího obrazu = ERROR");
    /* rev 2 (firmware 0.37) stejně: kód 0 a FRESULT 0 (reload_FDD ff_res nenastaví) */
    unicard_set_api_rev2(1);
    uc_mount(0, "/nothing.dsk");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & UNICARD_STS_ERROR, "rev 2: ERROR");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, uc_read_status_byte(2), "rev 2: kód 0 jako firmware 0.37");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, uc_read_status_byte(3), "rev 2: FRESULT 0");
    unicard_set_api_rev2(0);
    char *cfg = read_unicard_cfg("fd0.cfg");
    TEST_ASSERT_NOT_NULL_MESSAGE(cfg, "fd0.cfg se přesto zapíše");
    TEST_ASSERT_EQUAL_STRING("/nothing.dsk", cfg);
    g_free(cfg);
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
}


/* Vysunutí: konfigurace se smaže, fw 0.26 přesto hlásí ERROR. Bez konfigurace
 * také ERROR. */
void test_unicard_mount_eject_uc3_error(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("disk.dsk", 16);
    uc_mount(2, "/disk.dsk");
    uc_mount(2, "");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & UNICARD_STS_ERROR,
                                    "fw 0.26: vysunutí hlásí ERROR");
    char *cfg = read_unicard_cfg("fd2.cfg");
    TEST_ASSERT_NULL_MESSAGE(cfg, "fd2.cfg se smaže");
    g_free(cfg);
    uc_mount(2, "");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & UNICARD_STS_ERROR,
                                    "Vysunutí bez konfigurace = ERROR");
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
}


/* Relativní cesta je od kořene SD (ne od CWD) a zapíše se, jak přišla; cesta
 * v CP852 se zapíše v CP852. */
void test_unicard_mount_relative_and_cp852(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("disk.dsk", 16);
    char *full = g_build_filename(s_test_sd_root, "sub", NULL);
    g_mkdir(full, 0755);
    g_free(full);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdCHDIR);
    uc_send_str("/sub");
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_NONE);
    uc_send_cmd(cmdFDDMOUNT);       /* bez RESET: CWD zůstane /sub */
    uc_send_data(3);
    uc_send_str("disk.dsk");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "Relativní cesta od kořene");
    char *cfg = read_unicard_cfg("fd3.cfg");
    TEST_ASSERT_EQUAL_STRING("disk.dsk", cfg);
    g_free(cfg);

    create_test_file(CP852_FILE_UTF8, 16);
    uc_mount(3, "/" CP852_FILE_CARD);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "Cesta v CP852");
    cfg = read_unicard_cfg("fd3.cfg");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/" CP852_FILE_CARD, cfg, "fd3.cfg v CP852");
    g_free(cfg);
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
}


/* ================================================================
 * cmdRTCSETD / cmdRTCSETT (uc3): nastavení hodin karty
 * ================================================================ */

static void uc_rtc_set(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c)
{
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmd);
    uc_send_data(a);
    uc_send_data(b);
    uc_send_data(c);
}

static void uc_rtc_get(uint8_t cmd, uint8_t out[3])
{
    uc_send_cmd(cmd);
    for (int i = 0; i < 3; i++) out[i] = uc_recv_data();
}


/* Nastavené datum se čte zpět, hodiny jdou dál. */
void test_unicard_rtc_set_date(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_rtc_set(cmdRTCSETD, 1, 2, 2030 - 1980);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "RTCSETD = OK");
    uint8_t d[3];
    uc_rtc_get(cmdRTCGETD, d);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, d[0], "den");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, d[1], "měsíc");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2030 - 1980, d[2], "rok - 1980");
}


/* Nastavený čas se čte zpět (do 2 s), datum zůstane. */
void test_unicard_rtc_set_time(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t d0[3], d1[3], t[3];
    uc_rtc_get(cmdRTCGETD, d0);
    uc_rtc_set(cmdRTCSETT, 12, 34, 56);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "RTCSETT = OK");
    uc_rtc_get(cmdRTCGETT, t);
    uc_rtc_get(cmdRTCGETD, d1);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(12, t[0], "hodina");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(34, t[1], "minuta");
    TEST_ASSERT_TRUE_MESSAGE((t[2] >= 56) && (t[2] <= 58), "sekunda");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(d0, d1, 3, "datum se nezmění");
}


/* Neplatné hodnoty fw 0.26 nekontroluje a hlásí OK; emulace je nenastaví. */
void test_unicard_rtc_invalid_ignored(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_rtc_set(cmdRTCSETD, 1, 2, 2030 - 1980);
    uc_rtc_set(cmdRTCSETD, 31, 2, 2031 - 1980);     /* 31. 2. */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "neplatné datum = OK");
    uc_rtc_set(cmdRTCSETD, 5, 5, 1999 - 1980);      /* před 2000 */
    uc_rtc_set(cmdRTCSETT, 24, 0, 0);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "neplatný čas = OK");
    uint8_t d[3];
    uc_rtc_get(cmdRTCGETD, d);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, d[0], "datum se nezmění");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2030 - 1980, d[2], "rok se nezmění");
}


/* ================================================================
 * UC API rev 2 (uc3 v0.37): INFO 95h
 * ================================================================ */

/* fw 0.26 INFO nezná: ERROR. */
void test_unicard_info_absent_in_fw026(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(0);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdINFO);
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR, "fw 0.26: INFO = ERROR");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_DOUTRQ, "fw 0.26: INFO bez výstupu");
}


/* rev 2: INFO vrátí 16 B: revize 2, deska mk3b, příznaky UC v [8-9]. */
void test_unicard_info_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdINFO);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_DOUTRQ, uc_master_status() & (UNICARD_STS_DOUTRQ | UNICARD_STS_ERROR),
                                    "rev 2: INFO = výstup bez chyby");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(16, uc_read_status_byte(2), "INFO má 16 B");
    uint8_t info[16];
    for (int i = 0; i < 16; i++) info[i] = uc_recv_data();
    unicard_set_api_rev2(0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, info[0], "revize rozšíření 2");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_INFO_BOARD_MK3B, info[1], "deska mk3b");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, info[3], "bez Wi-Fi, NET, cloudu a NTP");
    uint16_t flags = info[8] | (info[9] << 8);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x01FF, flags, "příznaky UC jako firmware 0.37 (i cloud bez sítě)");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, info[10], "vydaný firmware: bez přípony [10]");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, info[11], "vydaný firmware: bez přípony [11]");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_DOUTRQ, "po 16 B konec výstupu");
}


/* v0.37 bez uc3 nic nedělá (uc1). Jen MZ-800. */
void test_unicard_info_rev2_needs_uc3(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    if (g_mzarch_platform_numeric != 800) return;

    unicard_set_api_rev2(1);
    unicard_set_fw(UNICARD_FW_UC1);
    int rev2 = unicard_get_api_rev2();
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdINFO);
    uint8_t sts = uc_master_status();
    unicard_set_fw(UNICARD_FW_UC3);
    unicard_set_api_rev2(0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rev2, "uc1: rev 2 neplatí");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, sts & UNICARD_STS_ERROR, "uc1: INFO = ERROR");
}


/* Text z cmdREV bez 0x0d (nejvýš size-1 znaků). */
static void uc_recv_rev(char *buf, size_t size)
{
    size_t n = 0;
    uc_send_cmd(cmdREV);
    while (n < size - 1) {
        uint8_t b = uc_recv_data();
        if (b == 0x0d) break;
        buf[n++] = (char)b;
    }
    buf[n] = ' ';
}


/* Verze uc3: v0.26 (default) hlásí REV "mz800emu v0.26" a REVD 0.26,
 * v0.37 (jen MZ-800) jako firmware mk3b 0.37 "MZ800 v0.37" a REVD 0.37
 * s UC API rev 2; jiná verze = v0.26. */
void test_unicard_uc3_version(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    char rev[64];
    uint8_t revd[4];

    unicard_set_uc3_version(UNICARD_FW_UC3_REV2_MINOR);
    unsigned minor = unicard_get_uc3_minor();
    int rev2 = unicard_get_api_rev2();
    uc_send_cmd(cmdRESET);
    uc_recv_rev(rev, sizeof(rev));
    uc_send_cmd(cmdREVD);
    for (int i = 0; i < 4; i++) revd[i] = uc_recv_data();
    unicard_set_uc3_version(UNICARD_FW_UC3_MINOR);

    if (g_mzarch_platform_numeric == 800) {
        TEST_ASSERT_EQUAL_UINT_MESSAGE(37, minor, "MZ-800: uc3 v0.37");
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, rev2, "v0.37 = UC API rev 2");
        TEST_ASSERT_EQUAL_STRING_MESSAGE("MZ800 v0.37", rev, "REV jako firmware 0.37");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, revd[0], "REVD major");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(37, revd[1], "REVD minor 37");
    } else {
        TEST_ASSERT_EQUAL_UINT_MESSAGE(26, minor, "v0.37 jen na MZ-800");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, rev2, "mimo MZ-800 bez rev 2");
    };
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UNICARD_FW_UC3_SUBTYPE, revd[2], "REVD subtype");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(unicard_get_uc3_pc_type(), revd[3], "REVD typ počítače");

    /* zpět v0.26 */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(26, unicard_get_uc3_minor(), "zpět v0.26");
    uc_recv_rev(rev, sizeof(rev));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("mz800emu v0.26", rev, "REV v0.26");
    uc_send_cmd(cmdREVD);
    for (int i = 0; i < 4; i++) revd[i] = uc_recv_data();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(26, revd[1], "REVD minor 26");

    /* jiná verze = v0.26 */
    unicard_set_uc3_version(30);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(26, unicard_get_uc3_minor(), "neznámá verze = v0.26");
}


/* Žádost o reset počítače (hlavní smyčka ji v testu nevyřídí): přečte a smaže. */
static bool uc_take_reset_request(void)
{
    bool r = g_mzarch_main.reset_request;
    g_mzarch_main.reset_request = false;
    return r;
}


/* GDGSYNC: příkaz 0Bh s bajtem v horní polovině adresy (OUT (C),A, v B). */
static void uc_gdgsync(uint8_t high)
{
    unicard_write_byte((uint16_t)((high << 8) | UNICARD_PORT_CMD), cmdGDGSYNC);
}


/* uc3 BOOT 07h: reset počítače (žádost pro hlavní smyčku), stav OK. */
void test_unicard_uc3_boot(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_take_reset_request();
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdBOOT);
    TEST_ASSERT_TRUE_MESSAGE(uc_take_reset_request(), "BOOT = reset počítače");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "BOOT = OK");
}


/* uc3 GDGSYNC 0Bh: 55h AAh FFh 00h v horním bajtu adresy resetuje počítač,
 * jiná hodnota (obraz VGA) ne; stav repozitáře se nemění (jako STSR). */
void test_unicard_uc3_gdgsync(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_take_reset_request();
    uc_send_cmd(cmdRESET);
    uc_send_cmd(0x7f);                          /* neznámý příkaz: ERROR */
    TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_ERROR, "předtím ERROR");
    uc_gdgsync(0x55);
    uc_gdgsync(0xaa);
    uc_gdgsync(0x10);                           /* obraz VGA: emulace ho nemá */
    uc_gdgsync(0xef);
    TEST_ASSERT_FALSE_MESSAGE(uc_take_reset_request(), "hodnota 10h počítač neresetuje");
    TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_ERROR, "GDGSYNC stav nemění");

    uc_gdgsync(0x55);
    uc_gdgsync(0x00);                           /* chybná posloupnost začne znovu */
    uc_gdgsync(0xff);
    uc_gdgsync(0x00);
    TEST_ASSERT_FALSE_MESSAGE(uc_take_reset_request(), "chybná posloupnost");

    uc_gdgsync(0x55);
    uc_gdgsync(0xaa);
    uc_gdgsync(0xff);
    uc_gdgsync(0x00);
    TEST_ASSERT_TRUE_MESSAGE(uc_take_reset_request(), "55h AAh FFh 00h = reset počítače");
    TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_ERROR, "stav pořád z neznámého příkazu");
}


/* uc3 INTCALLER 11h (1 B, emulace přerušení karty nemá: 0), USHELLON/OFF
 * a USARTBPS (4 B) hlásí OK. */
void test_unicard_uc3_intcaller_usart(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdINTCALLER);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_DOUTRQ, uc_master_status() & (UNICARD_STS_DOUTRQ | UNICARD_STS_ERROR),
                                    "INTCALLER = výstup bez chyby");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x00, uc_recv_data(), "INTCALLER = 0");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_DOUTRQ, "INTCALLER má 1 B");

    uc_send_cmd(cmdUSHELLON);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "USHELLON = OK");
    uc_send_cmd(cmdUSHELLOFF);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "USHELLOFF = OK");

    uc_send_cmd(cmdUSARTBPS);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "USARTBPS čeká parametry");
    uc_send_data(0x00);
    uc_send_data(0xc2);
    uc_send_data(0x01);
    uc_send_data(0x00);                         /* 115200 */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "USARTBPS = OK");
    uc_send_cmd(cmdGETCWD);                     /* po 4 B je příkaz hotový */
    char cwd[8];
    size_t n = 0;
    while ((n < sizeof(cwd) - 1) && (uc_master_status() & UNICARD_STS_DOUTRQ)) cwd[n++] = (char)uc_recv_data();
    cwd[n] = 0;
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(cwd, "/\r"), "další příkaz po USARTBPS (fw 0.26 hlásí 0:/)");
}


/* uc1 (rev.60) příkazy uc3 nezná: BOOT = ERROR a počítač se neresetuje. Jen MZ-800. */
void test_unicard_uc1_no_boot(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    if (g_mzarch_platform_numeric != 800) return;

    uc_take_reset_request();
    unicard_set_fw(UNICARD_FW_UC1);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdBOOT);
    uint8_t sts = uc_master_status();
    uc_gdgsync(0x55);
    uc_gdgsync(0xaa);
    uc_gdgsync(0xff);
    uc_gdgsync(0x00);
    bool reset = uc_take_reset_request();
    unicard_set_fw(UNICARD_FW_UC3);
    TEST_ASSERT_TRUE_MESSAGE(sts & UNICARD_STS_ERROR, "uc1: BOOT = ERROR");
    TEST_ASSERT_FALSE_MESSAGE(reset, "uc1: žádný reset");
}


/* ================================================================
 * UC API rev 2: chybové kódy ve stavu bajt 2 (podle MZPico)
 * ================================================================ */

/* Stav bajt 0 (ERROR), 2 (kód) a 3 (FRESULT) poslední operace. */
static void uc_err(uint8_t *err, uint8_t *code, uint8_t *fres)
{
    *err = uc_master_status() & UNICARD_STS_ERROR;
    *code = uc_read_status_byte(2);
    *fres = uc_read_status_byte(3);
}


/* rev 2: neznámý příkaz 1, bez souboru 4, chyba FatFS 6 s FRESULT, neplatná
 * jednotka 2, přetečení parametrů 3; úspěch chybu smaže. */
void test_unicard_err_codes_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t err, code, fres;
    unicard_set_api_rev2(1);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(0x7f);                          /* neznámý příkaz */
    uc_err(&err, &code, &fres);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, err, "neznámý příkaz = ERROR");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_NOT_IMPLEMENTED, code, "neznámý příkaz = 1");

    uc_send_cmd(cmdTELL);                       /* bez otevřeného souboru */
    uc_err(&err, &code, &fres);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_NO_FILE, code, "TELL bez souboru = 4");

    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str("/nothing.bin");
    uc_err(&err, &code, &fres);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_FATFS, code, "OPEN neexistujícího = 6");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_NO_FILE, fres, "FRESULT NO_FILE");

    uc_mount(9, "/x.dsk");
    uc_err(&err, &code, &fres);
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BAD_PARAM, code, "MOUNT jednotky 9 = 2");

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdCHDIR);
    /* buffer 255 B, 1 B na terminátor řetězce: 254. znak ho přeplní (další
     * bajty by už šly jako zápis do souboru) */
    for (int i = 0; i < 254; i++) uc_send_data('a');
    uc_err(&err, &code, &fres);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, err, "přetečení = ERROR");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_OVERFLOW, code, "přetečení = 3");

    uc_send_cmd(cmdRESET);
    uc_err(&err, &code, &fres);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, err, "úspěch chybu smaže");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_OK, fres, "rev 2: FRESULT jen poslední operace");
}


/* fw 0.26: při chybě je kód vždy 0. */
void test_unicard_err_codes_fw026_zero(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uint8_t err, code, fres;
    unicard_set_api_rev2(0);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str("/nothing.bin");
    uc_err(&err, &code, &fres);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, err, "OPEN neexistujícího = ERROR");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, code, "fw 0.26: kód 0");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_NO_FILE, fres, "FRESULT NO_FILE");
}


/* rev 2: vysunutí hlásí OK, i pro prázdnou jednotku; konfigurace se smaže. */
void test_unicard_mount_eject_rev2_ok(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    create_test_file("disk.dsk", 16);
    uc_mount(2, "/disk.dsk");
    uc_mount(2, "");
    uint8_t sts = uc_master_status();
    char *cfg = read_unicard_cfg("fd2.cfg");
    uc_mount(2, "");
    uint8_t sts_empty = uc_master_status();
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR, "rev 2: vysunutí = OK");
    TEST_ASSERT_NULL_MESSAGE(cfg, "fd2.cfg se smaže");
    g_free(cfg);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts_empty & UNICARD_STS_ERROR, "rev 2: vysunutí prázdné jednotky = OK");
}


/* rev 2 (Q14): neplatné datum nebo čas = ERROR 2, hodiny se nezmění; platné
 * datum OK. */
void test_unicard_rtc_invalid_rev2_error(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    uc_rtc_set(cmdRTCSETD, 1, 2, 2030 - 1980);
    uint8_t ok = uc_master_status() & UNICARD_STS_ERROR;
    uc_rtc_set(cmdRTCSETD, 31, 2, 2031 - 1980);
    uint8_t err_date = uc_master_status() & UNICARD_STS_ERROR;
    uint8_t code_date = uc_read_status_byte(2);
    uc_rtc_set(cmdRTCSETT, 12, 60, 0);
    uint8_t err_time = uc_master_status() & UNICARD_STS_ERROR;
    uint8_t d[3];
    uc_rtc_get(cmdRTCGETD, d);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, ok, "platné datum = OK");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, err_date, "neplatné datum = ERROR");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BAD_PARAM, code_date, "neplatné datum = 2");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, err_time, "neplatný čas = ERROR");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, d[0], "datum se nezmění");
}


/* ================================================================
 * UC API rev 2: MOUNTS 98h
 * ================================================================ */

/* Celý výstup MOUNTS (čte, dokud nezhasne DOUTRQ). */
static size_t uc_read_mounts(char *out, size_t cap)
{
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdMOUNTS);
    size_t n = 0;
    while ((n < cap - 1) && (uc_master_status() & UNICARD_STS_DOUTRQ)) {
        out[n++] = (char)uc_recv_data();
    }
    out[n] = '\0';
    return n;
}


/* Bez konfigurace všech 8 jednotek prázdných; po MOUNT jejich cesty. */
void test_unicard_mounts_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    char out[4096];
    uc_read_mounts(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("1:\r2:\r3:\r4:\rR:\rQ:\rE:\rS:\r", out, "všechny jednotky prázdné");

    create_test_file("disk.dsk", 16);
    uc_mount(1, "/disk.dsk");
    uc_mount(5, "/qd.mzq");                         /* neexistuje: chyba, cfg se ale zapíše */
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
    char *full = g_build_filename(s_test_sd_root, UNICARD_UNIMGR_DIR, "emm.cfg", NULL);
    g_file_set_contents(full, "/emm.dat\r\n", -1, NULL);   /* ručně psaná, s koncem řádku */
    g_free(full);
    uc_read_mounts(out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("1:\r2:/disk.dsk\r3:\r4:\rR:\rQ:/qd.mzq\rE:/emm.dat\rS:\r", out,
                                     "cesty z konfigurace jednotek");
}


/* Stav bajt 2 během MOUNTS = zbytek aktuálního řádku (firmware 0.37 ho vydává po řádcích). */
void test_unicard_mounts_status_line_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    create_test_file("disk.dsk", 16);
    uc_mount(1, "/disk.dsk");
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdMOUNTS);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, uc_read_status_byte(2), "1:\\r = 3 B");
    uc_recv_data();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, uc_read_status_byte(2), "zbytek řádku");
    uc_recv_data();
    uc_recv_data();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(12, uc_read_status_byte(2), "2:/disk.dsk\\r = 12 B");
}


/* Výstup delší než 255 B se přečte celý. */
void test_unicard_mounts_long_output(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    char path[120];
    memset(path, 'x', sizeof(path) - 1);
    path[0] = '/';
    path[sizeof(path) - 1] = '\0';
    for (uint8_t d = 0; d < 8; d++) uc_mount(d, path);
    unicard_set_managed_fdc(UNICARD_MANAGED_FDC_FDC0);
    char out[4096];
    size_t n = uc_read_mounts(out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(8 * (2 + strlen(path) + 1), n, "celý výstup (8 řádků)");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE('S', (uint8_t)out[n - 2 - strlen(path) - 1], "poslední řádek S:");
}


/* fw 0.26 MOUNTS nezná. */
void test_unicard_mounts_absent_in_fw026(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdMOUNTS);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & (UNICARD_STS_ERROR | UNICARD_STS_DOUTRQ),
                                    "fw 0.26: MOUNTS = ERROR");
}


/* ================================================================
 * UC API rev 2: COPY 9Ah
 * ================================================================ */

static void uc_copy(const char *src, const char *dst)
{
    uc_send_cmd(cmdCOPY);
    uc_send_str(src);
    uc_send_str(dst);
}


/* Kopie souboru (i jména v CP852), přepsání existujícího cíle. */
void test_unicard_copy_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    create_test_file("src.bin", 300);
    create_test_file("dst.bin", 7);
    uc_send_cmd(cmdRESET);
    uc_copy("/src.bin", "/dst.bin");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "COPY = OK");
    uint8_t buf[400];
    size_t n = read_file_content("dst.bin", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(300, n, "cíl přepsaný celým zdrojem");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(299 & 0xff, buf[299], "obsah");

    uc_copy("/src.bin", "/" CP852_FILE_CARD);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "COPY na jméno v CP852");
    n = read_file_content(CP852_FILE_UTF8, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(300, n, "cíl s UTF-8 jménem na hostiteli");
}


/* Chyby: 2 prázdná cesta, 5 otevřený soubor, 6 zdroj není / cíl je adresář. */
void test_unicard_copy_errors_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    create_test_file("src.bin", 3);
    uc_send_cmd(cmdRESET);
    uc_copy("/src.bin", "");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BAD_PARAM, uc_read_status_byte(2), "prázdný cíl = 2");
    uc_copy("/nothing.bin", "/x.bin");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_FATFS, uc_read_status_byte(2), "zdroj není = 6");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_NO_FILE, uc_read_status_byte(3), "FR_NO_FILE");
    uc_copy("/src.bin", "/" UNICARD_UNIMGR_DIR);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_DENIED, uc_read_status_byte(3), "cíl je adresář = FR_DENIED");
    uc_copy("/src.bin", "/SRC.BIN");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_DENIED, uc_read_status_byte(3), "tentýž soubor jinými písmeny = FR_DENIED");
    uc_copy("/src.bin", "src.bin");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_DENIED, uc_read_status_byte(3), "tentýž soubor relativně = FR_DENIED");
    uint8_t src_data[8];
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, read_file_content("src.bin", src_data, sizeof(src_data)), "zdroj nezkrácený");
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str("/src.bin");
    uc_copy("/src.bin", "/x.bin");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BUSY, uc_read_status_byte(2), "otevřený soubor = 5");
    uc_close();
}


/* Obsah souboru z create_test_file (bajt i = i & 0xff)? */
static int test_file_pattern_ok(const char *rel_path, size_t size)
{
    static uint8_t buf[262144];
    size_t n = read_file_content(rel_path, buf, sizeof(buf));
    if (n != size) return 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != (uint8_t)(i & 0xff)) return 0;
    }
    return 1;
}


/* Existuje soubor na SD? */
static int test_file_exists(const char *rel_path)
{
    char *full = g_build_filename(s_test_sd_root, rel_path, NULL);
    int e = g_file_test(full, G_FILE_TEST_EXISTS);
    g_free(full);
    return e;
}


/* COPY na pozadí: 200 000 B po kusech 64 KB, kus na každé čtení stavového
 * bajtu 0. Bit 6 a průběh v bajtu 2, mezitím ostatní příkazy chyba 5 a kopie
 * běží dál, na konci bit 6 zhasne a stav je OK. */
void test_unicard_copy_background_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    create_test_file("big.bin", 200000);
    uc_send_cmd(cmdRESET);
    uc_copy("/big.bin", "/big2.bin");
    uint8_t sts = uc_master_status();                       /* kus 1 */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_PROGRESS, sts & (UNICARD_STS_PROGRESS | UNICARD_STS_ERROR),
                                   "COPY běží na pozadí: bit 6, bez chyby");
    uint8_t pct = uc_read_status_byte(2);                   /* kus 2 */
    TEST_ASSERT_TRUE_MESSAGE((pct >= 30) && (pct <= 99), "bajt 2 = průběh v % (2 kusy ze 4)");
    uc_send_cmd(cmdTELL);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_PROGRESS | UNICARD_STS_ERROR,
                                   uc_master_status() & (UNICARD_STS_PROGRESS | UNICARD_STS_ERROR),
                                   "příkaz během kopie = chyba, kopie běží dál");   /* kus 3 */
    sts = uc_master_status();                               /* kus 4 = konec */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & (UNICARD_STS_PROGRESS | UNICARD_STS_ERROR),
                                   "konec: bit 6 zhasl, OK");
    TEST_ASSERT_TRUE_MESSAGE(test_file_pattern_ok("big2.bin", 200000), "kopie celá a stejná");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(cmdCOPY, uc_read_status_byte(1), "bajt 1 = COPY i po konci");
}


/* COPY na pozadí: odmítnutý příkaz má chybu 5, data také. */
void test_unicard_copy_background_busy_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    create_test_file("big.bin", 200000);
    uc_send_cmd(cmdRESET);
    uc_copy("/big.bin", "/big2.bin");
    (void) uc_master_status();
    uc_send_cmd(cmdINFO);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BUSY, uc_read_status_byte(2), "INFO během kopie = 5");
    uc_send_data(0x55);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BUSY, uc_read_status_byte(2), "data během kopie = 5");
    for (int i = 0; (i < 10) && (uc_master_status() & UNICARD_STS_PROGRESS); i++) {
    }
    TEST_ASSERT_TRUE_MESSAGE(test_file_pattern_ok("big2.bin", 200000), "kopie celá i po odmítnutých přístupech");
}


/* COPY na pozadí: STORNO a RESET kopii zruší, nedokončený cíl se smaže. */
void test_unicard_copy_background_cancel_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    create_test_file("big.bin", 200000);
    uint8_t cancel[2] = { cmdSTORNO, cmdRESET };
    for (int c = 0; c < 2; c++) {
        uc_send_cmd(cmdRESET);
        uc_copy("/big.bin", "/big3.bin");
        TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_PROGRESS, "kopie běží");
        TEST_ASSERT_TRUE_MESSAGE(test_file_exists("big3.bin"), "cíl rozpracovaný");
        uc_send_cmd(cancel[c]);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & (UNICARD_STS_PROGRESS | UNICARD_STS_ERROR),
                                       c ? "RESET: kopie zrušená, OK" : "STORNO: kopie zrušená, OK");
        TEST_ASSERT_FALSE_MESSAGE(test_file_exists("big3.bin"), "nedokončený cíl smazaný");
    }
    TEST_ASSERT_TRUE_MESSAGE(test_file_pattern_ok("big.bin", 200000), "zdroj nedotčený");
}


/* ================================================================
 * UC API rev 2: svazek cloud: (složka hostitele MZ800EMU_UNICARD_CLOUD)
 * ================================================================ */

static char *s_cloud_root = NULL;

/* Soubor v cloudu se vzorkem create_test_file (bajt i = i & 0xff). */
static void cloud_file(const char *rel, size_t size)
{
    char *full = g_build_filename(s_cloud_root, rel, NULL);
    FILE *f = g_fopen(full, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "soubor v cloudu");
    for (size_t i = 0; i < size; i++) {
        uint8_t b = (uint8_t)(i & 0xff);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    g_free(full);
}

/* Cloud: featured/{Game.mzf 428 B, B.mzf 3 B, big.mzf 20000 B}, tools/ (prázdná), top.txt 5 B. */
static void cloud_setup(void)
{
    s_cloud_root = g_dir_make_tmp("mztest-cloud-XXXXXX", NULL);
    TEST_ASSERT_NOT_NULL_MESSAGE(s_cloud_root, "složka cloudu");
    char *d = g_build_filename(s_cloud_root, "featured", NULL);
    g_mkdir(d, 0755);
    g_free(d);
    d = g_build_filename(s_cloud_root, "tools", NULL);
    g_mkdir(d, 0755);
    g_free(d);
    cloud_file("featured/Game.mzf", 428);
    cloud_file("featured/B.mzf", 3);
    cloud_file("featured/big.mzf", 20000);
    cloud_file("top.txt", 5);
    g_setenv("MZ800EMU_UNICARD_CLOUD", s_cloud_root, TRUE);
    unicard_set_api_rev2(1);
    uc_send_cmd(cmdRESET);
}

static void cloud_teardown(void)
{
    g_unsetenv("MZ800EMU_UNICARD_CLOUD");
    if (s_cloud_root) {
        rmtree(s_cloud_root);
        g_free(s_cloud_root);
        s_cloud_root = NULL;
    }
}

/* Čte stav, dokud běží požadavek na pozadí (bit 6). */
static uint8_t cloud_wait(int *polls)
{
    uint8_t s;
    int i = 0;
    do {
        s = uc_master_status();
        i++;
    } while ((s & UNICARD_STS_PROGRESS) && (i < 1000));
    if (polls) *polls = i;
    return s;
}

/* FILELIST cloudu (počká na výpis). */
static size_t cloud_filelist(const char *path, char *out, size_t cap)
{
    uc_send_cmd(cmdFILELIST);
    uc_send_str(path);
    cloud_wait(NULL);
    size_t n = 0;
    while ((n < cap - 1) && (uc_master_status() & UNICARD_STS_READDIR)) {
        out[n++] = (char)uc_recv_data();
    }
    out[n] = '\0';
    return n;
}


/* FILELIST a READDIR cloudu: na pozadí, pak záznamy (složky napřed); neexistující složka = prázdný výpis. */
void test_unicard_cloud_list_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    cloud_setup();

    uc_send_cmd(cmdFILELIST);
    uc_send_str("cloud:/");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_PROGRESS, uc_master_status() & (UNICARD_STS_PROGRESS | UNICARD_STS_ERROR |
                                   UNICARD_STS_READDIR), "výpis na pozadí: bit 6, bez záznamů");
    int polls;
    uint8_t sts = cloud_wait(&polls);
    TEST_ASSERT_TRUE_MESSAGE(polls >= 2, "výpis trvá několik čtení stavu");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_READDIR, sts & (UNICARD_STS_READDIR | UNICARD_STS_ERROR), "záznamy");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(cmdFILELIST, uc_read_status_byte(1), "bajt 1 = FILELIST");
    char out[1024];
    size_t n = 0;
    while ((n < sizeof(out) - 1) && (uc_master_status() & UNICARD_STS_READDIR)) out[n++] = (char)uc_recv_data();
    out[n] = 0;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("featured/\r0\rtools/\r0\rtop.txt\r5\r", out, "kořen cloudu");

    cloud_filelist("cloud:/featured", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("B.mzf\r3\rbig.mzf\r20000\rGame.mzf\r428\r", out, "složka, abecedně");
    cloud_filelist("cloud:/none", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", out, "neexistující složka = prázdný výpis");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "bez chyby");
    cloud_filelist("cloud:/../x", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", out, "'..' ven z cloudu nevede");

    uc_send_cmd(cmdREADDIR);
    uc_send_str("cloud:/featured");
    cloud_wait(NULL);
    uint32_t size;
    uint8_t attr;
    char name[64];
    TEST_ASSERT_TRUE_MESSAGE(uc_read_dir_filinfo(&size, &attr, name, sizeof(name)), "READDIR záznam");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("B.mzf", name, "jméno");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, size, "velikost");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x20, attr, "soubor");
    TEST_ASSERT_TRUE_MESSAGE(uc_read_dir_filinfo(&size, &attr, name, sizeof(name)), "druhý");
    TEST_ASSERT_TRUE_MESSAGE(uc_read_dir_filinfo(&size, &attr, name, sizeof(name)), "třetí");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Game.mzf", name, "třetí jméno");
    TEST_ASSERT_FALSE_MESSAGE(uc_read_dir_filinfo(&size, &attr, name, sizeof(name)), "konec");

    cloud_teardown();
}


/* OPEN z cloudu: stažení na pozadí s průběhem, pak čtení jako obyčejný soubor; zápis a chybějící soubor. */
void test_unicard_cloud_open_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    cloud_setup();

    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str("cloud:/featured/big.mzf");
    uint8_t last = 0, rising = 1;
    int polls = 0;
    for (;;) {
        /* bajty 0-2 jedním čtením: průběh platí, jen když bajt 0 ještě hlásí bit 6 */
        unicard_write_byte(UNICARD_PORT_CMD, cmdSTSR);
        uint8_t b0 = unicard_read_byte(UNICARD_PORT_CMD);
        (void) unicard_read_byte(UNICARD_PORT_CMD);
        uint8_t pct = unicard_read_byte(UNICARD_PORT_CMD);
        if (!(b0 & UNICARD_STS_PROGRESS) || (polls >= 1000)) break;
        if (pct < last) rising = 0;
        last = pct;
        polls++;
    }
    TEST_ASSERT_TRUE_MESSAGE(polls >= 10, "stahování po kusech (1 KB na čtení stavu)");
    TEST_ASSERT_TRUE_MESSAGE(rising && (last > 50), "průběh v bajtu 2 roste");
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_READ, sts & (UNICARD_STS_READ | UNICARD_STS_ERROR | UNICARD_STS_PROGRESS),
                                   "soubor otevřený pro čtení");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(cmdOPEN, uc_read_status_byte(1), "bajt 1 = OPEN");
    int ok = 1;
    for (int i = 0; i < 20000; i++) if (uc_recv_data() != (uint8_t)(i & 0xff)) ok = 0;
    TEST_ASSERT_TRUE_MESSAGE(ok, "obsah stejný");
    uc_close();
    TEST_ASSERT_TRUE_MESSAGE(test_file_pattern_ok("unicard/cloud.tmp", 20000), "kopie v /unicard/cloud.tmp");

    uc_send_cmd(cmdOPEN);
    uc_send_data(0x0a); /* FA_WRITE | FA_CREATE_ALWAYS */
    uc_send_str("cloud:/featured/Game.mzf");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_FATFS, uc_read_status_byte(2), "zápis do cloudu = 6");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_WRITE_PROTECTED, uc_read_status_byte(3), "FR_WRITE_PROTECTED");

    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01);
    uc_send_str("cloud:/featured/none.mzf");
    TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_PROGRESS, "dotaz na server");
    sts = cloud_wait(NULL);
    TEST_ASSERT_TRUE_MESSAGE(sts & UNICARD_STS_ERROR, "chybějící soubor = chyba");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_CLOUD, uc_read_status_byte(2), "kód 7 CLOUD");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(CLOUD_ERR_FAILED, uc_read_status_byte(3), "CLOUD_ERR_FAILED (404)");

    cloud_teardown();
}


/* COPY z cloudu na SD na pozadí; do cloudu ne; během stahování BUSY, STORNO a RESET zruší. */
void test_unicard_cloud_copy_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    cloud_setup();

    uc_copy("cloud:/featured/big.mzf", "/big.mzf");
    TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_PROGRESS, "COPY z cloudu na pozadí");
    uc_send_cmd(cmdINFO);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BUSY, uc_read_status_byte(2), "INFO během stahování = 5");
    uc_send_data(0x55);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BUSY, uc_read_status_byte(2), "data během stahování = 5");
    uint8_t sts = cloud_wait(NULL);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & UNICARD_STS_ERROR, "COPY OK");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(cmdCOPY, uc_read_status_byte(1), "bajt 1 = COPY");
    TEST_ASSERT_TRUE_MESSAGE(test_file_pattern_ok("big.mzf", 20000), "kopie celá");

    uc_copy("/big.mzf", "cloud:/x.mzf");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_WRITE_PROTECTED, uc_read_status_byte(3), "do cloudu = FR_WRITE_PROTECTED");

    uint8_t cancel[2] = { cmdSTORNO, cmdRESET };
    for (int c = 0; c < 2; c++) {
        uc_copy("cloud:/featured/big.mzf", "/part.mzf");
        TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_PROGRESS, "stahuje");
        TEST_ASSERT_TRUE_MESSAGE(test_file_exists("part.mzf"), "cíl rozpracovaný");
        uc_send_cmd(cancel[c]);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & (UNICARD_STS_PROGRESS | UNICARD_STS_ERROR),
                                       c ? "RESET zrušil" : "STORNO zrušilo");
        TEST_ASSERT_FALSE_MESSAGE(test_file_exists("part.mzf"), "nedokončený cíl smazaný");
    }
    uc_send_cmd(cmdFILELIST);
    uc_send_str("cloud:/");
    uc_send_cmd(cmdSTORNO);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & (UNICARD_STS_PROGRESS | UNICARD_STS_READDIR),
                                   "STORNO zruší i výpis");

    cloud_teardown();
}


/* LISTVOL a INFO: cloud jen s nastavenou složkou; bez ní chyba 7 / 1. */
void test_unicard_cloud_listvol_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    cloud_setup();

    uc_send_cmd(cmdLISTVOL);
    char out[64];
    size_t n = 0;
    while ((n < sizeof(out) - 1) && (uc_master_status() & UNICARD_STS_DOUTRQ)) out[n++] = (char)uc_recv_data();
    out[n] = 0;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("sd:\rcloud:\r", out, "LISTVOL s cloudem");
    uc_send_cmd(cmdINFO);
    uint8_t info[16];
    for (int i = 0; i < 16; i++) info[i] = uc_recv_data();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01, info[3] & 0x01, "INFO [3] bit 0 = cloud");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, info[8] & 0x80, "INFO [8] UC_INFO_CLOUD");

    /* složka cloudu chybí: karta cloud má, ale není síť */
    char *gone = g_build_filename(s_cloud_root, "missing", NULL);
    g_setenv("MZ800EMU_UNICARD_CLOUD", gone, TRUE);
    g_free(gone);
    uc_send_cmd(cmdINFO);
    for (int i = 0; i < 16; i++) info[i] = uc_recv_data();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, info[3] & 0x01, "bez sítě: INFO [3] bit 0 = 0");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, info[8] & 0x80, "UC_INFO_CLOUD zůstává");
    uc_send_cmd(cmdFILELIST);
    uc_send_str("cloud:/");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(CLOUD_ERR_NOT_CONNECTED, uc_read_status_byte(3), "bez sítě = CLOUD_ERR_NOT_CONNECTED");

    g_unsetenv("MZ800EMU_UNICARD_CLOUD");
    uc_send_cmd(cmdINFO);
    for (int i = 0; i < 16; i++) info[i] = uc_recv_data();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, info[8] & 0x80, "bez MZ800EMU_UNICARD_CLOUD svazek cloud: zůstává (fw 0.37)");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, info[3] & 0x01, "bez MZ800EMU_UNICARD_CLOUD cloud nedostupný");
    uc_send_cmd(cmdLISTVOL);
    n = 0;
    while ((n < sizeof(out) - 1) && (uc_master_status() & UNICARD_STS_DOUTRQ)) out[n++] = (char)uc_recv_data();
    out[n] = 0;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("sd:\r", out, "LISTVOL bez cloudu");
    uc_send_cmd(cmdFILELIST);
    uc_send_str("cloud:/");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_CLOUD, uc_read_status_byte(2), "bez cloudu = 7");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(CLOUD_ERR_NOT_CONNECTED, uc_read_status_byte(3), "CLOUD_ERR_NOT_CONNECTED");

    unicard_set_api_rev2(0);
    g_setenv("MZ800EMU_UNICARD_CLOUD", s_cloud_root, TRUE);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdLISTVOL);
    TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_ERROR, "fw 0.26 LISTVOL nezná");

    cloud_teardown();
}


/* fw 0.26 COPY nezná. */
void test_unicard_copy_absent_in_fw026(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    create_test_file("src.bin", 3);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdCOPY);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & (UNICARD_STS_ERROR | UNICARD_STS_BUSY),
                                    "fw 0.26: COPY = ERROR, bez parametrů");
    char *full = g_build_filename(s_test_sd_root, "x.bin", NULL);
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(full, G_FILE_TEST_EXISTS), "nic se nezkopírovalo");
    g_free(full);
}


/* ================================================================
 * UC API rev 2: GETCONFIG 92h, SETCONFIG 99h
 * ================================================================ */

/* Záznamy GETCONFIG sekce jako "KLÍČ=hodnota|..." (čte, dokud svítí bit 2). */
static int uc_getconfig(const char *section, char *out, size_t cap)
{
    uc_send_cmd(cmdGETCONFIG);
    uc_send_str(section);
    out[0] = '\0';
    int n = 0;
    while (uc_master_status() & UNICARD_STS_READDIR) {
        char rec[UNICARD_INI_REC_SIZE];
        for (int i = 0; i < UNICARD_INI_REC_SIZE; i++) rec[i] = (char)uc_recv_data();
        rec[UNICARD_INI_KEY_SIZE - 1] = '\0';
        rec[UNICARD_INI_REC_SIZE - 1] = '\0';
        g_strlcat(out, rec, cap);
        g_strlcat(out, "=", cap);
        g_strlcat(out, rec + UNICARD_INI_KEY_SIZE, cap);
        g_strlcat(out, "|", cap);
        n++;
    }
    return n;
}

static void uc_setconfig(const char *section, const char *key, const char *value)
{
    uc_send_cmd(cmdSETCONFIG);
    uc_send_str(section);
    uc_send_str(key);
    uc_send_str(value);
}

static void write_unicard_cfg(const char *name, const char *content)
{
    char *full = g_build_filename(s_test_sd_root, UNICARD_UNIMGR_DIR, name, NULL);
    g_file_set_contents(full, content, -1, NULL);
    g_free(full);
}


/* Soubor konfigurace do 4 KB jako firmware 0.37 (vyrovnávací paměť souboru):
 * větší = FR_DENIED pro GETCONFIG i SETCONFIG, SETCONFIG s výsledkem nad 4 KB
 * také (soubor se nezmění). */
void test_unicard_config_4kb_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    char out[256];
    char *big = g_malloc(4097 + 1);
    memcpy(big, "IPADDR=10.0.0.5\n;", 17);
    memset(big + 17, 'x', 4097 - 17 - 1);
    big[4096] = '\n';
    big[4097] = 0;
    write_unicard_cfg("network.cfg", big);      /* 4097 B */
    uc_send_cmd(cmdRESET);
    uc_getconfig("NETWORK", out, sizeof(out));
    TEST_ASSERT_TRUE_MESSAGE(uc_master_status() & UNICARD_STS_ERROR, "GETCONFIG souboru 4097 B = chyba");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_FATFS, uc_read_status_byte(2), "kód 6");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_DENIED, uc_read_status_byte(3), "FR_DENIED");
    uc_setconfig("NETWORK", "DHCP", "1");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_DENIED, uc_read_status_byte(3), "SETCONFIG souboru 4097 B = FR_DENIED");

    big[4095] = '\n';
    big[4096] = 0;                              /* 4096 B: vejde se */
    write_unicard_cfg("network.cfg", big);
    uc_getconfig("NETWORK", out, sizeof(out));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "GETCONFIG 4096 B = OK");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "IPADDR=10.0.0.5|"), "záznam z 4096 B");
    uc_setconfig("NETWORK", "DHCP", "1");       /* výsledek přes 4 KB */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(FR_DENIED, uc_read_status_byte(3), "SETCONFIG s výsledkem nad 4 KB = FR_DENIED");
    char *now = read_unicard_cfg("network.cfg");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4096, (uint32_t)strlen(now), "soubor se nezměnil");
    g_free(now);
    g_free(big);
}


/* GETCONFIG čte sekce jako firmware: network.cfg i bez hlavičky, unicard.cfg
 * po sekcích; neznámá sekce = prázdný proud. */
void test_unicard_getconfig_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    write_unicard_cfg("network.cfg", "; net\r\nIPADDR = 10.0.0.5 ; static\r\nFTP_NAME=mz\r\n");
    write_unicard_cfg("unicard.cfg", "[MZ]\nVGA=1\n[MZ800]\nborder = 3\n");
    char out[1024];
    uc_send_cmd(cmdRESET);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, uc_getconfig("network", out, sizeof(out)), "2 záznamy NETWORK");
    TEST_ASSERT_EQUAL_STRING("IPADDR=10.0.0.5|FTP_NAME=mz|", out);
    uc_getconfig("MZ800", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("BORDER=3|", out);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, uc_getconfig("NOTHING", out, sizeof(out)), "neznámá sekce = prázdný proud");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "prázdný proud bez chyby");
}


/* STATUS: použité adresy (z network.cfg, jinak výchozí firmwaru), LINK, DHCP. */
void test_unicard_getconfig_status_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    write_unicard_cfg("network.cfg", "IPADDR=10.0.0.5\nDHCP=1\n");
    char out[1024];
    uc_send_cmd(cmdRESET);
    uc_getconfig("STATUS", out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "IPADDR=10.0.0.5|"), "IP z network.cfg");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "NETMASK=255.255.255.0|"), "výchozí maska firmwaru");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "LINK=1|"), "LINK");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "DHCP=1|"), "DHCP");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "VERSION=MZ800 v0.37|"), "VERSION jako REV");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "NTP=OFF|"), "bez TIMEZONE NTP=OFF");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "CLOUD=OFF|"), "bez cloudu CLOUD=OFF (firmware 0.37 vždy)");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "OPENCPU=0|"), "OPENCPU=0 (emulátor nespustí ARM)");
    write_unicard_cfg("network.cfg", "TIMEZONE=CET-1CEST,M3.5.0,M10.5.0/3\n");
    uc_getconfig("STATUS", out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "NTP=KEPT|"), "s TIMEZONE: hodiny emulace nastavené (KEPT)");
    write_unicard_cfg("network.cfg", "TIMEZONE=CET-1CEST,M13.5.0,M10.5.0/3\n");
    uc_getconfig("STATUS", out, sizeof(out));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(out, "NTP=BAD TIMEZONE|"), "chybný TIMEZONE (měsíc 13)");
}


/* Hodiny z NTP (INFO [3] 10h) jen s TIMEZONE, který přijme firmware 0.37 (řetězec POSIX TZ). */
void test_unicard_info_ntp_timezone(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    static const struct { const char *tz; int ok; } cases[] = {
        { "CET-1CEST,M3.5.0,M10.5.0/3", 1 },
        { "  CET-1CEST,M3.5.0,M10.5.0/3  ", 1 },     /* mezery firmware ořízne */
        { "UTC0", 1 },
        { "<+03>-3", 1 },
        { "EST5EDT", 1 },                           /* letní čas s výchozími pravidly */
        { "NZST-12NZDT-13,M9.5.0,M4.1.0/3", 1 },
        { "AAA-1BBB,J60/1:30,300", 1 },
        { "CET", 0 },                               /* bez posunu */
        { "XY-1", 0 },                              /* jméno kratší než 3 */
        { "CET-168", 0 },                           /* hodiny nad 167 */
        { "CET-1:60", 0 },
        { "CET-1CEST,M3.5.0", 0 },                  /* chybí konec */
        { "CET-1CEST,M3.6.0,M10.5.0", 0 },          /* týden 6 */
        { "CET-1CEST,J0,M10.5.0", 0 },              /* Jn od 1 */
        { "CET-1x", 0 },                            /* jméno letního času kratší než 3 */
    };
    uint8_t info[16];
    unicard_set_api_rev2(1);
    uc_send_cmd(cmdRESET);
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char cfg[128];
        snprintf(cfg, sizeof(cfg), "TIMEZONE=%s\n", cases[c].tz);
        write_unicard_cfg("network.cfg", cfg);
        uc_send_cmd(cmdINFO);
        for (int i = 0; i < 16; i++) info[i] = uc_recv_data();
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(cases[c].ok ? 0x10 : 0x00, info[3] & 0x10, cases[c].tz);
    }
    write_unicard_cfg("network.cfg", "IPADDR=10.0.0.5\n");
    uc_send_cmd(cmdINFO);
    for (int i = 0; i < 16; i++) info[i] = uc_recv_data();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, info[3] & 0x10, "bez TIMEZONE bez NTP");
}


/* SETCONFIG přepíše, přidá a smaže klíč; GETCONFIG to hned vidí. */
void test_unicard_setconfig_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    write_unicard_cfg("network.cfg", "; net\nIPADDR=10.0.0.5\nNETMASK=255.0.0.0\n");
    char out[1024];
    uc_send_cmd(cmdRESET);
    uc_setconfig("NETWORK", "ipaddr", "10.0.0.6");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "SETCONFIG = OK");
    uc_setconfig("NETWORK", "NETMASK", "");
    uc_setconfig("NETWORK", "DHCP", "1");
    uc_getconfig("NETWORK", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("IPADDR=10.0.0.6|DHCP=1|", out);
    char *full = g_build_filename(s_test_sd_root, UNICARD_UNIMGR_DIR, "network.cfg", NULL);
    char *data = NULL;
    g_file_get_contents(full, &data, NULL, NULL);
    g_free(full);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("; net\nIPADDR=10.0.0.6\nDHCP=1\n", data, "komentář zůstal");
    g_free(data);

    uc_setconfig("MZ800", "BORDER", "2");           /* unicard.cfg ani sekce nejsou */
    uc_getconfig("MZ800", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("BORDER=2|", out);
}


/* Chyby SETCONFIG: 2 prázdný klíč, STATUS, neznámá sekce; 5 otevřený soubor;
 * fw 0.26 příkazy nezná. */
void test_unicard_setconfig_errors(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    uc_send_cmd(cmdRESET);
    uc_setconfig("NETWORK", "", "x");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BAD_PARAM, uc_read_status_byte(2), "prázdný klíč = 2");
    uc_setconfig("STATUS", "IPADDR", "1.2.3.4");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BAD_PARAM, uc_read_status_byte(2), "STATUS jen ke čtení = 2");
    uc_setconfig("WHATEVER", "A", "1");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BAD_PARAM, uc_read_status_byte(2), "neznámá sekce = 2");
    create_test_file("f.bin", 1);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str("/f.bin");
    uc_setconfig("NETWORK", "IPADDR", "1.2.3.4");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BUSY, uc_read_status_byte(2), "otevřený soubor = 5");
    uc_send_cmd(cmdGETCONFIG);
    uc_send_str("NETWORK");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(UC_ERR_BUSY, uc_read_status_byte(2), "GETCONFIG s otevřeným souborem = 5");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_READ, uc_master_status() & (UNICARD_STS_READ | UNICARD_STS_READDIR),
                                    "soubor zůstal otevřený, proud neběží");
    uc_close();

    unicard_set_api_rev2(0);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdGETCONFIG);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & (UNICARD_STS_ERROR | UNICARD_STS_BUSY),
                                    "fw 0.26: GETCONFIG = ERROR");
}


/* ================================================================
 * UC API rev 2: RESTART 9Bh
 * ================================================================ */

/* RESTART zavře soubor, vrátí CWD na / a hlásí OK; fw 0.26 ho nezná. */
void test_unicard_restart_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    unicard_set_api_rev2(1);
    create_test_file("f.bin", 4);
    char *full = g_build_filename(s_test_sd_root, "sub", NULL);
    g_mkdir(full, 0755);
    g_free(full);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdCHDIR);
    uc_send_str("/sub");
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str("/f.bin");
    uc_send_cmd(cmdRESTART);
    uint8_t sts = uc_master_status();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, sts & (UNICARD_STS_ERROR | UNICARD_STS_READ), "RESTART = OK, soubor zavřený");
    recv_cmdGETCWD_expect("0:/\x0d");

    unicard_set_api_rev2(0);
    uc_send_cmd(cmdRESTART);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & UNICARD_STS_ERROR, "fw 0.26: RESTART = ERROR");
}


/* rev 2: vzdálené klávesy RKEY 9Ch (fronta ze souboru hostitele MZ800EMU_UNICARD_RKEY): kódy v pořadí, pak 0;
 * příznak 0100h v INFO; otevřený soubor a jeho čtení RKEY neruší; vyprázdnění fronty (reset počítače); bez rev 2
 * je 9Ch neznámý příkaz. */
void test_unicard_rkey_rev2(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);

    char *keys = g_build_filename(s_test_sd_root, "rkey.bin", NULL);
    const uint8_t codes[3] = { 0x41, 0x8F, 0x0D };
    g_setenv("MZ800EMU_UNICARD_RKEY", keys, TRUE);
    unicard_set_api_rev2(1);
    unimgr_rkey_clear();
    create_test_file("f.bin", 4);
    uc_send_cmd(cmdRESET);

    uc_send_cmd(cmdINFO);
    uint8_t info[16];
    for (int i = 0; i < 16; i++) info[i] = uc_recv_data();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01, info[9] & 0x01, "INFO: příznak RKEY 0100h");

    uc_send_cmd(cmdRKEY);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_recv_data(), "prázdná fronta: 0");

    g_file_set_contents(keys, (const gchar *)codes, sizeof(codes), NULL);
    uc_send_cmd(cmdOPEN);
    uc_send_data(0x01); /* FA_READ */
    uc_send_str("/f.bin");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_recv_data(), "soubor: bajt 0");
    for (int i = 0; i < 3; i++) {
        uc_send_cmd(cmdRKEY);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(codes[i], uc_recv_data(), "klávesy v pořadí");
    }
    TEST_ASSERT_FALSE_MESSAGE(g_file_test(keys, G_FILE_TEST_EXISTS), "soubor kláves přečtený a smazaný");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x01, uc_recv_data(), "soubor pokračuje bajtem 1");
    uc_send_cmd(cmdRKEY);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_recv_data(), "fronta prázdná");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x02, uc_recv_data(), "soubor pokračuje bajtem 2");
    uc_send_cmd(cmdRKEY);
    uc_send_cmd(cmdSTSR);   /* jiný zápis ohlášení zruší: čtení dat je zase soubor */
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x03, uc_recv_data(), "po zrušeném RKEY bajt 3 souboru");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_master_status() & UNICARD_STS_ERROR, "RKEY bez chyby");

    g_file_set_contents(keys, (const gchar *)codes, sizeof(codes), NULL);
    uc_send_cmd(cmdRKEY);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(codes[0], uc_recv_data(), "fronta z nového souboru");
    unimgr_rkey_clear();
    uc_send_cmd(cmdRKEY);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, uc_recv_data(), "vyprázdněná fronta (reset počítače)");

    unicard_set_api_rev2(0);
    uc_send_cmd(cmdRESET);
    uc_send_cmd(cmdRKEY);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(UNICARD_STS_ERROR, uc_master_status() & UNICARD_STS_ERROR, "fw 0.26: 9Ch = ERROR");
    g_unsetenv("MZ800EMU_UNICARD_RKEY");
    g_free(keys);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_unicard_setup_creates_sd_structure);
    RUN_TEST(test_unicard_smoke_cmdRESET);
    RUN_TEST(test_unicard_smoke_cmdREV);
    RUN_TEST(test_unicard_smoke_cmdGETCWD);
    RUN_TEST(test_unicard_cmdGETCWD_subdir_single_slash);
    RUN_TEST(test_unicard_parser_BS_via_cmdOPEN);
    RUN_TEST(test_unicard_parser_BS_open_existing);
    RUN_TEST(test_unicard_cmdSIZE);
    RUN_TEST(test_unicard_status_eof_at_size);
    RUN_TEST(test_unicard_uc3_long_name_invalid);
    RUN_TEST(test_unicard_dns_resolving);
    RUN_TEST(test_unicard_uc3_wait_states);
    RUN_TEST(test_unicard_cloud_dir_ini_rev2);
    RUN_TEST(test_unicard_cmdTELL);
    RUN_TEST(test_unicard_cmdSTAT_file);
    RUN_TEST(test_unicard_cmdSTAT_dir);
    RUN_TEST(test_unicard_cmdSTAT_missing);
    RUN_TEST(test_unicard_cmdGETFREE);
    RUN_TEST(test_unicard_cmdSEEK_set);
    RUN_TEST(test_unicard_cmdSEEK_end);
    RUN_TEST(test_unicard_cmdSEEK_rel);
    RUN_TEST(test_unicard_cmdSEEK_no_file);
    RUN_TEST(test_unicard_write_create_new);
    RUN_TEST(test_unicard_write_existing_FA_WRITE);
    RUN_TEST(test_unicard_write_FA_CREATE_ALWAYS_new);
    RUN_TEST(test_unicard_write_FA_CREATE_ALWAYS_truncate);
    RUN_TEST(test_unicard_write_FA_OPEN_ALWAYS_existing_preserves);
    RUN_TEST(test_unicard_write_FA_OPEN_ALWAYS_creates_new);
    RUN_TEST(test_unicard_cmdTRUNC);
    RUN_TEST(test_unicard_cmdSYNC);
    RUN_TEST(test_unicard_cmdMKDIR);
    RUN_TEST(test_unicard_cmdMKDIR_exists);
    RUN_TEST(test_unicard_cmdUNLINK_file);
    RUN_TEST(test_unicard_cmdUNLINK_missing);
    RUN_TEST(test_unicard_cmdRENAME);
    RUN_TEST(test_unicard_cmdCHMOD_noop);
    RUN_TEST(test_unicard_cmdCHMOD_missing);
    RUN_TEST(test_unicard_cmdUTIME);
    RUN_TEST(test_unicard_cmdUTIME_missing);
    RUN_TEST(test_unicard_cmdREADDIR);
    RUN_TEST(test_unicard_cmdREADDIR_dir_attribute);

    /* FatFS 8.3 SFN alias generátor (unicard_sfn). */
    RUN_TEST(test_unicard_sfn_lossy_long_name);
    RUN_TEST(test_unicard_sfn_uc3_pure_83_uppercase_filled);
    RUN_TEST(test_unicard_sfn_83_lowercase_uppercased);
    RUN_TEST(test_unicard_sfn_space_in_name);
    RUN_TEST(test_unicard_sfn_long_base);
    RUN_TEST(test_unicard_sfn_collision_numbering);
    RUN_TEST(test_unicard_sfn_uc1_pure_83_filled);
    RUN_TEST(test_unicard_sfn_uc1_lowercase_preserved);
    RUN_TEST(test_unicard_sfn_uc1_mixed_uppercased);
    RUN_TEST(test_unicard_sfn_uc1_lossy);
    RUN_TEST(test_unicard_sfn_has_lfn_flag);
    RUN_TEST(test_unicard_cmdSTAT_sfn_alias);
    RUN_TEST(test_unicard_cmdREADDIR_sfn_alias);
    RUN_TEST(test_unicard_cmdREADDIR_uc3_lfn_always);
    RUN_TEST(test_unicard_cmdREADDIR_uc1_lfn_conditional);
    RUN_TEST(test_unicard_cmdREADDIR_uc1_dotdot);

    /* SD jail (directory traversal protection). */
    RUN_TEST(test_unicard_jail_basic_paths);
    RUN_TEST(test_unicard_jail_escape_blocked);
    RUN_TEST(test_unicard_jail_dot_segments);
    RUN_TEST(test_unicard_jail_backslash_traversal);
    RUN_TEST(test_unicard_jail_via_cmdSTAT);

    /* R/O režim emulované SD karty. */
    RUN_TEST(test_unicard_readonly_default_off);
    RUN_TEST(test_unicard_readonly_blocks_write);
    RUN_TEST(test_unicard_readonly_blocks_mkdir);
    RUN_TEST(test_unicard_readonly_blocks_unlink);
    RUN_TEST(test_unicard_readonly_blocks_rename);
    RUN_TEST(test_unicard_readonly_blocks_truncate);
    RUN_TEST(test_unicard_readonly_persists_session);
    RUN_TEST(test_unicard_readonly_allows_read);

    /* Jména v kódové stránce karty (uc3: CP852). */
    RUN_TEST(test_unicard_cp852_filelist);
    RUN_TEST(test_unicard_cp852_open);
    RUN_TEST(test_unicard_cp852_stat);
    RUN_TEST(test_unicard_cp852_length_in_card_cp);
    RUN_TEST(test_unicard_cp852_chdir_getcwd);
    RUN_TEST(test_unicard_uc1_names_unchanged);

    /* Jména, která se nevejdou (uc3: 8.3 alias). */
    RUN_TEST(test_unicard_alias_filelist);
    RUN_TEST(test_unicard_alias_open_stat);
    RUN_TEST(test_unicard_alias_readdir);
    RUN_TEST(test_unicard_alias_chdir_getcwd);
    RUN_TEST(test_unicard_alias_of_name_that_fits);
    RUN_TEST(test_unicard_uc1_long_name_skipped);

    /* cmdMOUNT (uc3 = fw 0.26): konfigurace jednotek. */
    RUN_TEST(test_unicard_mount_writes_cfg);
    RUN_TEST(test_unicard_fdcfg_read_keeps_cfg);
    RUN_TEST(test_unicard_mount_cfg_names_4_7);
    RUN_TEST(test_unicard_mount_missing_image);
    RUN_TEST(test_unicard_mount_eject_uc3_error);
    RUN_TEST(test_unicard_mount_relative_and_cp852);

    /* cmdRTCSETD / cmdRTCSETT (uc3). */
    RUN_TEST(test_unicard_rtc_set_date);
    RUN_TEST(test_unicard_rtc_set_time);
    RUN_TEST(test_unicard_rtc_invalid_ignored);

    /* UC API rev 2: INFO 95h. */
    RUN_TEST(test_unicard_info_absent_in_fw026);
    RUN_TEST(test_unicard_info_rev2);
    RUN_TEST(test_unicard_info_rev2_needs_uc3);
    RUN_TEST(test_unicard_uc3_version);
    RUN_TEST(test_unicard_uc3_boot);
    RUN_TEST(test_unicard_uc3_gdgsync);
    RUN_TEST(test_unicard_uc3_intcaller_usart);
    RUN_TEST(test_unicard_uc1_no_boot);

    /* UC API rev 2: chybové kódy. */
    RUN_TEST(test_unicard_err_codes_rev2);
    RUN_TEST(test_unicard_err_codes_fw026_zero);
    RUN_TEST(test_unicard_mount_eject_rev2_ok);
    RUN_TEST(test_unicard_rtc_invalid_rev2_error);

    /* UC API rev 2: MOUNTS 98h. */
    RUN_TEST(test_unicard_mounts_rev2);
    RUN_TEST(test_unicard_mounts_long_output);
    RUN_TEST(test_unicard_mounts_status_line_rev2);
    RUN_TEST(test_unicard_mounts_absent_in_fw026);

    /* UC API rev 2: COPY 9Ah. */
    RUN_TEST(test_unicard_copy_rev2);
    RUN_TEST(test_unicard_copy_errors_rev2);
    RUN_TEST(test_unicard_copy_background_rev2);
    RUN_TEST(test_unicard_copy_background_busy_rev2);
    RUN_TEST(test_unicard_copy_background_cancel_rev2);
    RUN_TEST(test_unicard_copy_absent_in_fw026);

    /* UC API rev 2: svazek cloud:, LISTVOL 90h. */
    RUN_TEST(test_unicard_cloud_list_rev2);
    RUN_TEST(test_unicard_cloud_open_rev2);
    RUN_TEST(test_unicard_cloud_copy_rev2);
    RUN_TEST(test_unicard_cloud_listvol_rev2);

    /* UC API rev 2: GETCONFIG, SETCONFIG. */
    RUN_TEST(test_unicard_getconfig_rev2);
    RUN_TEST(test_unicard_config_4kb_rev2);
    RUN_TEST(test_unicard_getconfig_status_rev2);
    RUN_TEST(test_unicard_info_ntp_timezone);
    RUN_TEST(test_unicard_setconfig_rev2);
    RUN_TEST(test_unicard_setconfig_errors);

    /* UC API rev 2: RESTART 9Bh. */
    RUN_TEST(test_unicard_restart_rev2);
    RUN_TEST(test_unicard_rkey_rev2);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
