# pg_tiktoken_c

A PostgreSQL extension that brings OpenAI's [tiktoken](https://github.com/openai/tiktoken) BPE tokenizer into SQL — implemented in pure C for maximum performance.

**Over 100× faster than [pg_tiktoken](https://github.com/kelvich/pg_tiktoken)** (the popular Rust-based alternative). For short to medium prompts the measured speedup reaches 1,700–2,700×; see [Performance](#performance) for full benchmark results.

Use it to count tokens before calling OpenAI APIs, build RAG pipelines that respect context limits, or store token arrays alongside your text data.

## Features

- **Token counting** — know your token budget before hitting the API
- **Token ID arrays** — store or inspect the exact BPE token sequence
- **All major OpenAI encodings** — `cl100k_base`, `o200k_base`, `r50k_base`, `p50k_base`, `p50k_edit`
- **Model name aliases** — pass `'gpt-4'` or `'gpt-3.5-turbo'` directly, no need to remember encoding names
- **Special token support** — correctly handles `<|endoftext|>` and similar control tokens
- **100× faster than pg_tiktoken** — encoder cached per-process in `TopMemoryContext`; BPE loop avoids heap allocation; open-addressing hash table fits L2/L3 cache

## Supported Encodings & Model Aliases

| Encoding | Model aliases |
|---|---|
| `cl100k_base` | `gpt-4`, `gpt-4-*`, `gpt-3.5-turbo`, `gpt-3.5-turbo-*`, `text-embedding-ada-002`, `text-embedding-3-small`, `text-embedding-3-large` |
| `o200k_base` | `gpt-4o`, `gpt-4o-*`, `o1`, `o1-*`, `o3`, `o3-*` |
| `r50k_base` | `gpt2`, `davinci` |
| `p50k_base` | `text-davinci-002`, `text-davinci-003`, `code-davinci-001`, `code-cushman-001` |
| `p50k_edit` | `text-davinci-edit-001`, `code-davinci-edit-001` |

## Quick Start with RelytOne (Recommended)

The easiest way to use `pg_tiktoken_c` is through [RelytOne](https://relytone.data.cloud/auth/signin), a fully managed PostgreSQL cloud service with this extension pre-installed.

### 1. Create an instance

Go to [https://relytone.data.cloud/auth/signin](https://relytone.data.cloud/auth/signin) and sign in. Then:

1. Click **New Instance** in the dashboard
2. Choose an instance type (the extension is available on all tiers)
3. Wait ~30 seconds for provisioning
4. Copy the connection string from the instance details page

### 2. Connect and enable the extension

```sql
CREATE EXTENSION IF NOT EXISTS pg_tiktoken_c;
```

That's it — the vocab data files are pre-loaded and no extra setup is needed.

## Self-Hosted Installation

### Prerequisites

| Requirement | Version |
|---|---|
| PostgreSQL | 13 – 17 |
| PCRE2 | ≥ 10.30 |
| C compiler | GCC / Clang |

Install PCRE2:

```bash
# macOS (Homebrew)
brew install pcre2

# Ubuntu / Debian
sudo apt-get install libpcre2-dev

# RHEL / Rocky
sudo dnf install pcre2-devel
```

### Build & Install

```bash
# Clone the repository
git clone https://github.com/relytcloud/pg_tiktoken_c.git
cd pg_tiktoken_c

# Build and install
make
sudo make install

# Download vocab data files (required once after install)
sudo make download-data
```

The `download-data` target fetches the BPE vocabulary files from `openaipublic.blob.core.windows.net` and installs them into PostgreSQL's `sharedir`. Requires internet access; files total ~10 MB.

### Enable the Extension

```sql
CREATE EXTENSION pg_tiktoken_c;
```

## SQL Reference

### `tiktoken_count(encoding_selector, content)`

Returns the number of tokens in `content` for the given encoding.

```
tiktoken_count(encoding_selector text, content text) → bigint
```

### `tiktoken_encode(encoding_selector, content)`

Returns an array of integer token IDs.

```
tiktoken_encode(encoding_selector text, content text) → bigint[]
```

### `chunk_text_table(input_text, chunk_size [, chunk_overlap [, encoding]])`

Table-valued version of `chunk_text()`. Returns one row per chunk — more ergonomic for SQL queries that need to join, filter, or aggregate.

```
chunk_text_table(
    input_text    text,
    chunk_size    int,
    chunk_overlap int  DEFAULT 0,
    encoding      text DEFAULT 'cl100k_base'
) → TABLE (chunk_index int, chunk text, token_count bigint)
```

| Column | Description |
|---|---|
| `chunk_index` | 0-based position of the chunk |
| `chunk` | Chunk text |
| `token_count` | Exact token count of this chunk |

### `chunk_text(input_text, chunk_size [, chunk_overlap [, encoding]])`

Splits `input_text` into an array of text chunks, each containing at most `chunk_size` tokens. Consecutive chunks share `chunk_overlap` tokens of context.

```
chunk_text(
    input_text    text,
    chunk_size    int,
    chunk_overlap int  DEFAULT 0,
    encoding      text DEFAULT 'cl100k_base'
) → text[]
```

| Parameter | Description |
|---|---|
| `input_text` | Text to split. Returns `'{}'` for `NULL` or empty string. |
| `chunk_size` | Maximum tokens per chunk. Must be `> 0`. |
| `chunk_overlap` | Tokens shared between adjacent chunks. Must satisfy `0 ≤ overlap < chunk_size`. |
| `encoding` | Tiktoken encoding name or model alias. |

All three functions are `IMMUTABLE PARALLEL SAFE` and can be used in indexes, generated columns, and parallel queries.

## Usage Examples

### Count tokens before calling the API

```sql
-- Check token budget for GPT-4o
SELECT tiktoken_count('gpt-4o', prompt_text) AS token_count
FROM   my_prompts
WHERE  tiktoken_count('gpt-4o', prompt_text) <= 128000;
```

### Truncate text to a token limit

```sql
-- Keep chunks under 512 tokens for embedding
WITH encoded AS (
    SELECT id,
           body,
           tiktoken_encode('text-embedding-3-small', body) AS tokens
    FROM   documents
)
SELECT id,
       array_length(tokens, 1)                        AS token_count,
       tokens[1:512]                                  AS truncated_tokens
FROM   encoded;
```

### Store token counts as a generated column

```sql
ALTER TABLE articles
    ADD COLUMN token_count bigint
    GENERATED ALWAYS AS (tiktoken_count('cl100k_base', body)) STORED;

-- Query by token range
SELECT title, token_count
FROM   articles
WHERE  token_count BETWEEN 100 AND 500
ORDER  BY token_count;
```

### Special tokens

```sql
SELECT tiktoken_encode('cl100k_base', 'hello<|endoftext|>world');
-- {15339,100257,14957}
```

### Inspect individual token IDs

```sql
SELECT token_id,
       row_number() OVER () - 1 AS position
FROM   unnest(tiktoken_encode('cl100k_base', 'Hello world!')) AS t(token_id);
--  token_id | position
-- ----------+----------
--      9906 |        0
--      1917 |        1
--         0 |        2
```

### Use the table function for direct SQL queries

```sql
-- Returns rows: chunk_index, chunk, token_count
SELECT * FROM chunk_text_table(document_body, 512, 64);

-- Filter: only chunks with more than 50 tokens
SELECT chunk_index, token_count, chunk
FROM   chunk_text_table(document_body, 512, 64)
WHERE  token_count > 50;

-- Bulk processing: expand every document in a table
SELECT d.id       AS doc_id,
       ct.chunk_index,
       ct.token_count,
       ct.chunk
FROM   documents d,
       chunk_text_table(d.body, 512, 64) ct
ORDER  BY d.id, ct.chunk_index;
```

### Split a document into chunks for RAG indexing

```sql
-- Non-overlapping 512-token chunks (typical embedding pipeline)
SELECT chunk_index,
       chunk
FROM   unnest(
           chunk_text(document_body, 512, 0, 'text-embedding-3-small')
       ) WITH ORDINALITY AS t(chunk, chunk_index)
WHERE  document_id = 42;
```

### Sliding window with overlap (better context continuity)

```sql
-- 512-token chunks with 64-token overlap
SELECT chunk_index,
       tiktoken_count('cl100k_base', chunk) AS tokens,
       chunk
FROM   unnest(
           chunk_text(document_body, 512, 64)
       ) WITH ORDINALITY AS t(chunk, chunk_index);
```

### Bulk chunking across an entire table

```sql
-- Expand every document into its chunks in one query
SELECT d.id        AS doc_id,
       t.chunk_idx AS chunk_index,
       t.chunk     AS chunk_text
FROM   documents d,
       unnest(chunk_text(d.body, 512, 64)) WITH ORDINALITY AS t(chunk, chunk_idx)
ORDER  BY d.id, t.chunk_idx;
```

### Verify all chunks are within the token limit

```sql
SELECT bool_and(tiktoken_count('cl100k_base', chunk) <= 512) AS all_ok
FROM   unnest(chunk_text(my_text, 512, 64)) AS chunk;
```

### Quick reference for common encodings

```sql
SELECT tiktoken_count('cl100k_base',   'Hello world!') AS cl100k,
       tiktoken_count('o200k_base',    'Hello world!') AS o200k,
       tiktoken_count('r50k_base',     'Hello world!') AS r50k,
       tiktoken_count('p50k_base',     'Hello world!') AS p50k;
```

## Running Tests

```bash
make installcheck
```

## Performance

### vs. pg_tiktoken (Rust)

Benchmark run on macOS arm64 (Apple M-series), PostgreSQL 17, single connection, 10 s measurement window + 2 s warmup.  Each cell shows throughput (rows/sec) and p50 latency (µs).

**`tiktoken_count` — `cl100k_base` encoding**

| Text size | pg_tiktoken_c (C) rows/s | pg_tiktoken_c p50 µs | pg_tiktoken (Rust) rows/s | pg_tiktoken p50 µs | Speedup |
|---|---|---|---|---|---|
| short  (~3 tok)   | 11,061 | 86   | 4 | 222,392 | **2,765×** |
| medium (~60 tok)  |  6,779 | 141  | 4 | 222,801 | **1,695×** |
| long   (~500 tok) |  1,202 | 810  | 4 | 226,298 |   **301×** |
| vlong  (~2000 tok)|     27 | 34,742 | 4 | 257,240 |     **7×** |

**`tiktoken_encode` — `cl100k_base` encoding**

| Text size | pg_tiktoken_c (C) rows/s | pg_tiktoken_c p50 µs | pg_tiktoken (Rust) rows/s | pg_tiktoken p50 µs | Speedup |
|---|---|---|---|---|---|
| short  (~3 tok)   | 10,652 | 88   | 4 | 222,342 | **2,663×** |
| medium (~60 tok)  |  6,008 | 159  | 4 | 223,440 | **1,502×** |
| long   (~500 tok) |  1,047 | 942  | 4 | 225,593 |   **262×** |
| vlong  (~2000 tok)|     28 | 35,426 | 4 | 256,597 |     **7×** |

**Why the gap?**  `pg_tiktoken` (Rust/pgrx) re-initialises the BPE encoder on every function call, spending ~220 ms per call just on setup regardless of input size.  `pg_tiktoken_c` loads the encoder once per backend and caches it in `TopMemoryContext` for the lifetime of the connection; subsequent calls pay only the actual tokenisation cost (< 1 ms for typical prompts).

> Reproduce:  `bench/bench_tiktoken -h localhost -p 5432 -d <db> -e cl100k_base -f count -t 10`

## Performance Notes

- The encoder (vocab hash table + PCRE2 pattern) is loaded once per PostgreSQL backend and cached for the lifetime of the connection — repeated calls within the same session pay no reload cost.
- The BPE merge loop avoids heap allocation for typical token lengths (< 2 KB word pieces).
- The hash table uses open-addressing with FNV-1a, sized to fit in L2/L3 cache.
- For bulk token counting over large tables, consider using a parallel query or `PARALLEL SAFE` aggregate.

## License

Apache 2.0. See [LICENSE](LICENSE).
