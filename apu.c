/* ------------------------------------------------------------------
 * apu.c - 2A03 APU model (2 pulse + noise + DMC, no triangle use in
 * this game) synthesised at frame granularity: one video frame of
 * audio is generated each time the game synchronises to vblank,
 * from the register state the music engine left in $4000-$4017.
 *
 * The frame counter IRQ is inhibited by the game ($4017 = $40), so
 * no interrupt emulation is needed.
 * ------------------------------------------------------------------ */
#include "runtime.h"
#include "apu.h"
#include <string.h>

#define FRAME_CYCLES 29780.5          /* 1789773 / 60.0988 fps */
#define SAMPLE_RATE  44100

/* ------------------------------------------------------------------ state */
static const uint8_t DUTY[4][8] = {
    {0,1,0,0,0,0,0,0}, {0,1,1,0,0,0,0,0},
    {0,1,1,1,1,0,0,0}, {1,0,0,1,1,1,1,1},
};
static const uint16_t LEN_TAB[32] = {
    10,254,20,2,40,4,80,6,160,8,60,10,14,12,26,14,
    12,16,24,18,48,20,96,22,192,24,72,26,16,28,32,30,
};
static const uint16_t NOISE_TAB[16] = {
    4,8,16,32,64,96,128,160,202,254,380,508,762,1016,2034,4068,
};
static const uint16_t DMC_RATE[16] = {
    428,380,340,320,286,254,226,214,190,160,142,128,106,84,56,2,
};

typedef struct {
    uint8_t duty, halt, cvol, vol;      /* $4000/$4004 */
    uint16_t period, timer;             /* 12-bit (timer in APU half-cycles) */
    uint8_t seq;                        /* duty sequencer position */
    uint8_t len;                        /* length counter */
    /* envelope */
    uint8_t env_start, env_decay, env_div;
    /* sweep */
    uint8_t sw_en, sw_neg, sw_shift, sw_div, sw_reload;
} Pulse;

typedef struct {
    uint8_t halt, cvol, vol;            /* $400C */
    uint8_t mode;                       /* $400E */
    uint16_t period, timer;             /* APU cycles */
    uint16_t lfsr;
    uint8_t len;
    uint8_t env_start, env_decay, env_div;
} Noise;

typedef struct {
    uint8_t irq, loop, rate;            /* $4010 */
    uint8_t level;                      /* $4011 direct load / DAC out */
    uint16_t addr, len;                 /* $4012/$4013 config */
    uint16_t cur_addr;
    int     bytes_left;
    uint8_t shift, bits, silence;
    uint16_t timer;
    uint8_t enabled;
} DMC;

static Pulse p[2];
static Noise nz = { .period = 4, .lfsr = 1 };  /* power-up safe defaults */
static DMC dmc;
static uint8_t reg[0x18];               /* register image (reads) */
static uint8_t enable;                  /* $4015 */
static double qtimer, htimer;           /* quarter/half frame tick timers */
static int dmc_irq_flag;

/* output ring buffer (mono s16) */
static int16_t ring[RING_SAMPLES];
static volatile unsigned ring_w, ring_r;

/* ------------------------------------------------------------------ register interface */
void apu_write(uint16_t a, uint8_t v) {
    int i = (a >= 0x4010) ? 14 : (a & 7) + ((a & 8) ? 4 : 0); /* 0..15 */
    reg[a - 0x4000] = v;
    if (a >= 0x4014 && a <= 0x4017) {
        if (a == 0x4015) {
            enable = v & 0x1F;
            if (!(v & 1)) p[0].len = 0;
            if (!(v & 2)) p[1].len = 0;
            if (!(v & 8)) nz.len = 0;
            if (v & 0x10) {
                if (dmc.bytes_left == 0) {   /* (re)start the sample */
                    dmc.bytes_left = dmc.len ? (dmc.len * 16 + 1) : 0;
                    dmc.cur_addr = dmc.addr;
                    dmc.silence = 1;
                    dmc.bits = 8;
                    dmc.shift = 0;
                }
                dmc.enabled = 1;
            } else {
                dmc.bytes_left = 0;
                dmc.enabled = 0;
            }
        }
        return;
    }
    switch (a) {
    case 0x4000: case 0x4004: {
        Pulse *q = &p[(a & 4) != 0];
        q->duty = v >> 6; q->halt = (v >> 5) & 1;
        q->cvol = (v >> 4) & 1; q->vol = v & 0x0F;
        break;
    }
    case 0x4001: case 0x4005: {
        Pulse *q = &p[(a & 4) != 0];
        q->sw_en = v >> 7; q->sw_div = (v >> 4) & 7;
        q->sw_neg = (v >> 3) & 1; q->sw_shift = v & 7;
        q->sw_reload = 1;
        break;
    }
    case 0x4002: case 0x4006:
        p[(a & 4) != 0].period = (p[(a & 4) != 0].period & 0x700) | v;
        break;
    case 0x4003: case 0x4007: {
        Pulse *q = &p[(a & 4) != 0];
        q->period = (q->period & 0xFF) | ((v & 7) << 8);
        if (enable & (((a & 4) ? 2 : 1))) q->len = LEN_TAB[(v >> 3) & 0x1F];
        q->seq = 0;
        q->env_start = 1;
        break;
    }
    case 0x400C:
        nz.halt = (v >> 5) & 1; nz.cvol = (v >> 4) & 1; nz.vol = v & 0x0F;
        break;
    case 0x400E:
        nz.mode = v & 0x80;
        nz.period = NOISE_TAB[v & 0x0F];
        break;
    case 0x400F:
        if (enable & 8) nz.len = LEN_TAB[(v >> 3) & 0x1F];
        nz.env_start = 1;
        break;
    case 0x4010:
        dmc.irq = v >> 7; dmc.loop = (v >> 6) & 1; dmc.rate = v & 0x0F;
        break;
    case 0x4011:
        dmc.level = v & 0x7F;
        break;
    case 0x4012:
        dmc.addr = (uint16_t)(0xC000 + (v << 6));
        break;
    case 0x4013:
        dmc.len = (uint16_t)(v << 4);
        break;
    }
    (void)i;
}

uint8_t apu_read(uint16_t a) {
    if (a == 0x4015) {
        uint8_t s = 0;
        if (p[0].len) s |= 1;
        if (p[1].len) s |= 2;
        if (nz.len)  s |= 8;
        if (dmc.bytes_left > 0) s |= 0x10;
        if (dmc_irq_flag) s |= 0x80;
        return s;
    }
    return reg[a - 0x4000];             /* register image (music engine
                                           reads back $4000/$4004 to keep duty) */
}

/* ------------------------------------------------------------------ synthesis */
static void env_tick(Pulse *q) {
    if (q->env_start) { q->env_start = 0; q->env_decay = 15; q->env_div = q->vol; }
    else if (q->env_div == 0) {
        q->env_div = q->vol;
        if (q->env_decay) q->env_decay--;
        else if (q->halt) q->env_decay = 15;
    } else q->env_div--;
}
static void nz_env_tick(void) {
    if (nz.env_start) { nz.env_start = 0; nz.env_decay = 15; nz.env_div = nz.vol; }
    else if (nz.env_div == 0) {
        nz.env_div = nz.vol;
        if (nz.env_decay) nz.env_decay--;
        else if (nz.halt) nz.env_decay = 15;
    } else nz.env_div--;
}

static inline uint16_t pulse_target(Pulse *q, int ch) {
    uint16_t delta = q->period >> q->sw_shift;
    return (uint16_t)(q->period + (q->sw_neg ? (ch ? -delta : ~delta) : delta));
}

static void apu_step(int cpu_cycles) {  /* advance one CPU cycle */
    for (int c = 0; c < cpu_cycles; c++) {
        for (int i = 0; i < 2; i++) {
            Pulse *q = &p[i];
            q->timer -= 2;              /* timer counts APU half-cycles */
            while ((int16_t)q->timer < 0) {
                q->timer = (uint16_t)(q->timer + (q->period + 1) * 4);
                q->seq = (q->seq + 1) & 7;
            }
        }
        if (nz.period == 0) nz.period = 4;
        nz.timer--;
        while ((int16_t)nz.timer < 0) {
            nz.timer = (uint16_t)(nz.timer + nz.period);
            int fb = (nz.lfsr & 1) ^ ((nz.lfsr >> (nz.mode ? 6 : 1)) & 1);
            nz.lfsr = (uint16_t)((nz.lfsr >> 1) | (fb << 14));
        }
        if (dmc.enabled) {
            if (dmc.timer == 0) {
                dmc.timer = DMC_RATE[dmc.rate];
                if (!dmc.silence) {
                    int bit = dmc.shift & 1;
                    if (bit) { if (dmc.level > 1) dmc.level -= 2; }
                    else     { if (dmc.level < 126) dmc.level += 2; }
                }
                dmc.shift >>= 1;
                if (--dmc.bits == 0) {
                    dmc.bits = 8;
                    if (dmc.bytes_left > 0) {
                        dmc.shift = rd(dmc.cur_addr);
                        dmc.silence = 0;
                        dmc.cur_addr = (uint16_t)((dmc.cur_addr + 1) & 0x7FFF);
                        if (dmc.cur_addr == 0) dmc.cur_addr = 0x8000;
                        dmc.bytes_left--;
                        if (dmc.bytes_left == 0 && dmc.loop) {
                            dmc.bytes_left = dmc.len * 16 + 1;
                            dmc.cur_addr = dmc.addr;
                        }
                    } else {
                        dmc.silence = 1;
                    }
                }
            } else dmc.timer--;
        }
        /* frame ticks */
        qtimer += 1;
        if (qtimer >= 3729) {
            qtimer -= 3729;
            env_tick(&p[0]); env_tick(&p[1]); nz_env_tick();
            htimer += 3729;
            if (htimer >= 14915) {   /* ~120 Hz: length + sweep */
                htimer -= 14915;
                for (int i = 0; i < 2; i++) {
                    Pulse *q = &p[i];
                    if (!q->halt && q->len) q->len--;
                    if (q->sw_en && q->sw_shift) {
                        uint16_t t = pulse_target(q, i);
                        if (t < 8 || t > 0x7FF) { /* muted, no update */ }
                        else q->period = t;
                    }
                }
                if (!nz.halt && nz.len) nz.len--;
            }
        }
    }
}

static inline int16_t pulse_out(Pulse *q) {
    if (!q->len) return 0;
    if (q->period < 8) return 0;
    uint16_t t = pulse_target(q, 0);
    if (q->sw_en && q->sw_shift && (t > 0x7FF || q->period < 8)) return 0;
    uint8_t vol = q->cvol ? q->vol : q->env_decay;
    return (int16_t)(DUTY[q->duty][q->seq & 7] * vol);
}
static inline int16_t noise_out(void) {
    if (!nz.len) return 0;
    uint8_t vol = nz.cvol ? nz.vol : nz.env_decay;
    return (int16_t)((~nz.lfsr & 1) * vol);
}

/* generate one video frame of audio into the ring buffer */
void apu_frame(void) {
    static double frac;
    double samples = FRAME_CYCLES * SAMPLE_RATE / 1789773.0;
    int n = (int)(samples + frac);
    frac += samples - n;
    for (int s = 0; s < n; s++) {
        apu_step((s == 0) ? 40 : 41);   /* ~40.58 CPU cycles per sample */
        int32_t mix = pulse_out(&p[0]) * 700 + pulse_out(&p[1]) * 700
                    + noise_out() * 500 + ((int)dmc.level - 64) * 150;
        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        unsigned w = ring_w;
        if ((unsigned)((w + 1) % RING_SAMPLES) != ring_r) {
            ring[w] = (int16_t)mix;
            ring_w = (w + 1) % RING_SAMPLES;
        }
    }
}

void apu_callback(void *stream, int bytes) {
    int16_t *out = (int16_t *)stream;
    int n = bytes / 2;
    for (int i = 0; i < n; i++) {
        if (ring_r != ring_w) {
            out[i] = ring[ring_r];
            ring_r = (ring_r + 1) % RING_SAMPLES;
        } else {
            out[i] = 0;                 /* underrun: silence */
        }
    }
}

int apu_take(int16_t *dst, int n) {
    int i;
    for (i = 0; i < n && ring_r != ring_w; i++) {
        dst[i] = ring[ring_r];
        ring_r = (ring_r + 1) % RING_SAMPLES;
    }
    return i;
}
