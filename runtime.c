/* ------------------------------------------------------------------
 * runtime.c - NES hardware model for the lifted kttgm game code
 *
 * Implements the CPU bus (rd/wr): RAM, PPU registers ($2000-$2007,
 * with the loopy v/t/x/w scroll state and PPUDATA read buffer),
 * OAM DMA ($4014), the controller port ($4016) and APU registers
 * ($4000-$4017, stored - audio output is not modelled).
 *
 * Time advances only inside the hardware hooks the lifted code calls
 * (hook_wait_vblank / hook_wait_frame): each wait_frame is one video
 * frame - vblank begins, the frame is rendered, NMI is delivered,
 * control returns.  This is exactly the order the original 6502 code
 * sees, because it synchronises to the hardware at these spin loops
 * and nowhere else (verified against the ROM's control flow).
 * ------------------------------------------------------------------ */
#include "runtime.h"
#include "apu.h"
#include <stdio.h>

int g_trace = 0;                          /* set from main: RAM/state trace */

/* CPU state (driven by game.c) */
uint8_t A, X, Y, S;
uint8_t fN, fV, fZ, fC, fD, fI;
uint8_t ram[0x800];

/* ------------------------------------------------------------------ PPU */
struct PPU {
    uint8_t ctrl, mask;             /* $2000, $2001 */
    uint8_t vblank;                 /* status bit 7 */
    uint16_t v, t;                  /* loopy scroll/addr registers */
    uint8_t x, w;                   /* fine x, write toggle */
    uint8_t buf;                    /* $2007 read buffer */
    uint8_t oam[256];               /* sprite memory */
    uint8_t oamaddr;
    uint8_t pal[32];                /* palette RAM */
    uint8_t nt[2][1024];            /* nametables (vertical mirroring) */
} ppu;

static uint8_t nt_read(uint16_t a) {   /* a: $2000-$2FFF, vertical mirroring: A,B,A,B */
    uint16_t off = a & 0x03FF;
    int idx = ((a >> 10) & 1);         /* NT select bit 0 only */
    return ppu.nt[idx][off];
}
static void nt_write(uint16_t a, uint8_t v) {
    uint16_t off = a & 0x03FF;
    ppu.nt[((a >> 10) & 1)][off] = v;
}
static uint8_t ppu_bus_read(uint16_t a) {  /* a: 14-bit PPU address */
    a &= 0x3FFF;
    if (a < 0x2000) return CHR_ROM[a];     /* pattern tables (CHR ROM) */
    if (a < 0x3F00) return nt_read(a);
    {   /* palette: mirrored entries */
        uint8_t i = a & 0x1F;
        if ((i & 0x13) == 0x10) i &= 0x0F;
        return ppu.pal[i] & 0x3F;
    }
}
static void ppu_bus_write(uint16_t a, uint8_t v) {
    a &= 0x3FFF;
    if (a < 0x2000) return;               /* CHR ROM: not writable */
    if (a < 0x3F00) { nt_write(a, v); return; }
    {
        uint8_t i = a & 0x1F;
        if ((i & 0x13) == 0x10) i &= 0x0F;
        ppu.pal[i] = v & 0x3F;
    }
}

static uint8_t ppu_reg_read_palette(void);

static uint8_t ppu_reg_read(int r) {
    switch (r) {
    case 2: {                            /* PPUSTATUS */
        uint8_t v = (uint8_t)(ppu.vblank << 7);
        ppu.vblank = 0;
        ppu.w = 0;
        return v;
    }
    case 4:                              /* OAMDATA */
        return ppu.oam[ppu.oamaddr];
    case 7: {                            /* PPUDATA (buffered) */
        uint8_t out;
        if ((ppu.v & 0x3F00) == 0x3F00) {  /* palette reads are direct */
            out = ppu_reg_read_palette();
        } else {
            out = ppu.buf;
            ppu.buf = ppu_bus_read(ppu.v);
        }
        ppu.v = (uint16_t)(ppu.v + ((ppu.ctrl & 0x04) ? 32 : 1));
        return out;
    }
    default:
        return 0;
    }
}
static uint8_t ppu_reg_read_palette(void) {
    uint8_t i = ppu.v & 0x1F;
    if ((i & 0x13) == 0x10) i &= 0x0F;
    return ppu.pal[i] & 0x3F;
}

static void ppu_reg_write(int r, uint8_t v) {
    switch (r) {
    case 0:                              /* PPUCTRL */
        ppu.ctrl = v;
        ppu.t = (uint16_t)((ppu.t & 0xF3FF) | ((v & 3) << 10));
        break;
    case 1:                              /* PPUMASK */
        ppu.mask = v;
        break;
    case 3:                              /* OAMADDR */
        ppu.oamaddr = v;
        break;
    case 4:                              /* OAMDATA */
        ppu.oam[ppu.oamaddr++] = v;
        break;
    case 5:                              /* PPUSCROLL */
        if (!ppu.w) {
            ppu.x = v & 7;
            ppu.t = (uint16_t)((ppu.t & ~0x001F) | ((v & 0xF8) >> 3));
            ppu.w = 1;
        } else {
            ppu.t = (uint16_t)((ppu.t & ~0x73E0) | ((v & 7) << 12) | ((v & 0xF8) << 2));
            ppu.w = 0;
        }
        break;
    case 6:                              /* PPUADDR */
        if (!ppu.w) {
            ppu.t = (uint16_t)((ppu.t & 0x00FF) | ((v & 0x3F) << 8));
            ppu.w = 1;
        } else {
            ppu.t = (uint16_t)((ppu.t & 0x7F00) | v);
            ppu.v = ppu.t;
            ppu.w = 0;
        }
        break;
    case 7:                              /* PPUDATA */
        ppu_bus_write(ppu.v, v);
        ppu.v = (uint16_t)(ppu.v + ((ppu.ctrl & 0x04) ? 32 : 1));
        break;
    }
}

/* ------------------------------------------------------------------ APU / IO */
static uint8_t pad_shift;                 /* controller serial shift */
static uint8_t pad_strobe;

static void io_write(uint16_t a, uint8_t v) {
    if (a == 0x4014) {                    /* OAM DMA: copy a CPU page */
        for (int i = 0; i < 256; i++)
            ppu.oam[(uint8_t)(ppu.oamaddr + i)] = rd((uint16_t)((v << 8) | i));
        return;
    }
    if (a == 0x4016) {                    /* controller strobe */
        pad_strobe = v & 1;
        if (pad_strobe) pad_shift = g_pad_state;
        return;
    }
    apu_write(a, v);                      /* APU registers */
}
static uint8_t io_read(uint16_t a) {
    if (a == 0x4016) {                    /* button serial, order A,B,Select,Start,Up,Down,Left,Right */
        if (pad_strobe) return g_pad_state & 1;
        uint8_t bit = pad_shift & 1;
        pad_shift >>= 1;
        return (uint8_t)(bit | 0x40);
    }
    if (a >= 0x4000 && a <= 0x4017) return apu_read(a);
    return 0;
}

/* ------------------------------------------------------------------ CPU bus */
uint8_t rd(uint16_t a) {
    if (a < 0x2000) return ram[a & 0x7FF];
    if (a < 0x4000) return ppu_reg_read(a & 7);
    if (a < 0x4020) return io_read(a);
    if (a >= 0x8000) return PRG_ROM[a - 0x8000];
    return 0;                             /* open bus / no PRG RAM */
}
void wr(uint16_t a, uint8_t v) {
    if (a < 0x2000) { ram[a & 0x7FF] = v; return; }
    if (a < 0x4000) { ppu_reg_write(a & 7, v); return; }
    if (a < 0x4020) { io_write(a, v); return; }
    /* ROM space: not writable */
}

/* ------------------------------------------------------------------ rendering */
uint8_t g_framebuffer[256 * 240 * 3];

/* NTSC master palette */
static const uint8_t PAL_NES[64][3] = {
    { 84, 84, 84},{  0, 30,116},{  8, 16,144},{ 48,  0,136},{ 68,  0,100},{ 92,  0, 48},{ 84,  4,  0},{ 60, 24,  0},
    { 32, 42,  0},{  8, 58,  0},{  0, 64,  0},{  0, 60,  0},{  0, 50, 60},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
    {152,150,152},{  8, 76,196},{ 48, 50,236},{ 92, 30,228},{136, 20,176},{160, 20,100},{152, 34, 32},{120, 60,  0},
    { 84, 90,  0},{ 40,114,  0},{  8,124,  0},{  0,118, 40},{  0,102,120},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
    {236,238,236},{ 76,154,236},{120,124,236},{176, 98,236},{228, 68,236},{236, 88,180},{236,106,100},{212,136, 32},
    {160,170,  0},{116,196,  0},{ 76,208, 32},{ 56,204,108},{ 56,180,204},{ 60, 60, 60},{  0,  0,  0},{  0,  0,  0},
    {236,238,236},{168,204,236},{188,188,236},{212,178,236},{236,174,236},{236,174,212},{236,180,176},{228,196,144},
    {204,210,120},{180,222,120},{168,226,144},{152,226,180},{160,214,228},{160,162,160},{  0,  0,  0},{  0,  0,  0},
};

static uint8_t palette_color(uint8_t idx) {
    if (ppu.mask & 0x01) idx &= 0x30;    /* greyscale */
    return idx & 0x3F;
}

/* Render one frame from the current PPU state (t = scroll, as at the
 * pre-render reload; OAM + nametables as left by the last NMI). */
static void render_frame(void) {
    uint8_t *fb = g_framebuffer;
    int bg_on  = (ppu.mask >> 3) & 1;
    int spr_on = (ppu.mask >> 4) & 1;
    uint8_t backdrop = palette_color(ppu.pal[0]);

    if (!(bg_on || spr_on)) {            /* rendering disabled: backdrop */
        for (int i = 0; i < 256 * 240; i++) {
            const uint8_t *c = PAL_NES[backdrop];
            fb[i * 3] = c[0]; fb[i * 3 + 1] = c[1]; fb[i * 3 + 2] = c[2];
        }
        return;
    }

    /* scroll position from t (this is what v is reloaded to at pre-render) */
    int totalX = ((ppu.t >> 10) & 1) * 256 + (ppu.t & 0x1F) * 8 + ppu.x;
    int totalY = ((ppu.t >> 11) & 1) * 240 + ((ppu.t >> 5) & 0x1F) * 8 + ((ppu.t >> 12) & 7);
    int bgbase = (ppu.ctrl & 0x10) ? 0x1000 : 0x0000;
    int sprbase = (ppu.ctrl & 0x08) ? 0x1000 : 0x0000;
    int spr16 = (ppu.ctrl & 0x20) ? 1 : 0;
    int bgleft  = (ppu.mask >> 1) & 1;
    int sprleft = (ppu.mask >> 2) & 1;

    /* sprite line buffers: lowest OAM index wins */
    static uint8_t spix[256], spal[256], sidx[256];

    for (int py = 0; py < 240; py++) {
        for (int px = 0; px < 256; px++) { spix[px] = 0; sidx[px] = 64; }

        if (spr_on) {
            for (int i = 0; i < 64; i++) {
                int sy = ppu.oam[i * 4] + 1;          /* OAM y is off-by-one */
                if (py < sy || py >= sy + (spr16 ? 16 : 8)) continue;
                uint8_t tile = ppu.oam[i * 4 + 1];
                uint8_t attr = ppu.oam[i * 4 + 2];
                int sx = ppu.oam[i * 4 + 3];
                int fy = (py - sy) ^ ((attr & 0x80) ? (spr16 ? 15 : 7) : 0);
                int base = spr16 ? ((tile & 1) * 0x1000) : sprbase;
                int tno = spr16 ? (tile & 0xFE) : tile;
                int row = spr16 ? (fy & 8 ? (tno + 1) : tno) : tno;
                uint16_t pa = (uint16_t)(base + row * 16 + (fy & 7));
                uint8_t p0 = CHR_ROM[pa], p1 = CHR_ROM[pa + 8];
                for (int k = 0; k < 8; k++) {
                    int fx = (attr & 0x40) ? k : (7 - k);
                    int sxp = sx + k;
                    if (sxp >= 256) break;            /* no wrap at right edge */
                    if (sxp < 8 && !sprleft) continue;
                    uint8_t v = (uint8_t)(((p0 >> fx) & 1) | (((p1 >> fx) & 1) << 1));
                    if (!v) continue;
                    if (i < sidx[sxp]) { sidx[sxp] = (uint8_t)i; spix[sxp] = v; spal[sxp] = attr; }
                }
            }
        }

        for (int px = 0; px < 256; px++) {
            /* background pixel */
            uint8_t bgv = 0, idx = 0;
            if (bg_on && (px >= 8 || bgleft)) {
                int x = (totalX + px) & 511;
                int y = (totalY + py) % 480;
                int tileX = (x >> 3) & 31, tileY = (y >> 3) % 30;
                int nt = (x >> 8) | ((y / 240) << 1); /* horizontal arrangement */
                uint16_t ntaddr = (uint16_t)(0x2000 | (nt << 10) | (tileY << 5) | tileX);
                uint8_t tile = nt_read(ntaddr);
                uint16_t attraddr = (uint16_t)(0x23C0 | (nt << 10) | ((tileY >> 2) << 3) | (tileX >> 2));
                uint8_t attr = nt_read(attraddr);
                int shift = (uint8_t)(((tileY & 2) << 1) | (tileX & 2));
                int palHi = (attr >> shift) & 3;
                uint16_t pa = (uint16_t)(bgbase + tile * 16 + (y & 7));
                int bit = 7 - (x & 7);
                uint8_t p0 = CHR_ROM[pa], p1 = CHR_ROM[pa + 8];
                bgv = (uint8_t)(((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1));
                if (bgv) idx = ppu.pal[palHi * 4 + bgv];
            }
            if (!bgv) idx = ppu.pal[0];    /* transparent -> backdrop */
            /* sprite over background (priority bit) */
            if (spr_on && spix[px] && (px >= 8 || sprleft)) {
                uint8_t attr = spal[px];
                if (!(attr & 0x20) || !bgv) {
                    idx = ppu.pal[0x10 | ((attr & 3) << 2) | spix[px]];
                }
            }
            const uint8_t *c = PAL_NES[palette_color(idx)];
            uint8_t *d = fb + (py * 256 + px) * 3;
            d[0] = c[0]; d[1] = c[1]; d[2] = c[2];
        }
    }
}

/* ------------------------------------------------------------------ hooks */
void hook_wait_vblank(void) {
    /* vblank flag sets; the BIT $2002 read that follows clears it and
     * leaves N=1 (vblank bit), V=0, Z per A&0x80 - just like the loop */
    ppu.vblank = 1;
    ppu.vblank = 0;                       /* BIT read clears it */
    ppu.w = 0;
    fN = 1; fV = 0; fZ = (uint8_t)((A & 0x80) == 0);
}

void hook_wait_frame(uint16_t a) {
    uint8_t before = rd(a);
    for (;;) {
        /* vblank begins: draw the frame that just finished displaying,
         * then deliver NMI (the game's OAM DMA / scroll / VRAM queue) */
        ppu.vblank = 1;
        render_frame();
        if (ppu.ctrl & 0x80) nes_nmi();
        ppu.vblank = 0;
        apu_frame();                      /* one frame of music from current regs */
        frontend_frame();                 /* present, input, pacing */
        if (g_trace) {
            static int n;
            if ((n++ % 30) == 0)
                fprintf(stderr, "f%-5d state=$%02X intro=$%02X/$%02X $75=$%02X $5C=$%02X $5A=$%02X ctrl=$%02X mask=$%02X ntqueue=$%02X\n",
                        g_frame_count, rd(0x64), rd(0x65), rd(0x66), rd(0x75),
                        rd(0x5C), rd(0x5A), ppu.ctrl, ppu.mask, rd(0x00));
        }
        uint8_t now = rd(a);
        if (now != before) {
            /* flags of the CMP that finally falls through */
            op_cmp(A, now);
            return;
        }
    }
}
