#!/usr/bin/env python3
"""E2E test: zápis do VRAM v režimu MZ-700 na MZ-800 jde přes registr WF (MCP pipe).

SM800 předepisuje v režimu MZ-700 WF = 01h ("Writes WD into the DATA, ATB,
and CG area"); skutečné MZ-800 zapisuje znaky a atributy na D000h jako do
roviny I podle registru formátu zápisu WF. Program, který nechá WF v režimu
XOR, na stroji nechá na obrazovce starý obraz (Unicard Manager 3.0 smazal
obrazovku a napsal LOADING před nastavením WF a zůstal nepořádek, 2026-11-12).

Kroky (režim MZ-700, DMD 08h, D000h až FFFFh namapované přes E3h):
  1  WF 01h (SINGLE, rovina I): zápis 0Fh přepíše bajt, čtení 0Fh;
  2  WF 21h (XOR, rovina I): zápis 55h, čtení 0Fh xor 55h = 5Ah;
  3  WF 41h (OR, rovina I): zápis 80h, čtení 5Ah or 80h = DAh;
  4  WF 61h (RESET, rovina I): zápis 0Ah, čtení DAh and not 0Ah = D0h;
  5  WF 01h po resetu: zápis zase přímo (jako dřív).

Jen MZ-800. Binárka se hledá v kořeni repa nebo v adresáři z proměnné
``MZ_EMU_DIR``. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import base64
import gettext
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
import test_gdg_cksw as h  # noqa: E402 - PipeEmu a příprava stroje

_ = gettext.gettext

#: Adresa testovacího kódu a výsledku.
_CODE = 0x5200
_RESULT = 0x6000


def _write_read(emu, wf, value):
    """V režimu MZ-700 nastaví WF, zapíše @p value na D000h a vrátí přečtený bajt."""
    code = bytes([
        0x3E, 0x08, 0xD3, 0xCE,             # LD A,08h; OUT (CEh),A: DMD režim MZ-700
        0xD3, 0xE3,                         # OUT (E3h),A: D000h až FFFFh = VRAM a ROM
        0x3E, wf, 0xD3, 0xCC,               # LD A,wf; OUT (CCh),A: WF
        0x3E, value, 0x32, 0x00, 0xD0,      # LD A,value; LD (D000h),A
        0x3A, 0x00, 0xD0,                   # LD A,(D000h)
        0x32, _RESULT & 0xFF, _RESULT >> 8,  # LD (6000h),A
        0x3E, 0x01, 0xD3, 0xCC,             # WF zpět 01h
        0xD3, 0xE1,                         # OUT (E1h),A: D000h až FFFFh = RAM
        0x3E, 0x00, 0xD3, 0xCE,             # DMD 00h
    ])
    emu.call("mem_write", {"addr": _CODE, "data_hex": code.hex() + "18fe"})
    emu.call("set_register", {"reg": "PC", "value": _CODE})
    emu.call("run", {"frames": 2})
    emu.call("set_register", {"reg": "PC", "value": h._LOOP})
    r = emu.call("mem_read", {"addr": _RESULT, "len": 1})
    return base64.b64decode(r.get("data_b64", ""))[0]


def run(exe):
    """Všechny kroky testu na jedné binárce. Vrací True = PASS."""
    print(f"\n=== {exe.name} ===")
    tmp = Path(tempfile.mkdtemp(prefix="mz700_vram_wf_"))
    emu = None
    try:
        emu = h.PipeEmu(exe, tmp)
        h._setup(emu)
        steps = (
            (0x01, 0x0F, 0x0F, "WF SINGLE: the byte written"),
            (0x21, 0x55, 0x5A, "WF XOR plane I: 0Fh xor 55h"),
            (0x41, 0x80, 0xDA, "WF OR plane I: 5Ah or 80h"),
            (0x61, 0x0A, 0xD0, "WF RESET plane I: DAh and not 0Ah"),
            (0x01, 0x33, 0x33, "WF 01h again: written directly"),
        )
        for wf, value, expected, what in steps:
            got = _write_read(emu, wf, value)
            print(f"  WF {wf:02X}h, write {value:02X}h: read {got:02X}h (expected {expected:02X}h)")
            h.check(got == expected, what)
        return True
    except h.TestFailure as e:
        print(f"  FAILED: {e}")
        return False
    finally:
        if emu is not None:
            emu.close()


def main():
    """Vstupní bod: najde binárku MZ-800 a spustí test."""
    emu_test_proc.install(deadline_s=120)
    exe = h._find_exe("mz800emu")
    if exe is None:
        print(_("SKIP: mz800emu binary not found"))
        return 77
    ok = run(exe)
    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
