/*
 * pg_tiktoken_c.c
 *
 * PostgreSQL C extension: tiktoken BPE tokenization.
 * Supports encodings: cl100k_base, r50k_base, p50k_base, p50k_edit
 * and model-name aliases (gpt-4, gpt-3.5-turbo, text-davinci-*, gpt2, …).
 *
 * Algorithm:
 *   1. Split input text with a PCRE2 Unicode-aware regex (pre-tokenization).
 *   2. Special tokens (<|endoftext|> etc.) are recognised before BPE.
 *   3. Each word piece is BPE-encoded using a hash-table vocab lookup.
 *
 * Performance notes vs. Rust/pgrx version:
 *   - No Rust/C FFI overhead, no pgrx wrapper allocations.
 *   - Vocab hash table uses open-addressing with FNV-1a, fits L2/L3 cache.
 *   - Encoder state is cached per-process in TopMemoryContext.
 *   - BPE loop avoids heap allocation for typical token lengths.
 *
 * Build requirements: PCRE2 (libpcre2-8), tiktoken vocab files in
 *   TIKTOKEN_DATA_DIR (set by Makefile from pg_config --sharedir).
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>

PG_MODULE_MAGIC;

/* ============================================================
 * Compile-time configuration
 * ============================================================ */
#ifndef TIKTOKEN_DATA_DIR
#define TIKTOKEN_DATA_DIR "/usr/local/pgsql/share/extension/pg_tiktoken_c"
#endif

#define INVALID_RANK    UINT32_MAX
/* Maximum bytes in a single pre-tokenized word piece.
 * Unicode chars can be up to 4 bytes; typical words are short. */
#define MAX_PIECE_BYTES 2048
/* Output token buffer per piece */
#define MAX_PIECE_TOKENS (MAX_PIECE_BYTES)

/* ============================================================
 * Base64 decoder (RFC 4648, standard alphabet)
 * ============================================================ */
static const signed char b64val[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* 0x00-0x0F */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, /* 0x10-0x1F */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, /* 0x20-0x2F  '+' '/' */
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1, /* 0x30-0x3F  '0'-'9' */
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, /* 0x40-0x4F  'A'-'O' */
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1, /* 0x50-0x5F  'P'-'Z' */
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, /* 0x60-0x6F  'a'-'o' */
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1, /* 0x70-0x7F  'p'-'z' */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

/*
 * Decode base64 src[0..srclen) into dst[].
 * dst must hold at least ceil(srclen * 3/4) bytes.
 * Returns number of decoded bytes, or -1 on error.
 */
static int
b64_decode(const char *src, int srclen, uint8_t *dst)
{
    int o = 0, i = 0;
    while (i < srclen) {
        /* skip padding */
        while (i < srclen && src[i] == '=') i++;
        if (i >= srclen) break;

        int a = b64val[(unsigned char)src[i++]];
        int b = (i < srclen) ? b64val[(unsigned char)src[i++]] : -1;
        int c = (i < srclen && src[i] != '=') ? b64val[(unsigned char)src[i++]] : -1;
        int d = (i < srclen && src[i] != '=') ? b64val[(unsigned char)src[i++]] : -1;

        if (a < 0 || b < 0) return -1;
        dst[o++] = (uint8_t)((a << 2) | (b >> 4));
        if (c >= 0) dst[o++] = (uint8_t)(((b & 0xf) << 4) | (c >> 2));
        if (d >= 0) dst[o++] = (uint8_t)(((c & 0x3) << 6) | d);

        /* skip trailing '=' */
        while (i < srclen && src[i] == '=') i++;
    }
    return o;
}

/* ============================================================
 * Open-addressing hash table: bytes -> uint32_t rank
 * ============================================================ */
typedef struct {
    uint32_t hash;    /* FNV-1a hash of key; 0 means empty slot */
    uint32_t rank;
    uint32_t key_off; /* byte offset into key_pool */
    uint16_t key_len;
} VocabEntry;

typedef struct {
    VocabEntry *entries;
    uint32_t    cap;      /* power-of-2 capacity */
    uint32_t    mask;     /* cap - 1 */
    uint32_t    count;
    uint8_t    *key_pool;
    uint32_t    pool_cap;
    uint32_t    pool_used;
} VocabTable;

/* FNV-1a 32-bit: never returns 0 (we use 0 as "empty") */
static inline uint32_t
fnv1a32(const uint8_t *data, int len)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++)
        h = (h ^ data[i]) * 16777619u;
    return h ? h : 1u;
}

static uint32_t
vocab_lookup(const VocabTable *ht, const uint8_t *key, int klen)
{
    if (ht->cap == 0) return INVALID_RANK;
    uint32_t h = fnv1a32(key, klen);
    uint32_t i = h & ht->mask;
    for (;;) {
        const VocabEntry *e = &ht->entries[i];
        if (e->hash == 0)
            return INVALID_RANK;
        if (e->hash == h && e->key_len == (uint16_t)klen &&
            memcmp(ht->key_pool + e->key_off, key, klen) == 0)
            return e->rank;
        i = (i + 1) & ht->mask;
    }
}

static void
vocab_insert(VocabTable *ht, const uint8_t *key, int klen, uint32_t rank)
{
    /* Safety: table must not be more than 75% full */
    if (ht->count >= ht->cap * 3 / 4)
        elog(ERROR, "pg_tiktoken_c: vocab hash table overflow "
             "(cap=%u count=%u) — increase size estimate",
             ht->cap, ht->count);

    uint32_t h = fnv1a32(key, klen);
    uint32_t i = h & ht->mask;
    while (ht->entries[i].hash != 0)
        i = (i + 1) & ht->mask;

    /* grow key pool if needed */
    if (ht->pool_used + (uint32_t)klen > ht->pool_cap)
    {
        uint32_t newcap = ht->pool_cap * 2 + klen + 1;
        ht->key_pool = repalloc(ht->key_pool, newcap);
        ht->pool_cap = newcap;
    }

    ht->entries[i].hash    = h;
    ht->entries[i].rank    = rank;
    ht->entries[i].key_off = ht->pool_used;
    ht->entries[i].key_len = (uint16_t)klen;
    memcpy(ht->key_pool + ht->pool_used, key, klen);
    ht->pool_used += klen;
    ht->count++;
}

/* Allocate a VocabTable sized for at most n entries (load factor ~0.65). */
static VocabTable *
vocab_create(MemoryContext mcxt, uint32_t n)
{
    VocabTable *ht = MemoryContextAllocZero(mcxt, sizeof(VocabTable));
    /* round up to next power of 2, at least 2x n */
    uint32_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    ht->cap      = cap;
    ht->mask     = cap - 1;
    ht->entries  = MemoryContextAllocZero(mcxt, cap * sizeof(VocabEntry));
    ht->pool_cap = n * 6; /* rough: avg 6 bytes/token */
    ht->key_pool = MemoryContextAlloc(mcxt, ht->pool_cap);
    return ht;
}

/* ============================================================
 * BPE word-piece encoder
 * ============================================================ */

/*
 * Encode one word piece (byte string) using BPE.
 * out[]  receives token IDs (must hold MAX_PIECE_TOKENS entries).
 * Returns number of tokens written, or -1 if a byte has no vocab entry.
 */
static int
bpe_encode_piece(const VocabTable *vocab,
                 const uint8_t *piece, int plen,
                 uint32_t *out)
{
    if (plen == 0) return 0;

    if (plen == 1) {
        uint32_t r = vocab_lookup(vocab, piece, 1);
        if (r == INVALID_RANK) return -1;
        out[0] = r;
        return 1;
    }

    /*
     * parts[i] = byte offset of the i-th current token boundary.
     * Initially: 0, 1, 2, ..., plen  (each byte is its own token).
     * n = number of offsets = number_of_tokens + 1.
     *
     * pair_rank[i] = vocab rank of the merged token spanning
     *                piece[parts[i] .. parts[i+2]),
     *                or INVALID_RANK if no such merge exists.
     */
    int      parts[MAX_PIECE_BYTES + 1];
    uint32_t pair_rank[MAX_PIECE_BYTES];
    int      n = plen + 1;

    for (int i = 0; i <= plen; i++) parts[i] = i;

    /* pre-compute initial pair ranks */
    for (int i = 0; i < n - 2; i++)
        pair_rank[i] = vocab_lookup(vocab, piece + parts[i],
                                    parts[i + 2] - parts[i]);
    pair_rank[n - 2] = INVALID_RANK; /* last pair has no right neighbour */

    for (;;) {
        /* find minimum-rank pair */
        uint32_t min_rank = INVALID_RANK;
        int      min_i    = -1;
        for (int i = 0; i < n - 2; i++) {
            if (pair_rank[i] < min_rank) {
                min_rank = pair_rank[i];
                min_i    = i;
            }
        }
        if (min_rank == INVALID_RANK) break; /* no more merges */

        /*
         * Merge parts[min_i] and parts[min_i+1]:
         * delete the boundary at parts[min_i+1].
         */
        int del = min_i + 1;
        memmove(&parts[del], &parts[del + 1], (n - del - 1) * sizeof(int));
        memmove(&pair_rank[del], &pair_rank[del + 1],
                (n - del - 2) * sizeof(uint32_t));
        n--;

        /* recompute pair ranks for the two affected positions */
        pair_rank[min_i] = (min_i + 2 < n)
            ? vocab_lookup(vocab, piece + parts[min_i],
                           parts[min_i + 2] - parts[min_i])
            : INVALID_RANK;
        if (min_i > 0)
            pair_rank[min_i - 1] = (min_i + 1 < n)
                ? vocab_lookup(vocab, piece + parts[min_i - 1],
                               parts[min_i + 1] - parts[min_i - 1])
                : INVALID_RANK;
    }

    /* write output token IDs */
    int ntok = n - 1;
    for (int i = 0; i < ntok; i++) {
        uint32_t r = vocab_lookup(vocab, piece + parts[i],
                                  parts[i + 1] - parts[i]);
        if (r == INVALID_RANK) return -1;
        out[i] = r;
    }
    return ntok;
}

/* ============================================================
 * Encoder definition
 * ============================================================ */

/* A special token: literal text + its rank/ID */
typedef struct {
    const char *text;
    int         text_len;
    uint32_t    rank;
} SpecialToken;

typedef struct EncoderState {
    const char     *name;          /* e.g. "cl100k_base" */
    const char     *pattern_str;   /* PCRE2 pattern */
    pcre2_code     *regex;         /* compiled pattern */
    VocabTable     *vocab;         /* token → rank */
    SpecialToken   *special;       /* sorted by text_len DESC for greedy match */
    int             n_special;
    bool            loaded;        /* vocab has been read from disk */
} EncoderState;

/* ============================================================
 * Regex patterns (PCRE2, UTF-8 + UCP)
 * ============================================================ */

/* GPT-2 style: r50k_base, p50k_base, p50k_edit */
#define GPT2_PATTERN \
    "'s|'t|'re|'ve|'m|'ll|'d" \
    "| ?\\p{L}+" \
    "| ?\\p{N}+" \
    "| ?[^\\s\\p{L}\\p{N}]+" \
    "|\\s+(?!\\S)" \
    "|\\s+"

/* GPT-4 / cl100k_base style */
#define CL100K_PATTERN \
    "(?i:'s|'t|'re|'ve|'m|'ll|'d)" \
    "|[^\\r\\n\\p{L}\\p{N}]?\\p{L}+" \
    "|\\p{N}{1,3}" \
    "| ?[^\\s\\p{L}\\p{N}]+[\\r\\n]*" \
    "|\\s*[\\r\\n]+" \
    "|\\s+(?!\\S)" \
    "|\\s+"

/* ============================================================
 * Special tokens per encoding
 * ============================================================ */

/* cl100k_base special tokens */
static SpecialToken cl100k_special[] = {
    { "<|endoftext|>",  13, 100257 },
    { "<|fim_prefix|>", 14, 100258 },
    { "<|fim_middle|>", 14, 100259 },
    { "<|fim_suffix|>", 14, 100260 },
    { "<|endofprompt|>",15, 100276 },
};

/* r50k_base special tokens */
static SpecialToken r50k_special[] = {
    { "<|endoftext|>", 13, 50256 },
};

/* p50k_base special tokens */
static SpecialToken p50k_base_special[] = {
    { "<|endoftext|>", 13, 50256 },
};

/* p50k_edit special tokens */
static SpecialToken p50k_edit_special[] = {
    { "<|endoftext|>",  13, 50256 },
    { "<|fim_prefix|>", 14, 50281 },
    { "<|fim_middle|>", 14, 50282 },
    { "<|fim_suffix|>", 14, 50283 },
};

/* o200k_base special tokens (GPT-4o, o1, o3 …) */
static SpecialToken o200k_special[] = {
    { "<|endoftext|>",  13, 199999 },
    { "<|endofprompt|>",15, 200018 },
};

/* ============================================================
 * Global encoder cache (5 encoders, process-local)
 * ============================================================ */

#define N_ENCODERS 5

static EncoderState g_encoders[N_ENCODERS] = {
    { "cl100k_base", CL100K_PATTERN, NULL, NULL,
      cl100k_special,    lengthof(cl100k_special),    false },
    { "o200k_base",  CL100K_PATTERN, NULL, NULL,
      o200k_special,     lengthof(o200k_special),     false },
    { "r50k_base",   GPT2_PATTERN,   NULL, NULL,
      r50k_special,      lengthof(r50k_special),      false },
    { "p50k_base",   GPT2_PATTERN,   NULL, NULL,
      p50k_base_special, lengthof(p50k_base_special), false },
    { "p50k_edit",   GPT2_PATTERN,   NULL, NULL,
      p50k_edit_special, lengthof(p50k_edit_special), false },
};

/* ============================================================
 * Model-name → encoding-name alias table
 * ============================================================ */
typedef struct { const char *model; const char *encoding; } ModelAlias;
static const ModelAlias model_aliases[] = {
    /* o200k_base: GPT-4o, o1, o3 family */
    { "gpt-4o",                      "o200k_base" },
    { "gpt-4o-2024-05-13",           "o200k_base" },
    { "gpt-4o-2024-08-06",           "o200k_base" },
    { "gpt-4o-2024-11-20",           "o200k_base" },
    { "gpt-4o-mini",                 "o200k_base" },
    { "gpt-4o-mini-2024-07-18",      "o200k_base" },
    { "o1",                          "o200k_base" },
    { "o1-2024-12-17",               "o200k_base" },
    { "o1-mini",                     "o200k_base" },
    { "o1-mini-2024-09-12",          "o200k_base" },
    { "o1-preview",                  "o200k_base" },
    { "o1-preview-2024-09-12",       "o200k_base" },
    { "o3",                          "o200k_base" },
    { "o3-mini",                     "o200k_base" },
    { "o4-mini",                     "o200k_base" },
    /* cl100k_base: GPT-4 (non-o), GPT-3.5, embeddings */
    { "gpt-4",                       "cl100k_base" },
    { "gpt-4-0314",                  "cl100k_base" },
    { "gpt-4-0613",                  "cl100k_base" },
    { "gpt-4-32k",                   "cl100k_base" },
    { "gpt-4-32k-0314",              "cl100k_base" },
    { "gpt-4-turbo",                 "cl100k_base" },
    { "gpt-4-turbo-preview",         "cl100k_base" },
    { "gpt-3.5-turbo",               "cl100k_base" },
    { "gpt-3.5-turbo-0301",          "cl100k_base" },
    { "gpt-3.5-turbo-0613",          "cl100k_base" },
    { "gpt-3.5-turbo-16k",           "cl100k_base" },
    { "gpt-3.5-turbo-16k-0613",      "cl100k_base" },
    { "text-embedding-ada-002",       "cl100k_base" },
    { "text-embedding-3-small",       "cl100k_base" },
    { "text-embedding-3-large",       "cl100k_base" },
    /* p50k_base */
    { "text-davinci-003",             "p50k_base" },
    { "text-davinci-002",             "p50k_base" },
    { "text-similarity-davinci-001",  "p50k_base" },
    { "code-davinci-002",             "p50k_base" },
    { "code-davinci-001",             "p50k_base" },
    { "code-cushman-002",             "p50k_base" },
    { "code-cushman-001",             "p50k_base" },
    { "davinci-codex",                "p50k_base" },
    { "cushman-codex",                "p50k_base" },
    /* p50k_edit */
    { "text-davinci-edit-001",        "p50k_edit" },
    { "code-davinci-edit-001",        "p50k_edit" },
    /* r50k_base (legacy GPT-2 era) */
    { "gpt2",                         "r50k_base" },
    { "davinci",                      "r50k_base" },
    { "curie",                        "r50k_base" },
    { "babbage",                      "r50k_base" },
    { "ada",                          "r50k_base" },
    { "text-davinci-001",             "r50k_base" },
    { "text-curie-001",               "r50k_base" },
    { "text-babbage-001",             "r50k_base" },
    { "text-ada-001",                 "r50k_base" },
    { NULL, NULL }
};

/* Resolve model alias → encoding name. Returns input if no alias found. */
static const char *
resolve_encoding(const char *selector)
{
    for (int i = 0; model_aliases[i].model; i++)
        if (strcmp(selector, model_aliases[i].model) == 0)
            return model_aliases[i].encoding;
    return selector;
}

/* Find encoder by name. Returns NULL if not found. */
static EncoderState *
find_encoder(const char *name)
{
    for (int i = 0; i < N_ENCODERS; i++)
        if (strcmp(g_encoders[i].name, name) == 0)
            return &g_encoders[i];
    return NULL;
}

/* ============================================================
 * Vocab file loader
 * ============================================================ */

/*
 * Load a tiktoken vocab file: each line is "<base64_token> <rank>\n".
 * Returns number of entries loaded, or -1 on error.
 */
static int
load_vocab_file(VocabTable *vocab, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char    line[8192];
    uint8_t token_buf[4096];
    int     count = 0;

    while (fgets(line, sizeof(line), f)) {
        /* strip trailing newline */
        int llen = strlen(line);
        while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r'))
            line[--llen] = '\0';
        if (llen == 0) continue;

        /* find space separator */
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';

        uint32_t rank = (uint32_t)strtoul(sp + 1, NULL, 10);

        int tlen = b64_decode(line, sp - line, token_buf);
        if (tlen <= 0) continue;

        vocab_insert(vocab, token_buf, tlen, rank);
        count++;
    }
    fclose(f);
    return count;
}

/* ============================================================
 * Encoder initialisation (on first use)
 * ============================================================ */

static void
encoder_load(EncoderState *enc)
{
    if (enc->loaded) return;

    MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);

    /* compile regex */
    if (enc->regex == NULL)
    {
        int        errcode;
        PCRE2_SIZE erroff;
        enc->regex = pcre2_compile(
            (PCRE2_SPTR)enc->pattern_str,
            PCRE2_ZERO_TERMINATED,
            PCRE2_UTF | PCRE2_UCP,
            &errcode, &erroff, NULL);

        if (!enc->regex)
        {
            PCRE2_UCHAR errbuf[256];
            pcre2_get_error_message(errcode, errbuf, sizeof(errbuf));
            MemoryContextSwitchTo(oldcxt);
            ereport(ERROR,
                    (errmsg("pg_tiktoken_c: regex compile error for %s: %s",
                            enc->name, (char *)errbuf)));
        }
        /* JIT compile for speed if available */
        pcre2_jit_compile(enc->regex, PCRE2_JIT_COMPLETE);
    }

    /* load vocab file */
    char path[MAXPGPATH];
    snprintf(path, sizeof(path), "%s/%s.tiktoken",
             TIKTOKEN_DATA_DIR, enc->name);

    /* pre-estimate: cl100k has ~100k entries, others ~50k */
    uint32_t est;
    if (strncmp(enc->name, "o200k", 5) == 0)
        est = 210000;
    else if (strncmp(enc->name, "cl100k", 6) == 0)
        est = 110000;
    else
        est = 55000;
    enc->vocab = vocab_create(TopMemoryContext, est);

    int n = load_vocab_file(enc->vocab, path);
    if (n < 0)
    {
        MemoryContextSwitchTo(oldcxt);
        ereport(ERROR,
                (errmsg("pg_tiktoken_c: cannot open vocab file %s", path),
                 errhint("Run: make -C contrib/pg_tiktoken_c download-data")));
    }
    if (n == 0)
    {
        MemoryContextSwitchTo(oldcxt);
        ereport(ERROR,
                (errmsg("pg_tiktoken_c: vocab file is empty: %s", path)));
    }

    enc->loaded = true;
    MemoryContextSwitchTo(oldcxt);
}

/* ============================================================
 * Full-text tokenizer: special tokens + BPE
 * ============================================================ */

/*
 * Search for a special token at text[pos..text_len).
 * Returns index in enc->special[] if found (and sets *matched_len),
 * or -1 if none match.
 */
static int
match_special(const EncoderState *enc,
              const char *text, int text_len, int pos,
              int *matched_len)
{
    for (int i = 0; i < enc->n_special; i++) {
        int slen = enc->special[i].text_len;
        if (pos + slen <= text_len &&
            memcmp(text + pos, enc->special[i].text, slen) == 0) {
            *matched_len = slen;
            return i;
        }
    }
    return -1;
}

/*
 * Encode the segment text[seg_start..seg_end) with BPE (no special tokens).
 * Appends token IDs to *tokens, growing the array as needed.
 * *ntokens is updated; *tokens_cap is the current palloc capacity.
 */
static void
encode_segment(EncoderState *enc,
               const char *text, int seg_start, int seg_end,
               int64 **tokens, int *ntokens, int *tokens_cap)
{
    if (seg_start >= seg_end) return;

    pcre2_match_data *md = pcre2_match_data_create_from_pattern(enc->regex, NULL);
    PCRE2_SIZE offset = (PCRE2_SIZE)seg_start;
    int         text_len = seg_end;
    uint32_t    piece_out[MAX_PIECE_TOKENS];

    while ((int)offset < text_len) {
        int rc = pcre2_match(enc->regex,
                             (PCRE2_SPTR)text, (PCRE2_SIZE)text_len,
                             offset, 0, md, NULL);
        if (rc < 0) break; /* no more matches */

        PCRE2_SIZE *ov   = pcre2_get_ovector_pointer(md);
        PCRE2_SIZE  msta = ov[0];
        PCRE2_SIZE  mend = ov[1];

        if (mend == msta) { offset = mend + 1; continue; } /* zero-length guard */

        int nout = bpe_encode_piece(enc->vocab,
                                    (const uint8_t *)text + msta,
                                    (int)(mend - msta),
                                    piece_out);
        if (nout < 0) {
            pcre2_match_data_free(md);
            ereport(ERROR,
                    (errmsg("pg_tiktoken_c: BPE encoding failed (byte not in vocab)")));
        }

        /* grow output array if needed */
        if (*ntokens + nout > *tokens_cap) {
            *tokens_cap = (*ntokens + nout) * 2 + 64;
            *tokens = repalloc(*tokens, *tokens_cap * sizeof(int64));
        }
        for (int j = 0; j < nout; j++)
            (*tokens)[(*ntokens)++] = (int64)piece_out[j];

        offset = mend;
    }

    pcre2_match_data_free(md);
}

/*
 * Tokenize the entire text, honouring special tokens.
 * Returns a palloc'd array of int64 token IDs; sets *ntokens.
 */
static int64 *
tiktoken_tokenize(EncoderState *enc,
                  const char *text, int text_len,
                  int *ntokens)
{
    int   cap    = 64;
    int64 *toks  = palloc(cap * sizeof(int64));
    *ntokens     = 0;

    int pos = 0;
    int seg_start = 0; /* start of the current plain-text segment */

    while (pos < text_len) {
        int matched_len = 0;
        int si = match_special(enc, text, text_len, pos, &matched_len);
        if (si >= 0) {
            /* flush plain segment before this special token */
            encode_segment(enc, text, seg_start, pos,
                           &toks, ntokens, &cap);

            /* emit the special token ID */
            if (*ntokens + 1 > cap) {
                cap = (*ntokens + 1) * 2 + 8;
                toks = repalloc(toks, cap * sizeof(int64));
            }
            toks[(*ntokens)++] = (int64)enc->special[si].rank;
            pos += matched_len;
            seg_start = pos;
        } else {
            pos++;
        }
    }
    /* flush trailing segment */
    encode_segment(enc, text, seg_start, text_len, &toks, ntokens, &cap);

    return toks;
}

/* ============================================================
 * PostgreSQL-callable functions
 * ============================================================ */

PG_FUNCTION_INFO_V1(tiktoken_encode);
Datum
tiktoken_encode(PG_FUNCTION_ARGS)
{
    text *arg_enc  = PG_GETARG_TEXT_PP(0);
    text *arg_text = PG_GETARG_TEXT_PP(1);

    const char *enc_name = resolve_encoding(text_to_cstring(arg_enc));
    EncoderState *enc = find_encoder(enc_name);
    if (!enc)
        ereport(ERROR,
                (errmsg("'%s': unknown model or encoder", enc_name)));

    encoder_load(enc);

    const char *input    = VARDATA_ANY(arg_text);
    int         input_len = VARSIZE_ANY_EXHDR(arg_text);

    int   ntokens = 0;
    int64 *tokens = tiktoken_tokenize(enc, input, input_len, &ntokens);

    /* Build a PostgreSQL int8[] array */
    Datum    *datums = palloc(ntokens * sizeof(Datum));
    for (int i = 0; i < ntokens; i++)
        datums[i] = Int64GetDatum(tokens[i]);

    int dims[1]  = { ntokens };
    int lbs[1]   = { 1 };
    ArrayType *arr = construct_md_array(datums, NULL, 1, dims, lbs,
                                        INT8OID, sizeof(int64), true, 'd');
    pfree(datums);
    pfree(tokens);

    PG_RETURN_ARRAYTYPE_P(arr);
}

PG_FUNCTION_INFO_V1(tiktoken_count);
Datum
tiktoken_count(PG_FUNCTION_ARGS)
{
    text *arg_enc  = PG_GETARG_TEXT_PP(0);
    text *arg_text = PG_GETARG_TEXT_PP(1);

    const char *enc_name = resolve_encoding(text_to_cstring(arg_enc));
    EncoderState *enc = find_encoder(enc_name);
    if (!enc)
        ereport(ERROR,
                (errmsg("'%s': unknown model or encoder", enc_name)));

    encoder_load(enc);

    const char *input    = VARDATA_ANY(arg_text);
    int         input_len = VARSIZE_ANY_EXHDR(arg_text);

    int   ntokens = 0;
    int64 *tokens = tiktoken_tokenize(enc, input, input_len, &ntokens);
    pfree(tokens);

    PG_RETURN_INT64((int64)ntokens);
}
