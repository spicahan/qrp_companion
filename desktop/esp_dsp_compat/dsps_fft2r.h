// Desktop compatibility shim for esp-dsp FFT API
// Wraps a simple Cooley-Tukey FFT to match esp-dsp function signatures.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize FFT tables (no-op on desktop, table_buff can be NULL)
esp_err_t dsps_fft2r_init_fc32(float *fft_table_buff, int table_size);

// In-place complex FFT on interleaved data [re0,im0,re1,im1,...]
// N = number of complex points (must be power of 2)
// Output is in bit-reversed order (call dsps_bit_rev_fc32 after)
esp_err_t dsps_fft2r_fc32(float *data, int N);

// Bit-reversal permutation on interleaved complex data
esp_err_t dsps_bit_rev_fc32(float *data, int N);

#ifdef __cplusplus
}
#endif
