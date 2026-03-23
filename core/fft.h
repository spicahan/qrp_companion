// Portable Cooley-Tukey radix-2 FFT
// API matches esp-dsp's dsps_fft2r for drop-in compatibility
#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif

// Initialize (no-op for this implementation)
esp_err_t dsps_fft2r_init_fc32(float *fft_table_buff, int table_size);

// In-place complex FFT on interleaved data [re0,im0,re1,im1,...]
// N = number of complex points. Output is in bit-reversed order.
esp_err_t dsps_fft2r_fc32(float *data, int N);

// Bit-reversal permutation on interleaved complex data
esp_err_t dsps_bit_rev_fc32(float *data, int N);

#ifdef __cplusplus
}
#endif
