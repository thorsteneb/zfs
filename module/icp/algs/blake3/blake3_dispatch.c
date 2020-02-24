#include <sys/simd.h>
#include "blake3_impl.h"

/*
#if defined(IS_X86)
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__)
#include <immintrin.h>
#else
#error "Unimplemented!"
#endif
#endif
*/

// Declarations for implementation-specific functions.
void blake3_compress_in_place_portable(uint32_t cv[8],
                                       const uint8_t block[BLAKE3_BLOCK_LEN],
                                       uint8_t block_len, uint64_t counter,
                                       uint8_t flags);

void blake3_compress_xof_portable(const uint32_t cv[8],
                                  const uint8_t block[BLAKE3_BLOCK_LEN],
                                  uint8_t block_len, uint64_t counter,
                                  uint8_t flags, uint8_t out[64]);

void blake3_hash_many_portable(const uint8_t *const *inputs, size_t num_inputs,
                               size_t blocks, const uint32_t key[8],
                               uint64_t counter, boolean_t increment_counter,
                               uint8_t flags, uint8_t flags_start,
                               uint8_t flags_end, uint8_t *out);

#if defined(IS_X86)
#if !defined(BLAKE3_NO_SSE41)
void blake3_compress_in_place_sse41(uint32_t cv[8],
                                    const uint8_t block[BLAKE3_BLOCK_LEN],
                                    uint8_t block_len, uint64_t counter,
                                    uint8_t flags);
void blake3_compress_xof_sse41(const uint32_t cv[8],
                               const uint8_t block[BLAKE3_BLOCK_LEN],
                               uint8_t block_len, uint64_t counter,
                               uint8_t flags, uint8_t out[64]);
void blake3_hash_many_sse41(const uint8_t *const *inputs, size_t num_inputs,
                            size_t blocks, const uint32_t key[8],
                            uint64_t counter, boolean_t increment_counter,
                            uint8_t flags, uint8_t flags_start,
                            uint8_t flags_end, uint8_t *out);
#endif
#if !defined(BLAKE3_NO_AVX2)
void blake3_hash_many_avx2(const uint8_t *const *inputs, size_t num_inputs,
                           size_t blocks, const uint32_t key[8],
                           uint64_t counter, boolean_t increment_counter,
                           uint8_t flags, uint8_t flags_start,
                           uint8_t flags_end, uint8_t *out);
#endif
#if !defined(BLAKE3_NO_AVX512)
void blake3_compress_in_place_avx512(uint32_t cv[8],
                                     const uint8_t block[BLAKE3_BLOCK_LEN],
                                     uint8_t block_len, uint64_t counter,
                                     uint8_t flags);

void blake3_compress_xof_avx512(const uint32_t cv[8],
                                const uint8_t block[BLAKE3_BLOCK_LEN],
                                uint8_t block_len, uint64_t counter,
                                uint8_t flags, uint8_t out[64]);

void blake3_hash_many_avx512(const uint8_t *const *inputs, size_t num_inputs,
                             size_t blocks, const uint32_t key[8],
                             uint64_t counter, boolean_t increment_counter,
                             uint8_t flags, uint8_t flags_start,
                             uint8_t flags_end, uint8_t *out);
#endif
#endif

#if defined(BLAKE3_USE_NEON)
void blake3_hash_many_neon(const uint8_t *const *inputs, size_t num_inputs,
                           size_t blocks, const uint32_t key[8],
                           uint64_t counter, boolean_t increment_counter,
                           uint8_t flags, uint8_t flags_start,
                           uint8_t flags_end, uint8_t *out);
#endif

enum cpu_feature {
  CF_SSE2 = 1 << 0,
  CF_SSSE3 = 1 << 1,
  CF_SSE41 = 1 << 2,
  CF_AVX = 1 << 3,
  CF_AVX2 = 1 << 4,
  CF_AVX512F = 1 << 5,
  CF_AVX512VL = 1 << 6,
  /* ... */
  CF_UNDEFINED = 1 << 30
};

#if !defined(BLAKE3_TESTING)
static /* Allow the variable to be controlled manually for testing */
#endif
    enum cpu_feature g_cpu_features = CF_UNDEFINED;

#if !defined(BLAKE3_TESTING)
static
#endif
    enum cpu_feature
    get_cpu_features(void) {

  if (g_cpu_features != CF_UNDEFINED) {
    return g_cpu_features;
  } else {
#if defined(IS_X86)
  enum cpu_feature features = 0;
#if defined(__amd64__) || defined(_M_X64)
    features |= CF_SSE2;
#else
    if (zfs_sse2_available())
      features |= CF_SSE2;
#endif
    if (zfs_sse3_available())
      features |= CF_SSSE3;
    if (zfs_sse4_1_available())
      features |= CF_SSE41;
    if (zfs_avx_available())
      features |= CF_AVX;
    if (zfs_avx2_available())
      features |= CF_AVX2;
    if (zfs_avx512f_available())
      features |= CF_AVX512F;
    if (zfs_avx512vl_available())
      features |= CF_AVX512VL;
    g_cpu_features = features;
    return features;
#else
    /* How to detect NEON? */
    return 0;
#endif
  }
}

void blake3_compress_in_place(uint32_t cv[8],
                              const uint8_t block[BLAKE3_BLOCK_LEN],
                              uint8_t block_len, uint64_t counter,
                              uint8_t flags) {
#if defined(IS_X86)
  const enum cpu_feature features = get_cpu_features();
#if !defined(BLAKE3_NO_AVX512)
  if (features & CF_AVX512VL) {
    kfpu_begin();
    blake3_compress_in_place_avx512(cv, block, block_len, counter, flags);
    kfpu_end();
    return;
  }
#endif
#if !defined(BLAKE3_NO_SSE41)
  if (features & CF_SSE41) {
    kfpu_begin();
    blake3_compress_in_place_sse41(cv, block, block_len, counter, flags);
    kfpu_end();
    return;
  }
#endif
#endif
  blake3_compress_in_place_portable(cv, block, block_len, counter, flags);
}

void blake3_compress_xof(const uint32_t cv[8],
                         const uint8_t block[BLAKE3_BLOCK_LEN],
                         uint8_t block_len, uint64_t counter, uint8_t flags,
                         uint8_t out[64]) {
#if defined(IS_X86)
  const enum cpu_feature features = get_cpu_features();
#if !defined(BLAKE3_NO_AVX512)
  if (features & CF_AVX512VL) {
    kfpu_begin();
    blake3_compress_xof_avx512(cv, block, block_len, counter, flags, out);
    kfpu_end();
    return;
  }
#endif
#if !defined(BLAKE3_NO_SSE41)
  if (features & CF_SSE41) {
    kfpu_begin();
    blake3_compress_xof_sse41(cv, block, block_len, counter, flags, out);
    kfpu_end();
    return;
  }
#endif
#endif
  blake3_compress_xof_portable(cv, block, block_len, counter, flags, out);
}

void blake3_hash_many(const uint8_t *const *inputs, size_t num_inputs,
                      size_t blocks, const uint32_t key[8], uint64_t counter,
                      boolean_t increment_counter, uint8_t flags,
                      uint8_t flags_start, uint8_t flags_end, uint8_t *out) {
#if defined(IS_X86)
  const enum cpu_feature features = get_cpu_features();
#if !defined(BLAKE3_NO_AVX512)
  if (features & CF_AVX512F) {
    kfpu_begin();
    blake3_hash_many_avx512(inputs, num_inputs, blocks, key, counter,
                            increment_counter, flags, flags_start, flags_end,
                            out);
    kfpu_end();
    return;
  }
#endif
#if !defined(BLAKE3_NO_AVX2)
  if (features & CF_AVX2) {
    kfpu_begin();
    blake3_hash_many_avx2(inputs, num_inputs, blocks, key, counter,
                          increment_counter, flags, flags_start, flags_end,
                          out);
    kfpu_end();
    return;
  }
#endif
#if !defined(BLAKE3_NO_SSE41)
  if (features & CF_SSE41) {
    kfpu_begin();
    blake3_hash_many_sse41(inputs, num_inputs, blocks, key, counter,
                           increment_counter, flags, flags_start, flags_end,
                           out);
    kfpu_end();
    return;
  }
#endif
#endif

#if defined(BLAKE3_USE_NEON)
  blake3_hash_many_neon(inputs, num_inputs, blocks, key, counter,
                        increment_counter, flags, flags_start, flags_end, out);
  return;
#endif

  blake3_hash_many_portable(inputs, num_inputs, blocks, key, counter,
                            increment_counter, flags, flags_start, flags_end,
                            out);
}

// The dynamically detected SIMD degree of the current platform.
size_t blake3_simd_degree(void) {
#if defined(IS_X86)
  const enum cpu_feature features = get_cpu_features();
#if !defined(BLAKE3_NO_AVX512)
  if (features & CF_AVX512F) {
    return 16;
  }
#endif
#if !defined(BLAKE3_NO_AVX2)
  if (features & CF_AVX2) {
    return 8;
  }
#endif
#if !defined(BLAKE3_NO_SSE41)
  if (features & CF_SSE41) {
    return 4;
  }
#endif
#endif
#if defined(BLAKE3_USE_NEON)
  return 4;
#endif
  return 1;
}
