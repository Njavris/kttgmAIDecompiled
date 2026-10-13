#ifndef APU_H
#define APU_H
#include <stdint.h>

#define RING_SAMPLES 8192

void apu_write(uint16_t a, uint8_t v);   /* $4000-$4017 writes */
uint8_t apu_read(uint16_t a);            /* $4000-$4017 reads */
void apu_frame(void);                    /* synthesise one video frame of audio */
void apu_callback(void *stream, int bytes); /* SDL audio callback */
int  apu_take(int16_t *dst, int n);      /* pull synthesised samples (headless) */

#endif
