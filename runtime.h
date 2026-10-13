/* ------------------------------------------------------------------
 * runtime.h - NES hardware stand-ins for the lifted kttgm code
 *
 * The 6502 program itself lives in game.c as native C; this header
 * defines the CPU state it drives, the memory bus it talks to
 * (rd/wr), and the hooks the lifted code calls where the original
 * spun on hardware registers.
 * ------------------------------------------------------------------ */
#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdint.h>
#include <stddef.h>

/* ---- 6502 CPU state (owned by the lifted code) ---- */
extern uint8_t A, X, Y, S;        /* registers */
extern uint8_t fN, fV, fZ, fC, fD, fI;  /* status flags */

/* ---- bus ---- */
extern uint8_t ram[0x800];        /* 2 KiB internal RAM (mirrored to $1FFF) */
uint8_t rd(uint16_t a);           /* read from the CPU bus */
void    wr(uint16_t a, uint8_t v);/* write to the CPU bus */

/* ---- ROM data (defined in game.c) ---- */
extern const uint8_t PRG_ROM[0x8000];
extern const uint8_t CHR_ROM[0x2000];

/* ---- lifted entry points ---- */
void nes_reset(void);
void nes_nmi(void);
void nes_irq(void);

/* ---- hardware hooks called by the lifted code ----
 * hook_wait_vblank: BIT $2002 / BPL spin  -> waits for vblank
 * hook_wait_frame : CMP zp / BEQ spin     -> waits until NMI changed zp
 * ------------------------------------------------------------------ */
void hook_wait_vblank(void);
void hook_wait_frame(uint16_t a);

/* ---- frontend (main.c) ---- */
void frontend_frame(void);        /* present + poll input + pace, once per frame */
extern uint8_t g_framebuffer[256 * 240 * 3];
extern uint8_t g_pad_state;       /* A B Sel Sta U D L R  <- bits 0..7 */
extern int g_headless;            /* 1: no window, dump frames */
extern int g_trace;               /* 1: RAM/state trace to stderr */
extern int g_frame_count;

/* ------------------------------------------------------------------
 * 6502 operation helpers used by the generated code
 * (NES 2A03: decimal mode is disabled in hardware, so ADC/SBC are
 * always binary here - that is faithful, not a shortcut)
 * ------------------------------------------------------------------ */
static inline void op_setNZ(uint8_t v) { fN = (uint8_t)(v >> 7); fZ = (v == 0); }

static inline void op_adc(uint8_t m) {
    uint16_t r = (uint16_t)A + m + fC;
    fV = (uint8_t)(((~(A ^ m) & (A ^ (uint8_t)r)) & 0x80) != 0);
    fC = (uint8_t)(r > 0xFF);
    A = (uint8_t)r;
    op_setNZ(A);
}
static inline void op_sbc(uint8_t m) { op_adc((uint8_t)(m ^ 0xFF)); }

static inline void op_cmp(uint8_t r, uint8_t m) {
    uint8_t d = (uint8_t)(r - m);
    fC = (r >= m);
    op_setNZ(d);
}
static inline void op_bit(uint8_t m) {
    fZ = (uint8_t)((A & m) == 0);
    fN = (uint8_t)(m >> 7);
    fV = (uint8_t)((m >> 6) & 1);
}

/* read-modify-write helpers (address given, memory via rd/wr) */
static inline void op_asl(uint16_t a) { uint8_t t = rd(a); fC = (uint8_t)(t >> 7); t <<= 1; wr(a, t); op_setNZ(t); }
static inline void op_lsr(uint16_t a) { uint8_t t = rd(a); fC = (uint8_t)(t & 1); t >>= 1;    wr(a, t); op_setNZ(t); }
static inline void op_rol(uint16_t a) { uint8_t t = rd(a); uint8_t c = (uint8_t)(t >> 7); t = (uint8_t)((t << 1) | fC); fC = c; wr(a, t); op_setNZ(t); }
static inline void op_ror(uint16_t a) { uint8_t t = rd(a); uint8_t c = (uint8_t)(t & 1);  t = (uint8_t)((t >> 1) | (fC << 7)); fC = c; wr(a, t); op_setNZ(t); }
static inline void op_inc(uint16_t a) { uint8_t t = (uint8_t)(rd(a) + 1); wr(a, t); op_setNZ(t); }
static inline void op_dec(uint16_t a) { uint8_t t = (uint8_t)(rd(a) - 1); wr(a, t); op_setNZ(t); }

/* accumulator variants */
static inline void op_asl_a(void) { fC = (uint8_t)(A >> 7); A <<= 1; op_setNZ(A); }
static inline void op_lsr_a(void) { fC = (uint8_t)(A & 1);  A >>= 1;    op_setNZ(A); }
static inline void op_rol_a(void) { uint8_t c = (uint8_t)(A >> 7); A = (uint8_t)((A << 1) | fC); fC = c; op_setNZ(A); }
static inline void op_ror_a(void) { uint8_t c = (uint8_t)(A & 1);  A = (uint8_t)((A >> 1) | (fC << 7)); fC = c; op_setNZ(A); }

/* stack ($0100-$01FF, S wraps as a byte) */
static inline void op_pha(void) { wr(0x0100 | S, A); S--; }
static inline void op_php(void) { wr(0x0100 | S, (uint8_t)((fN << 7) | (fV << 6) | 0x30 | (fD << 3) | (fI << 2) | (fZ << 1) | fC)); S--; }
static inline void op_pla(void) { S++; A = rd(0x0100 | S); op_setNZ(A); }
static inline void op_plp(void) { S++; uint8_t p = rd(0x0100 | S); fN = (p >> 7) & 1; fV = (p >> 6) & 1; fD = (p >> 3) & 1; fI = (p >> 2) & 1; fZ = (p >> 1) & 1; fC = p & 1; }

/* zero-page indirect addressing helpers */
static inline uint16_t op_ad_izx(uint8_t z) { uint8_t p = (uint8_t)(z + X); return (uint16_t)(rd(p) | (rd((uint8_t)(p + 1)) << 8)); }
static inline uint16_t op_ad_izy(uint8_t z) { uint16_t b = (uint16_t)(rd(z) | (rd((uint8_t)(z + 1)) << 8)); return (uint16_t)(b + Y); }

#endif /* RUNTIME_H */
