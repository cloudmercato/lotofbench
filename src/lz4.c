/* LZ4 block-format codec for the LotofBench File Compression workload.
 *
 * Format reference: https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 * A block is a sequence of: [token][literals][offset][match extension].
 * The token packs the literal length (4 bits, 15 = extended) and the
 * match length minus MINMATCH (4 bits, 15 = extended). Every sequence
 * but the last carries a 16-bit little-endian backward offset; the last
 * 5 bytes of the input are always encoded as pure literals.
 *
 * Only the encoding side does heuristics; the decoder is a straight
 * format walker that also enforces every bound the format implies.
 */

#include "lz4.h"

#include <string.h>

#define MINMATCH         4
#define HASH_LOG         16
#define HASH_SIZE        (1u << HASH_LOG)
#define MAX_DISTANCE     65535u
#define LAST_LITERALS    5   /* trailing bytes that must stay literals */
#define MF_LIMIT         12  /* matches may not start in the last 12 bytes */
#define HIGH_CHAIN_DEPTH 16  /* candidates examined per probe on level 2 */
#define NO_POS           0xFFFFFFFFu
#define MAX_BLOCK_SIZE   0x7E000000u  /* single-block cap from the spec */

size_t lz4_compress_bound(size_t n)
{
    return n + n / 255 + 16;
}

/* Knuth multiplicative hash over a 4-byte little-endian word. Only used
 * by the encoder, so endianness is irrelevant as long as it is
 * consistent within one run. */
static uint32_t read32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint32_t hash32(uint32_t seq)
{
    return (uint32_t)((seq * 2654435761u) >> (32 - HASH_LOG));
}

/* Bytes needed to emit one sequence of the given literal/match lengths. */
static size_t seq_bytes(size_t litlen, size_t matchlen)
{
    size_t n = 1 + litlen + 2; /* token + literals + offset */
    if (litlen >= 15)
        n += (litlen - 15) / 255 + 1;
    if (matchlen >= MINMATCH + 15)
        n += (matchlen - MINMATCH - 15) / 255 + 1;
    return n;
}

static size_t literal_only_bytes(size_t litlen)
{
    size_t n = 1 + litlen;
    if (litlen >= 15)
        n += (litlen - 15) / 255 + 1;
    return n;
}

/* Forward match length between position s (current) and candidate c,
 * capped so the match ends at least LAST_LITERALS bytes before the end
 * of the block. Callers verified the first 4 bytes already. */
static size_t match_len(const uint8_t *src, size_t s, size_t c, size_t max_n)
{
    size_t n = 0;
    while (n < max_n && src[c + n] == src[s + n])
        n++;
    return n;
}

/* Emit the trailing pure-literal run (token without offset). */
static size_t emit_last_literals(const uint8_t *src, size_t len,
                                 uint8_t *out, size_t op, size_t bound)
{
    size_t litlen = len;
    size_t need = literal_only_bytes(litlen);
    if (op + need > bound)
        return 0;
    if (litlen >= 15) {
        out[op++] = 0xF0;
        size_t rem = litlen - 15;
        while (rem >= 255) {
            out[op++] = 255;
            rem -= 255;
        }
        out[op++] = (uint8_t)rem;
    } else {
        out[op++] = (uint8_t)(litlen << 4);
    }
    memcpy(out + op, src, litlen);
    op += litlen;
    return op;
}

size_t lz4_compress(const uint8_t *src, size_t src_len, uint8_t *out,
                    int level)
{
    uint32_t table[HASH_SIZE];
    uint16_t chain[1u << 16];
    const int chained = (level == LZ4_LEVEL_HIGH);
    const size_t bound = lz4_compress_bound(src_len);
    size_t ip = 0, anchor = 0, op = 0;

    if (src_len > MAX_BLOCK_SIZE || (level != LZ4_LEVEL_FAST &&
                                     level != LZ4_LEVEL_HIGH))
        return 0;

    memset(table, 0xFF, sizeof(table)); /* NO_POS everywhere */
    memset(chain, 0, sizeof(chain));

    /* Too short to hold a match plus the mandatory literal tail. */
    if (src_len < MF_LIMIT + MINMATCH)
        return emit_last_literals(src, src_len, out, op, bound);

    const size_t match_limit = src_len - MF_LIMIT;

    while (ip < match_limit) {
        uint32_t seq = read32(src + ip);
        uint32_t h = hash32(seq);
        size_t cand = table[h];
        size_t best = 0, best_len = 0;
        unsigned depth = HIGH_CHAIN_DEPTH;

        /* Link this position into the chain before searching, but take
         * the previous table slot as the first candidate. */
        table[h] = (uint32_t)ip;
        if (cand != NO_POS && ip - cand <= MAX_DISTANCE)
            chain[ip & 0xFFFFu] = (uint16_t)(ip - cand);
        else
            chain[ip & 0xFFFFu] = 0;

        while (cand != NO_POS && ip - cand <= MAX_DISTANCE) {
            if (read32(src + cand) == seq) {
                size_t len = match_len(src, ip, cand,
                                       src_len - ip - LAST_LITERALS);
                if (len > best_len) {
                    best_len = len;
                    best = cand;
                }
                if (!chained)
                    break;
            }
            if (!chained || depth-- == 0)
                break;
            uint16_t d = chain[cand & 0xFFFFu];
            cand = d ? cand - d : NO_POS;
        }

        if (best_len >= MINMATCH) {
            size_t litlen = ip - anchor;
            size_t ml = best_len;
            size_t off = ip - best;
            size_t need = seq_bytes(litlen, ml);
            unsigned token_lit =
                (unsigned)(litlen >= 15 ? 15 : litlen);
            unsigned token_match =
                (unsigned)(ml - MINMATCH >= 15 ? 15 : ml - MINMATCH);

            if (op + need > bound)
                return 0;

            out[op++] = (uint8_t)((token_lit << 4) | token_match);
            if (litlen >= 15) {
                size_t rem = litlen - 15;
                while (rem >= 255) {
                    out[op++] = 255;
                    rem -= 255;
                }
                out[op++] = (uint8_t)rem;
            }
            memcpy(out + op, src + anchor, litlen);
            op += litlen;
            out[op++] = (uint8_t)(off & 0xFF);
            out[op++] = (uint8_t)(off >> 8);
            if (ml - MINMATCH >= 15) {
                size_t rem = ml - MINMATCH - 15;
                while (rem >= 255) {
                    out[op++] = 255;
                    rem -= 255;
                }
                out[op++] = (uint8_t)rem;
            }
            ip += ml;
            anchor = ip;
        } else {
            ip++;
        }
    }

    return emit_last_literals(src + anchor, src_len - anchor, out, op, bound);
}

size_t lz4_decompress(const uint8_t *src, size_t src_len, uint8_t *out,
                      size_t out_cap)
{
    size_t ip = 0, op = 0;

    while (ip < src_len) {
        unsigned token = src[ip++];
        size_t litlen = token >> 4;

        if (litlen == 15) {
            unsigned b;
            do {
                if (ip >= src_len)
                    return 0;
                b = src[ip++];
                litlen += b;
            } while (b == 255);
        }
        if (litlen > src_len - ip || litlen > out_cap - op)
            return 0;
        memcpy(out + op, src + ip, litlen);
        ip += litlen;
        op += litlen;

        if (ip == src_len)
            break; /* trailing literal run: end of block */

        if (src_len - ip < 2)
            return 0; /* truncated offset */
        size_t off = (size_t)src[ip] | ((size_t)src[ip + 1] << 8);
        ip += 2;
        if (off == 0 || off > op)
            return 0; /* offsets point backward, 1..65535 */

        size_t ml = (token & 0xF) + MINMATCH;
        if ((token & 0xF) == 15) {
            unsigned b;
            do {
                if (ip >= src_len)
                    return 0;
                b = src[ip++];
                ml += b;
            } while (b == 255);
        }
        if (ml > out_cap - op)
            return 0;

        /* Overlapping matches (off < ml) copy byte-by-byte, which is
         * exactly how run-length behaviour is specified. */
        const uint8_t *mp = out + op - off;
        if (off < ml) {
            for (size_t i = 0; i < ml; i++)
                out[op + i] = mp[i];
        } else {
            memcpy(out + op, mp, ml);
        }
        op += ml;
    }
    return op;
}
