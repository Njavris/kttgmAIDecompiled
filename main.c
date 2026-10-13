/* ------------------------------------------------------------------
 * main.c - SDL2 frontend for the lifted kttgm game (Linux, macOS)
 *
 * SDL2 window (256x240, scaled 2x), keyboard as the NES controller:
 *   arrows  - d-pad       Z - button A      X - button B
 *   Enter   - Start       Tab - Select      Esc - quit
 *
 * Headless mode for testing/CI:  ./kttgm --headless N [prefix]
 * runs N frames without a window, dumps frame PPMs, optional WAV.
 * Build with -DKTTGM_NO_SDL for an SDL-free headless binary.
 * ------------------------------------------------------------------ */
#include "runtime.h"
#include "apu.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef KTTGM_NO_SDL
#include <SDL.h>
#endif

uint8_t g_pad_state;
int g_headless = 0;
int g_frame_count = 0;

static int frames_to_run = 0;
static int dump_every = 1;
static const char *dump_prefix = "frame";

/* scripted input: "frame:mask,frame:mask,..." (mask: A,B,Sel,Sta,U,D,L,R bits) */
static struct { int frame; uint8_t mask; } script[256];
static int nscript, script_i;

static void parse_script(const char *s) {
    while (s && *s && nscript < 256) {
        char *end;
        long f = strtol(s, &end, 10);
        if (*end == ':') {
            long m = strtol(end + 1, &end, 0);
            script[nscript].frame = (int)f;
            script[nscript].mask = (uint8_t)m;
            nscript++;
        }
        s = (*end == ',') ? end + 1 : NULL;
    }
}

static void apply_script(void) {
    if (!nscript) return;
    while (script_i + 1 < nscript && script[script_i + 1].frame <= g_frame_count)
        script_i++;
    g_pad_state = (g_frame_count >= script[script_i].frame) ? script[script_i].mask : 0;
}

/* ---- headless WAV capture (KTTGM_WAV=path) ---- */
static FILE *wavf;
static long wav_samples;
static void wav_finish(void) {
    if (!wavf) return;
    fflush(wavf);
    long data = wav_samples * 2;
    fseek(wavf, 0, SEEK_SET);
    unsigned char hdr[44] = {
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0, 1,0, 1,0, 0x44,0xAC,0,0, 0x88,0x58,0x01,0, 2,0, 16,0,
        'd','a','t','a',0,0,0,0 };
    hdr[4] = (data + 36)      ; hdr[5] = (data + 36) >> 8;
    hdr[6] = (data + 36) >> 16; hdr[7] = (data + 36) >> 24;
    hdr[40] = data; hdr[41] = data >> 8; hdr[42] = data >> 16; hdr[43] = data >> 24;
    fwrite(hdr, 1, 44, wavf);
    fclose(wavf);
    fprintf(stderr, "kttgm: wrote %ld audio samples to WAV\n", wav_samples);
}
static void wav_begin(const char *path) {
    wavf = fopen(path, "wb");
    if (!wavf) { fprintf(stderr, "kttgm: cannot open %s\n", path); return; }
    unsigned char hdr[44] = {0};
    fwrite(hdr, 1, 44, wavf);              /* sizes patched at exit */
    atexit(wav_finish);
}

static void dump_ppm(int n) {
    char path[512];
    snprintf(path, sizeof path, "%s%04d.ppm", dump_prefix, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n256 240\n255\n");
    fwrite(g_framebuffer, 1, sizeof g_framebuffer, f);
    fclose(f);
}

#ifndef KTTGM_NO_SDL
/* ---- window / audio frontend ---- */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID adev;

static void die(const char *m) { fprintf(stderr, "kttgm: %s: %s\n", m, SDL_GetError()); exit(1); }

static void sdl_audio_cb(void *userdata, uint8_t *stream, int len) {
    (void)userdata;
    apu_callback(stream, len);
}

static void frontend_init_audio(void) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = sdl_audio_cb;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) {
        fprintf(stderr, "kttgm: no audio device (%s) - continuing silently\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(adev, 0);
}

static void frontend_init_window(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) die("SDL_Init");
    atexit(SDL_Quit);
    win = SDL_CreateWindow("Kur tu teci, Gailiti manu? (kttgm.nes -> C port)",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           512, 480,
                           SDL_WINDOW_RESIZABLE);
    if (!win) die("SDL_CreateWindow");
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) die("SDL_CreateRenderer");
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 256, 240);
    if (!tex) die("SDL_CreateTexture");
}

/* map keyboard state to controller bits (A,B,Select,Start,Up,Down,Left,Right) */
static void update_pad(const uint8_t *ks) {
    uint8_t p = 0;
    if (ks[SDL_SCANCODE_Z])      p |= 0x01;  /* A */
    if (ks[SDL_SCANCODE_X])      p |= 0x02;  /* B */
    if (ks[SDL_SCANCODE_TAB])    p |= 0x04;  /* Select */
    if (ks[SDL_SCANCODE_RETURN]) p |= 0x08;  /* Start */
    if (ks[SDL_SCANCODE_UP])     p |= 0x10;
    if (ks[SDL_SCANCODE_DOWN])   p |= 0x20;
    if (ks[SDL_SCANCODE_LEFT])   p |= 0x40;
    if (ks[SDL_SCANCODE_RIGHT])  p |= 0x80;
    g_pad_state = p;
}
#endif /* !KTTGM_NO_SDL */

void frontend_frame(void) {
    g_frame_count++;
    if (g_headless) {
        apply_script();
        if (wavf) {
            int16_t buf[1024];
            int n;
            while ((n = apu_take(buf, 1024)) > 0) {
                fwrite(buf, 2, n, wavf);
                wav_samples += n;
            }
        }
        if (g_frame_count % dump_every == 0) dump_ppm(g_frame_count);
        if (frames_to_run && g_frame_count >= frames_to_run) {
            printf("ran %d frames\n", g_frame_count);
            exit(0);
        }
        return;
    }
#ifndef KTTGM_NO_SDL
    SDL_UpdateTexture(tex, NULL, g_framebuffer, 256 * 3);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
        if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) exit(0);
    }
    update_pad(SDL_GetKeyboardState(NULL));
#endif
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) {
            g_headless = 1;
            frames_to_run = (i + 1 < argc) ? atoi(argv[++i]) : 0;
        } else if (!strcmp(argv[i], "--prefix") && i + 1 < argc) {
            dump_prefix = argv[++i];
        } else if (!strcmp(argv[i], "--every") && i + 1 < argc) {
            dump_every = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            parse_script(argv[++i]);
        }
    }

    /* power-on state; the reset routine sets S, I, D itself.
     * PPU/APU/ram live in runtime.c and start zeroed. */
    A = X = Y = 0;
    S = 0xFD;
    fN = fV = fZ = fC = fD = fI = 0;
    memset(ram, 0, sizeof ram);

    if (getenv("KTTGM_TRACE")) g_trace = 1;

    if (g_headless) {
        if (getenv("KTTGM_WAV")) wav_begin(getenv("KTTGM_WAV"));
    } else {
#ifndef KTTGM_NO_SDL
        frontend_init_window();
        frontend_init_audio();
#else
        fprintf(stderr, "kttgm: this binary was built with KTTGM_NO_SDL; "
                        "only --headless is available\n");
        return 1;
#endif
    }

    /* run the lifted reset routine: it never returns - the game runs
     * inside it, synchronising to the video frame in the hooks */
    nes_reset();
    return 0;
}
