/*
 * File:   mz800_vramctrl.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 18. června 2015, 19:12
 *
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MZ800_VRAMCTRL_H
#define MZ800_VRAMCTRL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>
#include "debugger/debugger.h"

    typedef enum
    {
        GDG_WF_MODE_SINGLE = 0,
        GDG_WF_MODE_EXOR,
        GDG_WF_MODE_OR,
        GDG_WF_MODE_RESET,
        GDG_WF_MODE_REPLACE,
        GDG_WF_MODE_PSET = 6
    } WFR_MODE;

    typedef struct st_VRAMCTRL
    {
        unsigned regWF_PLANE;
        WFR_MODE regWF_MODE;

        unsigned regRF_PLANE;
        unsigned regRF_SEARCH;
        unsigned regWFRF_VBANK;
        unsigned mz700_wr_latch_is_used;
    } st_VRAMCTRL;

    extern st_VRAMCTRL g_vramctrl;

    extern void vramctrl_reset(void);

    extern void vramctrl_mz800_set_wf_rf_reg(int addr, uint8_t value);

    extern uint8_t vramctrl_mz800_memop_read_byte_sync(uint16_t addr);
    extern uint8_t vramctrl_mz800_memop_read_byte(uint16_t addr);
    extern void vramctrl_mz800_memop_write_byte(uint16_t addr, uint8_t value);
    extern void vramctrl_mz800_memop_write_byte_sync(uint16_t addr, uint8_t value);

#include "mz800_gdg.h"
#include "mz800_framebuffer.h"
#include "memory/memory.h"
    /*******************************************************************************
     *
     * VRAM kontroler pro rezimy MZ-700
     *
     ******************************************************************************/

    /**
     * Cteni z MZ-700 VRAM bez ohledu na synchronizaci.
     *
     * @param addr
     * @return
     */
#define vramctrl_mz700_memop_read_byte(addr) g_memory.VRAM[addr]

    /**
     * Interni subrutina fyzickeho zapisu do MZ-700 VRAM - generuje vsak zmenu ve screenu.
     *
     * @param addr
     * @param value
     */
    /**
     * Hodnota zapisu do MZ-700 VRAM podle registru WF: GDG ji zpracuje jako rovinu I.
     *
     * SM800 (tabulka WF): v rezimu MZ-700 ma byt WF = 01h (SINGLE, rovina I) - "Writes WD into the DATA, ATB,
     * and CG area". Skutecne MZ-800 s WF ponechanym v rezimu XOR (posledni kresleni programu v rezimu MZ-800)
     * nove znaky do stareho obsahu prixoruje a smazani nulou nic nezmeni (foto autora UC-UniMgr, 2026-11-12).
     * SINGLE, XOR, OR, RESET zapisuji jen s vybranou rovinou I; REPLACE a PSET s nevybranou rovinou I zapisuji
     * 0 (REPLACE), resp. VRAM AND NOT data (PSET), jako nevybrane roviny v rezimu MZ-800.
     *
     * @param old   Puvodni bajt VRAM.
     * @param value Bajt od CPU.
     * @param out   Vystup: bajt k zapisu.
     * @return 1 = zapsat, 0 = beze zmeny.
     */
    static inline int vramctrl_mz700_wf_value(uint8_t old, uint8_t value, uint8_t *out)
    {
        int p1 = (g_vramctrl.regWF_PLANE & 0x01) ? 1 : 0;
        switch (g_vramctrl.regWF_MODE) {
            case GDG_WF_MODE_SINGLE:
                *out = value;
                return p1;
            case GDG_WF_MODE_EXOR:
                *out = (uint8_t) (old ^ value);
                return p1;
            case GDG_WF_MODE_OR:
                *out = (uint8_t) (old | value);
                return p1;
            case GDG_WF_MODE_RESET:
                *out = (uint8_t) (old & ~value);
                return p1;
            case GDG_WF_MODE_REPLACE:
                *out = p1 ? value : 0x00;
                return 1;
            default: /* PSET */
                *out = p1 ? (uint8_t) (old | value) : (uint8_t) (old & ~value);
                return 1;
        }
    }

#define vramctrl_mz700_memop_write_byte_internal(addr, value)                                                     \
    {                                                                                                             \
        /* TODO: meli by jsme nastavovat jen pokud se zapisuje do CG-RAM, nebo do VRAM, ktera je opravdu videt */ \
        uint8_t wf_out_;                                                                                          \
        if (vramctrl_mz700_wf_value(g_memory.VRAM[addr], (value), &wf_out_)) {                                    \
            g_framebuffer.screen_changes = SCRSTS_THIS_IS_CHANGED;                                                \
            g_memory.VRAM[addr] = wf_out_;                                                                        \
        }                                                                                                         \
        VRAMCTRL_DBG_NOTIFY_VRAM_TOUCH();                                                                         \
    }

    /**
     * Zapis do MZ-700 VRAM bez synchronizace - generuje vsak zmenu ve screenu.
     *
     * @param addr
     * @param value
     */
#define vramctrl_mz700_memop_write_byte(addr, value) vramctrl_mz700_memop_write_byte_internal(addr, value)

    /**
     * One-shot WAIT logika pro MZ-700 mode VRAM/CG-RAM pristup.
     *
     * Pravidla z gate-level HDL simulace GDG (viz mz800-knowledge
     * public/reference/agent/hw/08-video-mz700-mode.md):
     *
     *   1. SIGNAL_GDG_HBLNK = 1 (visible canvas, vcetne 5 px pre-canvas zone):
     *      - Prvni R/W pristup na scanline projde bez WAIT (= "first free").
     *      - Kazdy dalsi R/W pristup na stejnem scanline dostane WAIT az do
     *        konce visible (~3 px pred koncem canvas, kdy fyrne HBLN_START
     *        event a deaktivuje SIGNAL_GDG_HBLNK).
     *      - Latch je sdileny pro READ i WRITE.
     *   2. SIGNAL_GDG_HBLNK = 0 (border / blanking): zadny WAIT bez ohledu
     *      na latch (= raster-chasing free okno).
     *   3. Latch reset na visible -> blank prechodu (= MZEVENT_GDG_HBLN_START
     *      event handler resetuje g_vramctrl.mz700_wr_latch_is_used na 0).
     *
     * Predpoklad: pred volanim doslo k mzarch_main_insideop_mreq() (sync
     * total_elapsed.ticks tak, ze SIGNAL_GDG_HBLNK odpovida current beam pos).
     */
    static inline void vramctrl_mz700_memop_oneshot_wait(void)
    {
        if (!SIGNAL_GDG_HBLNK) {
            return;     /* mimo visible - WAIT nikdy */
        }
        if (g_vramctrl.mz700_wr_latch_is_used) {
            /* dalsi pristup ve stejne visible periode - WAIT do konce visible */
            mzarch_main_insideop_mreq_mz700_vramctrl();
            return;
        }
        /* prvni pristup v aktualni visible periode - oznac latch, NO WAIT */
        g_vramctrl.mz700_wr_latch_is_used = 1;
    }

    /**
     * Cteni z MZ-700 VRAM se synchronizaci a HDL-spravnym WAIT.
     *
     * @param addr
     * @return
     */
    static inline uint8_t vramctrl_mz700_memop_read_byte_sync(uint16_t addr)
    {
        mzarch_main_insideop_mreq();
        vramctrl_mz700_memop_oneshot_wait();
        return vramctrl_mz700_memop_read_byte(addr);
    }

    /**
     * Zapis do MZ-700 VRAM se synchronizaci a HDL-spravnym WAIT.
     *
     * @param addr
     * @param value
     */
#define vramctrl_mz700_memop_write_byte_sync(addr, value)           \
    {                                                               \
        mzarch_main_insideop_mreq();                                \
        vramctrl_mz700_memop_oneshot_wait();                        \
        vramctrl_mz700_memop_write_byte_internal(addr, value);      \
    }

#ifdef __cplusplus
}
#endif

#endif /* MZ800_VRAMCTRL_H */
