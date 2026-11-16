#include "deflate.h"

#include <stdlib.h>
#include <string.h>

#define WSIZE      32768
#define HASH_BITS  15
#define HASH_SIZE  (1u << HASH_BITS)
#define MAX_PROBES 128 /* same probe budget as the Windows tdefl flags */
#define MIN_MATCH  3
#define MAX_MATCH  258

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
				      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
				      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
				       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
				       6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
				       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

struct bw {
	uint8_t *out;
	size_t cap, pos;
	uint64_t acc;
	unsigned nbits;
	int overflow;
};

static void put_bits(struct bw *w, uint32_t v, unsigned n)
{
	w->acc |= (uint64_t)v << w->nbits;
	w->nbits += n;
	while (w->nbits >= 8) {
		if (w->pos < w->cap)
			w->out[w->pos++] = (uint8_t)w->acc;
		else
			w->overflow = 1;
		w->acc >>= 8;
		w->nbits -= 8;
	}
}

/* Huffman codes are defined MSB-first; the bit stream is LSB-first. */
static void put_code(struct bw *w, uint32_t code, unsigned len)
{
	uint32_t r = 0;
	for (unsigned i = 0; i < len; i++)
		r |= ((code >> i) & 1u) << (len - 1 - i);
	put_bits(w, r, len);
}

static void put_litlen(struct bw *w, unsigned sym)
{
	if (sym < 144)
		put_code(w, 0x30 + sym, 8);
	else if (sym < 256)
		put_code(w, 0x190 + (sym - 144), 9);
	else if (sym < 280)
		put_code(w, sym - 256, 7);
	else
		put_code(w, 0xC0 + (sym - 280), 8);
}

static void put_match(struct bw *w, unsigned len, unsigned dist)
{
	unsigned li = 28;
	while (li > 0 && len_base[li] > len)
		li--;
	put_litlen(w, 257 + li);
	if (len_extra[li])
		put_bits(w, len - len_base[li], len_extra[li]);
	unsigned di = 29;
	while (di > 0 && dist_base[di] > dist)
		di--;
	put_code(w, di, 5);
	if (dist_extra[di])
		put_bits(w, dist - dist_base[di], dist_extra[di]);
}

static uint32_t hash3(const uint8_t *p)
{
	return ((uint32_t)p[0] << 10 ^ (uint32_t)p[1] << 5 ^ p[2]) & (HASH_SIZE - 1);
}

long tdx_deflate_fixed(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
	int32_t *head = malloc(HASH_SIZE * sizeof *head);
	int32_t *prev = malloc(WSIZE * sizeof *prev);
	if (!head || !prev) {
		free(head);
		free(prev);
		return -1;
	}
	for (size_t i = 0; i < HASH_SIZE; i++)
		head[i] = -1;

	struct bw w = {.out = out, .cap = cap};
	put_bits(&w, 1, 1); /* BFINAL */
	put_bits(&w, 1, 2); /* BTYPE = 01 fixed Huffman */

	size_t i = 0;
	while (i < n) {
		unsigned best_len = 0, best_dist = 0;
		if (i + MIN_MATCH <= n) {
			uint32_t h = hash3(in + i);
			int32_t cand = head[h];
			size_t maxlen = n - i < MAX_MATCH ? n - i : MAX_MATCH;
			for (int probes = 0; cand >= 0 && probes < MAX_PROBES; probes++) {
				size_t dist = i - (size_t)cand;
				if (dist > WSIZE)
					break;
				if (in[cand + best_len] == in[i + best_len]) {
					size_t l = 0;
					while (l < maxlen && in[cand + l] == in[i + l])
						l++;
					if (l > best_len) {
						best_len = (unsigned)l;
						best_dist = (unsigned)dist;
						if (l == maxlen)
							break;
					}
				}
				int32_t nx = prev[cand & (WSIZE - 1)];
				if (nx >= cand)
					break;
				cand = nx;
			}
		}
		size_t adv = best_len >= MIN_MATCH ? best_len : 1;
		if (best_len >= MIN_MATCH)
			put_match(&w, best_len, best_dist);
		else
			put_litlen(&w, in[i]);
		/* insert every covered position into the hash chains (greedy parse) */
		for (size_t k = 0; k < adv; k++, i++) {
			if (i + MIN_MATCH <= n) {
				uint32_t h = hash3(in + i);
				prev[i & (WSIZE - 1)] = head[h];
				head[h] = (int32_t)i;
			}
		}
	}
	put_litlen(&w, 256);          /* end of block */
	put_bits(&w, 0, (8 - w.nbits % 8) % 8); /* flush to byte */
	free(head);
	free(prev);
	return w.overflow ? -1 : (long)w.pos;
}
