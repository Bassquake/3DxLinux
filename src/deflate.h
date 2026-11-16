/*
 * Tiny raw-DEFLATE (RFC 1951) encoder: one final fixed-Huffman block (BTYPE=01)
 * with hash-chain LZ77. No zlib header/trailer.
 *
 * Matches what 3DxWare's HIDDev.dll sends to WinUSB LCDs: miniz tdefl with flags
 * 0x46080 = FORCE_ALL_STATIC_BLOCKS | GREEDY_PARSING | COMPUTE_ADLER32 | 128 probes
 * and no WRITE_ZLIB_HEADER (0x1000), i.e. raw deflate with static Huffman blocks.
 */
#ifndef TDX_DEFLATE_H
#define TDX_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Worst case output size for @n input bytes (all 9-bit literals + block header). */
#define TDX_DEFLATE_BOUND(n) ((n) + (n) / 8 + 16)

/* Compress @in into @out (capacity @cap). Returns compressed size, or -1 if it won't fit. */
long tdx_deflate_fixed(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

#endif
