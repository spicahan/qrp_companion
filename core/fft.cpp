// Portable Cooley-Tukey radix-2 DIF FFT
// Replaces esp-dsp's dsps_fft2r_fc32 which has precision issues on ESP32-P4
#include "fft.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

esp_err_t dsps_fft2r_init_fc32(float *, int)
{
    return ESP_OK;
}

// Decimation-in-frequency FFT: natural-order input → bit-reversed output
esp_err_t dsps_fft2r_fc32(float *data, int N)
{
    for (int len = N; len >= 2; len >>= 1) {
        int half = len >> 1;
        float angle = -2.0f * (float)M_PI / len;
        for (int i = 0; i < N; i += len) {
            float wre = 1.0f, wim = 0.0f;
            float cos_a = cosf(angle), sin_a = sinf(angle);
            for (int j = 0; j < half; j++) {
                int a = i + j;
                int b = a + half;
                float tre = data[2 * a]     - data[2 * b];
                float tim = data[2 * a + 1] - data[2 * b + 1];
                data[2 * a]     += data[2 * b];
                data[2 * a + 1] += data[2 * b + 1];
                data[2 * b]     = tre * wre - tim * wim;
                data[2 * b + 1] = tre * wim + tim * wre;
                float new_wre = wre * cos_a - wim * sin_a;
                wim = wre * sin_a + wim * cos_a;
                wre = new_wre;
            }
        }
    }
    return ESP_OK;
}

// Bit-reversal permutation on interleaved complex data
esp_err_t dsps_bit_rev_fc32(float *data, int N)
{
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        while (j & bit) { j ^= bit; bit >>= 1; }
        j ^= bit;
        if (i < j) {
            float tr = data[2 * i]; data[2 * i] = data[2 * j]; data[2 * j] = tr;
            float ti = data[2*i+1]; data[2*i+1] = data[2*j+1]; data[2*j+1] = ti;
        }
    }
    return ESP_OK;
}
